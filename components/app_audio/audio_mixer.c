/*
 * audio_mixer.c —— 混音引擎实现
 *
 * 关键实现决策:
 *   1. 【Q15 定点混音】10ms 帧(480帧×2ch), int32 累加 + 饱和, 双核320MHz下 <1ms;
 *   2. 【阻塞式输出为节拍】drv_audio_output 在 DMA 环满时阻塞, 混音循环自然
 *      与 I2S 消耗速率同步, 无需额外定时器(避免双时钟漂移);
 *   3. 【ducking 渐变】每个 duckable 源维护 Q15 增益, 每 tick 向目标线性逼近,
 *      300ms 降/500ms 升, 避免跳变爆音;
 *   4. 【AEC 参考在 master gain 之后取】保证与扬声器实际输出一致。
 */
#include "audio_mixer.h"

#include <string.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "drv_audio.h"
#include "cubecore.h"
#include "esp_heap_caps.h"

static const char *TAG = "mixer";

/* ---- 常量(与设计文档 §3.1 契约一致) ---- */
#define MIX_FRAME_MS            10
#define MIX_DUCK_DOWN_TICKS     30      /* 30×10ms = 300ms 下降 */
#define MIX_DUCK_UP_TICKS       50      /* 50×10ms = 500ms 恢复 */
#define MIX_DUCK_GAIN_Q15       6553    /* -14dB ≈ 0.2 */
#define MIX_HEALTH_PERIOD_TICKS 1000    /* 健康心跳周期(×10ms = 10s) */

/* ---- 音源对象 ---- */
struct audio_source {
    char name[8];
    audio_src_type_t type;
    bool duckable;
    bool blocking;           /* 生产者是否可阻塞(见 audio_source_cfg_t) */
    size_t prefill_bytes;    /* 预填充门限 */
    bool gating;             /* true=未攒够预填充, 混音器暂不消耗 */
    bool gating_full;        /* true=使用完整预填充门限(首次), 欠载恢复用低水位 */
    bool active;
    cube_ringbuf_handle_t rb;
    int32_t gain_q15;               /* 当前增益(渐变中) */
    int32_t target_gain_q15;        /* 目标增益 */
    audio_source_handle_t next;
};

/* ---- 引擎状态 ---- */
static struct {
    audio_mixer_cfg_t cfg;
    audio_source_handle_t sources;  /* 单链表 */
    TaskHandle_t task;
    volatile bool running;
    int master_q15;
    int master_percent;
    bool ducking;
    int32_t duck_cur_q15;           /* ducking 当前增益 Q15(向目标渐近) */
    audio_ref_sink_fn ref_sink;
    void *ref_sink_ctx;
    /* 频谱抽头(双缓冲指针交换) */
    int16_t *spectrum_buf[2];
    volatile int spectrum_idx;
} s_mixer;

/* 音源链表保护: add_source(生产侧)与混音任务遍历跨核并发。
 * 节点创建后永不释放(无 remove 接口, 见 audio_mixer.h), 因此遍历侧只需
 * 对【链表头快照】加临界区即可安全; 节点内部字段由各自使用方串行访问。 */
static portMUX_TYPE s_list_mux = portMUX_INITIALIZER_UNLOCKED;

/* 10ms 工作缓冲(内部 SRAM, 每 tick 复用) */
static int16_t s_tmp[MIX_FRAME_MS * 48 * 2];    /* 帧读入 */
static int16_t s_sum[MIX_FRAME_MS * 48 * 2];    /* 求和输出 */
static int16_t s_master[MIX_FRAME_MS * 48 * 2]; /* master gain 后 */

static inline int32_t gain_apply(int32_t sample, int32_t g_q15)
{
    return (sample * g_q15) >> 15;
}

static inline int16_t sat16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int32_t percent_to_q15(int percent)
{
    /* 感知音量曲线(平方): 50% 听感约为半响 */
    int32_t p = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return (p * p * 32767) / (100 * 100);
}

/* ================= 音源管理 ================= */
audio_source_handle_t audio_mixer_add_source(const audio_source_cfg_t *cfg)
{
    struct audio_source *s = calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    strncpy(s->name, cfg->name ? cfg->name : "?", sizeof(s->name) - 1);
    s->type = cfg->type;
    s->blocking = cfg->blocking;
    s->prefill_bytes = (size_t)s_mixer.cfg.sample_rate * s_mixer.cfg.channels *
                       sizeof(int16_t) * (cfg->prefill_ms > 0 ? cfg->prefill_ms : 0) / 1000;
    s->gating = (s->prefill_bytes > 0);
    s->gating_full = true;
    s->duckable = (cfg->type == AUDIO_SRC_BT || cfg->type == AUDIO_SRC_NET);
    s->active = false;                      /* 默认不激活, 由使用方开启 */
    s->gain_q15 = s->target_gain_q15 = 32767;
    s->rb = cube_ringbuf_create((size_t)s_mixer.cfg.sample_rate *
                                s_mixer.cfg.channels * sizeof(int16_t) *
                                (cfg->buffer_ms > 0 ? cfg->buffer_ms : 200) / 1000);
    if (s->rb == NULL) {
        free(s);
        return NULL;
    }
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
    ESP_LOGI("mixer", "add_source[%s]: blocking=%d prefill_ms=%d buffer_ms=%d",
             s->name, s->blocking, (int)(s->prefill_bytes * 1000 /
             ((size_t)s_mixer.cfg.sample_rate * s_mixer.cfg.channels * 2)),
             cfg->buffer_ms);
#endif
    /* 原子挂入链表(混音任务可能正在其他核遍历, 见 s_list_mux 注释) */
    portENTER_CRITICAL(&s_list_mux);
    s->next = s_mixer.sources;
    s_mixer.sources = s;
    portEXIT_CRITICAL(&s_list_mux);
    return s;
}

size_t audio_mixer_source_write(audio_source_handle_t src,
                                const void *pcm, size_t bytes, TickType_t wait)
{
    if (src == NULL || pcm == NULL) return 0;
    (void)wait;
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = bytes;
    while (left > 0) {
        size_t w = cube_ringbuf_write(src->rb, p, left);
        p += w;
        left -= w;
        if (left == 0) break;
        if (!src->blocking) {
            break;   /* 不可阻塞生产者(BT): 丢弃剩余, 保护已缓冲音频 */
        }
        /* 阻塞式生产者(网络流/TTS): 按混音实时消耗节奏推进, 丢弃=破音 */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return bytes - left;
}

void audio_mixer_set_source_active(audio_source_handle_t src, bool active)
{
    if (src == NULL) return;
    src->active = active;
    if (!active) {
        cube_ringbuf_reset(src->rb);        /* 关闭时清残留, 避免旧数据突播 */
    }
    src->gating = (src->prefill_bytes > 0); /* 重新激活需再次预填充 */
    src->gating_full = true;
}

/* ================= 混音主循环 ================= */
static uint32_t s_tick_count;   /* 混音节拍累计(应≈100/s, 用于验证输出时钟) */

static void mixer_tick(void)
{
    s_tick_count++;
    const int frames = s_mixer.cfg.sample_rate / 100;   /* 10ms */
    const size_t frame_bytes = (size_t)frames * s_mixer.cfg.channels * sizeof(int16_t);

    memset(s_sum, 0, sizeof(s_sum));

    /* ---- ducking 增益渐变状态机 ----
     * 当前增益向目标渐近: ducking时目标=-14dB(300ms内压低), 结束后500ms内恢复。
     * (旧实现恢复斜率反置, 稳态增益卡死在0.2, 整机音量只剩1/5 —— 实测bug) */
    int32_t target = s_mixer.ducking ? MIX_DUCK_GAIN_Q15 : 32767;
    int32_t cur = s_mixer.duck_cur_q15;
    if (cur < target) {
        cur += (32767 - MIX_DUCK_GAIN_Q15) / MIX_DUCK_UP_TICKS;      /* 500ms恢复 */
        if (cur > target) cur = target;
    } else if (cur > target) {
        cur -= (32767 - MIX_DUCK_GAIN_Q15) / MIX_DUCK_DOWN_TICKS;   /* 300ms压低 */
        if (cur < target) cur = target;
    }
    s_mixer.duck_cur_q15 = cur;
    int32_t duck_q15 = cur;

    /* ---- 遍历音源: 拉取 → 增益 → 累加(快照遍历, 节点永不释放故安全) ---- */
    portENTER_CRITICAL(&s_list_mux);
    struct audio_source *snap = s_mixer.sources;
    portEXIT_CRITICAL(&s_list_mux);
    for (struct audio_source *s = snap; s != NULL; s = s->next) {
        if (!s->active) continue;
        size_t gate_need = s->gating_full ? s->prefill_bytes
                           : (s->prefill_bytes / 4 > frame_bytes * 2 ? s->prefill_bytes / 4
                                                                     : frame_bytes * 2);
        if (s->gating) {
            /* 预填充门: 首次攒满 prefill; 欠载后只需 1/4 即可快速恢复 */
            if (cube_ringbuf_available(s->rb) >= gate_need) {
                s->gating = false;
                s->gating_full = false;
            } else {
                continue;
            }
        }
        size_t got = cube_ringbuf_read(s->rb, s_tmp, frame_bytes);
        if (got < frame_bytes) {
            /* 欠载: 本 tick 该源静默, 以低水位重新预填充(自愈抖动缓冲) */
            if (s->prefill_bytes > 0) {
                s->gating = true;
                s->gating_full = false;
            }
            continue;
        }
        int32_t g = s->duckable ? duck_q15 : 32767;
        const int total = frames * s_mixer.cfg.channels;
        for (int i = 0; i < total; i++) {
            s_sum[i] = sat16((int32_t)s_sum[i] + gain_apply(s_tmp[i], g));
        }
    }

    /* ---- master gain ---- */
    int32_t mg = s_mixer.master_q15;
    const int total = frames * s_mixer.cfg.channels;
    for (int i = 0; i < total; i++) {
        s_master[i] = sat16(gain_apply(s_sum[i], mg));
    }

    /* ---- 观测抽头(在输出前拷贝, 参考与扬声器同源) ---- */
    if (s_mixer.ref_sink) {
        s_mixer.ref_sink(s_master, frames, s_mixer.ref_sink_ctx);
    }
    s_mixer.spectrum_idx ^= 1;
    memcpy(s_mixer.spectrum_buf[s_mixer.spectrum_idx], s_master, frame_bytes);

#if CONFIG_ESP_CUBE_AUDIO_DEBUG
    /* ---- 每秒遥测: 输出峰值幅度(数字侧是否静音) + 各音源缓冲水位 ---- */
    {
        static int dbg_cnt;
        if (++dbg_cnt >= 100) {
            dbg_cnt = 0;
            int16_t peak = 0;
            for (int i = 0; i < frames * s_mixer.cfg.channels; i++) {
                int16_t a = s_master[i] < 0 ? (int16_t)-s_master[i] : s_master[i];
                if (a > peak) peak = a;
            }
            ESP_LOGI(TAG, "dbg: out_peak=%d vol=%d%% ticks=%u", peak,
                     s_mixer.master_percent, s_tick_count);
            for (struct audio_source *s = snap; s != NULL; s = s->next) {
                unsigned buf_ms = (unsigned)(cube_ringbuf_available(s->rb) * 1000 /
                        ((size_t)s_mixer.cfg.sample_rate * s_mixer.cfg.channels * 2));
                ESP_LOGI(TAG, "  src[%s] active=%d buf=%ums%s",
                         s->name, s->active, buf_ms, s->gating ? " (filling)" : "");
            }
        }
    }
#endif

    /* ---- 健康心跳(10秒一次, 非门控): 运行状态低频可观测 ---- */
    {
        static int hb_cnt;
        if (++hb_cnt >= MIX_HEALTH_PERIOD_TICKS) {
            hb_cnt = 0;
            int16_t peak = 0;
            for (int i = 0; i < frames * s_mixer.cfg.channels; i++) {
                int16_t a = s_master[i] < 0 ? (int16_t)-s_master[i] : s_master[i];
                if (a > peak) peak = a;
            }
            unsigned net_ms = 0;
            for (struct audio_source *s = snap; s != NULL; s = s->next) {
                if (s->type == AUDIO_SRC_NET) {
                    net_ms = (unsigned)(cube_ringbuf_available(s->rb) * 1000 /
                            ((size_t)s_mixer.cfg.sample_rate * s_mixer.cfg.channels * 2));
                }
            }
            ESP_LOGI(TAG, "health: peak=%d net_buf=%ums vol=%d%%", peak, net_ms,
                     s_mixer.master_percent);
        }
    }

    /* ---- 输出(阻塞=节拍) ---- */
    int written = drv_audio_output(s_master, frames);
    if (written != frames) {
        static int err_cnt;
        if ((err_cnt++ % 100) == 0) {
            ESP_LOGW(TAG, "output underrun");
        }
    }
}

static void mixer_task(void *arg)
{
    ESP_LOGI(TAG, "mixer task running on core%d @%dHz", xPortGetCoreID(), s_mixer.cfg.sample_rate);
    while (s_mixer.running) {
        mixer_tick();
    }
    vTaskDelete(NULL);
}

/* ================= 生命周期 ================= */
esp_err_t audio_mixer_init(const audio_mixer_cfg_t *cfg)
{
    memset(&s_mixer, 0, sizeof(s_mixer));
    s_mixer.cfg = *cfg;
    s_mixer.master_percent = 60;
    s_mixer.master_q15 = percent_to_q15(60);
    s_mixer.duck_cur_q15 = 32767;   /* ducking 当前增益: 初始=满增益 */
    for (int i = 0; i < 2; i++) {
        /* 内部 RAM: LVGL 任务 30fps 高频读取, 且仅 ~2KB×2 */
        s_mixer.spectrum_buf[i] = heap_caps_malloc(
            (size_t)(cfg->sample_rate / 100) * cfg->channels * sizeof(int16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_mixer.spectrum_buf[i] == NULL) {
            heap_caps_free(s_mixer.spectrum_buf[0]);   /* 部分失败整体回收 */
            s_mixer.spectrum_buf[0] = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t audio_mixer_start(void)
{
    if (s_mixer.task != NULL) return ESP_OK;
    s_mixer.running = true;
    BaseType_t r = xTaskCreatePinnedToCore(mixer_task, CUBE_TASK_MIXER_NAME,
                                           CUBE_TASK_MIXER_STACK, NULL,
                                           CUBE_TASK_MIXER_PRIO,
                                           &s_mixer.task, CUBE_TASK_MIXER_CORE);
    return r == pdPASS ? ESP_OK : ESP_FAIL;
}

void audio_mixer_stop(void)
{
    s_mixer.running = false;
    s_mixer.task = NULL;    /* 任务自行退出 */
}

void audio_mixer_set_master_volume(int percent)
{
    s_mixer.master_percent = percent;
    s_mixer.master_q15 = percent_to_q15(percent);
    drv_audio_set_volume(100);  /* codec 模拟级全开, 数字级统一控制 */
    cube_event_publish(CUBE_EVT_VOLUME_CHANGED, &s_mixer.master_percent, sizeof(int));
}

int audio_mixer_get_master_volume(void)
{
    return s_mixer.master_percent;
}

void audio_mixer_set_ducking(bool ai_talking)
{
    s_mixer.ducking = ai_talking;
}

bool audio_mixer_take_spectrum(int16_t *out, int frames)
{
    int max_frames = s_mixer.cfg.sample_rate / 100;
    if (frames > max_frames) frames = max_frames;
    /* 双检防撕裂: 写侧"翻转索引→memcpy"期间读到的是正在写的块;
     * 拷贝后核对索引未变, 变了则重拷一次(旧块此时已稳定)。 */
    for (int retry = 0; retry < 2; retry++) {
        int idx = s_mixer.spectrum_idx;
        memcpy(out, s_mixer.spectrum_buf[idx],
               (size_t)frames * s_mixer.cfg.channels * sizeof(int16_t));
        if (s_mixer.spectrum_idx == idx) return true;
    }
    return true;                        /* 重试仍翻转: 接受最新块(频谱抖动无害) */
}

void audio_mixer_set_ref_sink(audio_ref_sink_fn fn, void *user_ctx)
{
    s_mixer.ref_sink = fn;
    s_mixer.ref_sink_ctx = user_ctx;
}
