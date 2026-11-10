/*
 * app_player.c —— ICY/HTTP 网络流播放器实现
 *
 * 管线: esp_http_client(流式读) → esp_audio_codec(MP3/AAC 解码)
 *       → cube_resampler(→48k) → audio_mixer(AUDIO_SRC_NET 通道)
 *
 * 关键点:
 *   1. ICY 元数据: 请求头带 "Icy-MetaData: 1";IDF 的 esp_http_client 不提供
 *      响应头查询, 通过 HTTP_EVENT_ON_HEADER 事件捕获 icy-metaint 与
 *      Content-Type(大小写不敏感按协议头原样上报);
 *   2. 解码循环遵循 esp_audio_codec 语义: raw.consumed 推进输入,
 *      out.needed_size 提示输出缓冲不足(本实现缓冲 2048 帧已覆盖 MP3/AAC 最大帧);
 *   3. 断流自动重连(线性退避, ≤5 次)。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE              /* strcasestr(GNU 扩展, 头部嗅探用) */
#endif

#include "mp3dec.h"             /* libhelix-mp3 定点解码(chmorgan/esp-libhelix-mp3)。
                                 * 选型依据(GitHub 调研 2026-11-09):
                                 * - esp_audio_codec 的 MP3 即闭源 minimp3 派生,
                                 *   S31 实测 23ms/帧(仅1.1x实时, 必卡顿)且吞帧66%;
                                 * - S31 USE_ASM 汇编库不覆盖 MP3(esp-adf-libs #85);
                                 * - minimp3 上游存在未初始化 scratch 进 Huffman(#141)、
                                 *   嵌入式栈过大静默崩溃(#117) 等已知问题;
                                 * - libhelix 定点实现, 社区实测 ESP32 44.1k/2ch 实时无压力 */
#include "app_player.h"
#include "app_net.h"       /* BT 释放后等 WiFi IP 再恢复电台 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <esp_timer.h>
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_types.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "icy_stream.h"
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "esp_hls_extractor.h"
#include "esp_hls_helper.h"
#include "esp_aac_dec.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_io_file.h"

static const char *TAG = "player";

#define PLAYER_MAX_RETRIES      5       /* 单次启播最大重连次数 */
#define PLAYER_RETRY_STEP_MS    1000    /* 重连退避步进(线性: step×次数) */
#define PLAYER_TASK_STACK       CUBE_TASK_PLAYER_STACK  /* 栈(PSRAM 静态分配); 参数见 cube_tasks.h */

#define FEED_BUF_SIZE    4096                    /* 网络读块 */
#define PCM_BUF_FRAMES   2048                    /* 解码输出缓冲(帧, 覆盖MP3/AAC最大帧) */

static struct {
    char url[512];
    int  playlist_idx;                          /* 当前播放列表下标(-1=单次指定) */
    bool last_read_error;                       /* 上次结束是否为错误(n<0) */
    app_player_state_t state;
    TaskHandle_t task;
    volatile bool stop_req;
    volatile bool bt_pause;                     /* BT 音乐接管: 挂起电台供流 */
    audio_source_handle_t src;
    /* HTTP 响应头捕获(HTTP_EVENT_ON_HEADER 上下文写入) */
    char content_type[64];
    int  icy_metaint;
} s_player;

/* ================= HTTP 头捕获 ================= */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            strncpy(s_player.content_type, evt->header_value,
                    sizeof(s_player.content_type) - 1);
        } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_player.icy_metaint = atoi(evt->header_value);
        }
    }
    return ESP_OK;
}

/* ================= BT 音乐接管(电台让路) =================
 * 手机蓝牙放歌时电台挂起, A2DP 断开后原地恢复(不拆任务/不换台)。
 * 语义: 以"真实音频数据流入"为准(仅配对不触发), 以断开为恢复时机。
 * 订阅回调可能在 BT 栈任务上下文执行 → 只做 volatile 写, 所有实际
 * 动作(停读/关源/恢复)都由播放任务在 bt_pause_gate() 内完成。
 */
static void player_evt_cb(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    (void)ctx;
    if (id != CUBE_EVT_BT_AUDIO_ACTIVE || data == NULL || len != sizeof(bool)) {
        return;                             /* 事件总线全量派发: 必须按 ID 过滤 */
    }
    if (s_player.state == PLAYER_IDLE) {
        return;                             /* 电台未运行: 无可让路, 也不登记暂停 */
    }
    bool active = *(const bool *)data;
    if (active && !s_player.bt_pause) {
        ESP_LOGI(TAG, "BT audio active -> radio suspend");
    }
    s_player.bt_pause = active;
}

esp_err_t app_player_init(void)
{
    static bool inited;
    if (inited) {
        return ESP_OK;
    }
    inited = true;
    return cube_event_subscribe(player_evt_cb, NULL);
}

/* 播放任务内调用: BT 暂停期间在此驻留(不读网络→TCP背压, 不供混音器)。
 * 恢复时重开源通道并返回; 暂停期间连接被服务器断开属预期(直播服务器
 * 踢空闲连接), 由外层循环以"零失败"重开会话。 */
static void bt_pause_gate(void)
{
    if (!s_player.bt_pause || s_player.src == NULL) {
        return;
    }
    audio_mixer_set_source_active(s_player.src, false);
    bool held = false;
    while (s_player.bt_pause && !s_player.stop_req) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held = true;
    }
    if (held && !s_player.stop_req) {
        /* BT 模式期间 WiFi 已被 app_net 停掉(BT 独占射频): 先等 IP
         * 再重开会话, 否则首个连接烧掉宝贵的重试次数 */
        if (app_net_wait_up(10000) != ESP_OK) {
            ESP_LOGW(TAG, "wifi not up after BT release, retry loop will keep trying");
        }
        audio_mixer_set_source_active(s_player.src, true);
        ESP_LOGI(TAG, "radio resumed (BT released)");
    }
}
typedef struct {
    esp_audio_dec_handle_t dec;
    esp_audio_dec_info_t info;
    bool info_valid;
    cube_resampler_t rs;
    bool resample_needed;
    bool is_mp3;            /* MP3: libhelix 定点解码路径 */
    bool is_raw;            /* RAW PCM 直通(无解码) */
    HMP3Decoder mp3;        /* libhelix 解码器句柄 */
    int  frame_cnt;
    size_t total_decoded;   /* 解码器累计输出字节 */
} decoder_t;

/* 按 Content-Type/URL 选择解码器类型 */
static esp_audio_type_t pick_decoder_type(const char *content_type, const char *url)
{
    if (content_type[0]) {
        if (strcasestr(content_type, "aac") || strcasestr(content_type, "aacp")) {
            return ESP_AUDIO_TYPE_AAC;
        }
        if (strcasestr(content_type, "mpeg") || strcasestr(content_type, "mp3")) {
            return ESP_AUDIO_TYPE_MP3;
        }
    }
    /* URL 兜底嗅探 */
    if (strstr(url, ".aac") || strstr(url, "-aac")) return ESP_AUDIO_TYPE_AAC;
    return ESP_AUDIO_TYPE_MP3;                      /* 电台主流为 MP3 */
}

/* 打开解码器: MP3 → libhelix; 其余(AAC等) → esp_audio_codec
 * codec_cfg/cfg_sz: codec 专用配置(如 AAC 裸帧 esp_aac_dec_cfg_t), 可为 NULL */
static esp_err_t decoder_open(decoder_t *d, esp_audio_type_t type,
                              void *codec_cfg, uint32_t codec_cfg_sz)
{
    memset(d, 0, sizeof(*d));
    d->is_mp3 = (type == ESP_AUDIO_TYPE_MP3);
    if (d->is_mp3) {
        d->mp3 = MP3InitDecoder();
        ESP_RETURN_ON_FALSE(d->mp3 != NULL, ESP_ERR_NO_MEM, TAG, "MP3InitDecoder");
        return ESP_OK;
    }
    esp_audio_dec_cfg_t cfg = {
        .type = type,
        .cfg = codec_cfg,
        .cfg_sz = codec_cfg_sz,
    };
    ESP_RETURN_ON_FALSE(esp_audio_dec_open(&cfg, &d->dec) == ESP_AUDIO_ERR_OK,
                        ESP_FAIL, TAG, "dec open failed (type=%d)", type);
    return ESP_OK;
}

/* ================= MP3 帧边界工具 ================= */
/* MPEG1 Layer3 帧长 = 144*bitrate/samplerate + padding;本项目的源(电台/文件)均为此格式 */
static int mp3_frame_len(const uint8_t *h)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return -1;
    int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    static const int brs[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
    static const int srs[4]  = { 44100,48000,32000,0 };
    int bi = (h[2] >> 4) & 0xF, si = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (ver != 3 || layer != 1 || bi == 0 || bi == 15 || si == 3) return -1;
    return 144 * brs[bi] * 1000 / srs[si] + pad;
}

/* 解码原始块 → (重采样到混音主时钟) → 推入混音器
 * pending 缓冲: 网络读取块尾部的"半帧"数据必须保留到下一块, 否则解码器
 * 流错位 → 周期性 "Not supported format" 与杂音(实测问题) */
static uint32_t s_pending_drops;    /* pending 溢出丢弃计数(数据损失可见化) */
static uint32_t s_aac_fail_streak;  /* AAC解码连续失败计数(HE-AAC SBR自动重试用) */
static bool     s_aac_sbr_retry_done;
static int64_t s_dec_us_total;      /* 解码耗时累计(诊断: esp解码器疑似~40ms/帧) */
static uint32_t s_dec_calls;
static int64_t s_dec_us_max;
static uint8_t s_pending[FEED_BUF_SIZE * 2];
static size_t  s_pending_len;
static int16_t s_res_out[PCM_BUF_FRAMES * 2];   /* 重采样独立输出(原地升采样会自覆盖) */

/* 解码输出 → (重采样到主时钟) → 写入混音器 */
static void output_pcm(decoder_t *d, int16_t *pcm, size_t frames)
{
    if (frames == 0) return;
    if (d->resample_needed) {
        if (d->info.channel == 1) {
            frames = cube_resampler_process_mono_to_stereo(
                    &d->rs, pcm, frames, s_res_out, PCM_BUF_FRAMES);
        } else {
            frames = cube_resampler_process_stereo(
                    &d->rs, pcm, frames, s_res_out, PCM_BUF_FRAMES);
        }
        audio_mixer_source_write(s_player.src, s_res_out,
                                 frames * 2 * sizeof(int16_t), 0);
    } else {
        audio_mixer_source_write(s_player.src, pcm,
                                 frames * 2 * sizeof(int16_t), 0);
    }
}


static void helix_feed(decoder_t *d);
static void decoder_feed(void *ctx, const void *data, size_t len);

/* icy_audio_sink_fn(ctx-last) → decoder_feed(ctx-first) 形参顺序适配 */
static void decoder_feed_sink(const void *data, size_t len, void *ctx)
{
    decoder_feed(ctx, data, len);
}

static void decoder_feed(void *ctx, const void *data, size_t len)
{
    decoder_t *d = (decoder_t *)ctx;
    static int16_t pcm_out[PCM_BUF_FRAMES * 2];

    size_t fed_frames_before = d->frame_cnt;
    if (len > sizeof(s_pending) - s_pending_len) {
        /* 异常积压(解码器长期不消费): 丢弃最旧数据保持流前进 */
        s_pending_drops++;
        ESP_LOGW(TAG, "pending RESET #%u, discarding %uB", (unsigned)s_pending_drops,
                 (unsigned)s_pending_len);
        s_pending_len = 0;
    }
    memcpy(s_pending + s_pending_len, data, len);
    s_pending_len += len;

    /* RAW PCM 直通: pending 即 PCM, 按帧输出, 4字节尾差保留 */
    if (d->is_raw) {
        size_t frames = s_pending_len / 4;
        if (frames > 0) {
            output_pcm(d, (int16_t *)s_pending, frames);
        }
        size_t used = frames * 4;
        if (used > 0) {
            memmove(s_pending, s_pending + used, s_pending_len - used);
            s_pending_len -= used;
        }
        return;
    }

    if (d->is_mp3) {
        /* libhelix: 自带同步字定位, 半帧返回 UNDERFLOW 且不消耗输入 */
        helix_feed(d);
        return;
    }

    /* esp_audio_codec 路径(AAC 等): 帧边界感知, 只喂到最后一个完整帧结尾 */
    size_t feed_len = s_pending_len;
    {
        size_t p = 0, last_ok = 0;
        while (p + 4 <= s_pending_len) {
            if (s_pending[p] == 0xFF && (s_pending[p + 1] & 0xE0) == 0xE0) {
                int flen = mp3_frame_len(&s_pending[p]);
                if (flen <= 0) break;
                if (p + flen <= s_pending_len) { last_ok = p + flen; p += flen; }
                else break;
            } else p++;
        }
        if (last_ok > 0) feed_len = last_ok;
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer = s_pending,
        .len = (uint32_t)feed_len,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = (uint8_t *)pcm_out,
        .len = (uint32_t)sizeof(pcm_out),
    };
    while (raw.len > 0) {
        int64_t t0 = esp_timer_get_time();
        esp_audio_err_t ret = esp_audio_dec_process(d->dec, &raw, &out);
        int64_t dt = esp_timer_get_time() - t0;
        s_dec_us_total += dt;
        s_dec_calls++;
        if (dt > s_dec_us_max) s_dec_us_max = dt;
        if (ret != ESP_AUDIO_ERR_OK) {
            s_aac_fail_streak++;                /* 连续失败计数(HLS会话用于SBR重试) */
        } else {
            s_aac_fail_streak = 0;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            /* 坏帧: 解码器未消费输入时, 按MP3同步字(0xFF Ex)扫描重同步,
             * 防止同一坏帧反复触发错误导致供流卡死 */
            if (raw.consumed == 0) {
                size_t skip = 1;
                while (skip + 1 < raw.len &&
                       !((raw.buffer[skip] == 0xFF) && (raw.buffer[skip + 1] & 0xE0) == 0xE0)) {
                    skip++;
                }
                raw.consumed = skip;
                out.decoded_size = 0;
            }
        }
        /* 关键钳制: 解码器内部缓存半帧时, consumed 可能大于本次提供的输入
         * (实测bug源), 严禁让 raw.len 下溢 */
        if (raw.consumed > raw.len) {
            raw.consumed = raw.len;
        }
        if (out.decoded_size > 0) {
            /* 首次成功解码: 记录流参数并配置重采样 */
            if (!d->info_valid) {
                esp_audio_dec_get_info(d->dec, &d->info);
                d->info_valid = true;
                cube_resampler_init(&d->rs, (float)d->info.sample_rate,
                                    (float)CONFIG_ESP_CUBE_MIXER_RATE);
                d->resample_needed = (d->info.sample_rate != (uint32_t)CONFIG_ESP_CUBE_MIXER_RATE)
                                     || (d->info.channel == 1);
                ESP_LOGI(TAG, "decoded: %luHz %dbit %dch", (unsigned long)d->info.sample_rate,
                         d->info.bits_per_sample, d->info.channel);
            }
            size_t frames = out.decoded_size /
                            (2u * (d->info.channel ? d->info.channel : 2));
            /* 前10帧逐帧幅度探针: 验证解码输出电平与位深解释是否正确 */
            d->frame_cnt++;
            d->total_decoded += out.decoded_size;
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
            if (d->frame_cnt <= 10) {
                int16_t pk = 0;
                for (size_t i = 0; i < out.decoded_size / 2; i++) {
                    int16_t a = pcm_out[i] < 0 ? (int16_t)-pcm_out[i] : pcm_out[i];
                    if (a > pk) pk = a;
                }
                ESP_LOGI(TAG, "dec frame#%d: pk=%d size=%u consumed=%u",
                         d->frame_cnt, pk, out.decoded_size, raw.consumed);
            }
#endif
            if (d->resample_needed) {
                if (d->info.channel == 1) {
                    frames = cube_resampler_process_mono_to_stereo(
                            &d->rs, pcm_out, frames, s_res_out, PCM_BUF_FRAMES);
                } else {
                    frames = cube_resampler_process_stereo(
                            &d->rs, pcm_out, frames, s_res_out, PCM_BUF_FRAMES);
                }
                audio_mixer_source_write(s_player.src, s_res_out,
                                         frames * 2 * sizeof(int16_t), 0);
            } else {
                audio_mixer_source_write(s_player.src, pcm_out,
                                         frames * 2 * sizeof(int16_t), 0);
            }
        }
        if (raw.consumed == 0 && out.decoded_size == 0) {
            break;                                  /* 剩余不足以构成一帧 */
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
    /* 保留尾部(半帧)到下一次: consumed 相对 feed_len(=s_pending_len) */
    size_t consumed_total = feed_len - raw.len;
    size_t tail = s_pending_len - consumed_total;
    if (tail > 0 && tail < s_pending_len) {
        memmove(s_pending, s_pending + consumed_total, tail);
    }
    s_pending_len = tail;
    if (d->frame_cnt == fed_frames_before && consumed_total > 1024) {
        /* 自愈: 消费了数据却0帧输出 = 解码器状态异常, 复位后从同步字重新工作 */
        ESP_LOGW(TAG, "decoder stall, reset (consumed %uB)", (unsigned)consumed_total);
        esp_audio_dec_reset(d->dec);
    }
}

/* MP3 路径: libhelix 逐帧解码。
 * 半帧处理: MP3Decode 需要完整帧在缓冲内, 不足时返回 INDATA_UNDERFLOW 且
 * 不消耗输入 —— 半帧自然留在 pending, 下次凑齐再解(无需预扫描)。 */
static void helix_feed(decoder_t *d)
{
    static int16_t mp3pcm[1152 * 2];
    size_t off = 0;
    for (;;) {
        int left = (int)(s_pending_len - off);
        if (left < 8) break;                        /* 不足一个最小帧头+数据 */

        int sync = MP3FindSyncWord(s_pending + off, left);
        if (sync < 0) {
            off = s_pending_len;                    /* 无同步字: 剩余全是垃圾 */
            break;
        }
        off += (size_t)sync;
        left = (int)(s_pending_len - off);

        uint8_t *rp = s_pending + off;
        int64_t t0 = esp_timer_get_time();
        int rc = MP3Decode(d->mp3, &rp, &left, mp3pcm, 0);
        int64_t dt = esp_timer_get_time() - t0;
        s_dec_us_total += dt;
        s_dec_calls++;
        if (dt > s_dec_us_max) s_dec_us_max = dt;
        if (rc == ERR_MP3_INDATA_UNDERFLOW || rc == ERR_MP3_MAINDATA_UNDERFLOW) {
            break;                                  /* 半帧: 保留 pending 等更多数据 */
        }
        if (rc != ERR_MP3_NONE) {
            off++;                                  /* 坏帧: 跳过此同步字重同步 */
            continue;
        }

        size_t consumed = (size_t)(rp - (s_pending + off));
        off += consumed;

        if (!d->info_valid) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(d->mp3, &fi);
            memset(&d->info, 0, sizeof(d->info));
            d->info.sample_rate = (uint32_t)fi.samprate;
            d->info.channel = (uint8_t)(fi.nChans ? fi.nChans : 1);
            d->info_valid = true;
            cube_resampler_init(&d->rs, (float)fi.samprate, (float)CONFIG_ESP_CUBE_MIXER_RATE);
            d->resample_needed = ((uint32_t)fi.samprate != (uint32_t)CONFIG_ESP_CUBE_MIXER_RATE)
                                 || (fi.nChans == 1);
            ESP_LOGI(TAG, "decoded: %dHz %dch (libhelix)", fi.samprate, fi.nChans);
        }

        d->frame_cnt++;
        d->total_decoded += consumed;
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
        if (d->frame_cnt <= 10) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(d->mp3, &fi);
            int16_t pk = 0;
            for (int i = 0; i < fi.outputSamps; i++) {
                int16_t a = mp3pcm[i] < 0 ? (int16_t)-mp3pcm[i] : mp3pcm[i];
                if (a > pk) pk = a;
            }
            ESP_LOGI(TAG, "dec frame#%d: pk=%d bytes=%u", d->frame_cnt, pk, (unsigned)consumed);
        }
#endif
        /* outputSamps = 总交错样本数(帧数 = outputSamps/channels) */
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d->mp3, &fi);
        int ch = fi.nChans ? fi.nChans : 1;
        output_pcm(d, mp3pcm, (size_t)(fi.outputSamps > 0 ? fi.outputSamps / ch : 0));
    }
    if (off > 0 && off <= s_pending_len) {
        memmove(s_pending, s_pending + off, s_pending_len - off);
        s_pending_len -= off;
    } else if (off > s_pending_len) {
        s_pending_len = 0;
    }
}

/* ================= 播放列表(原型: 局域网HTTP曲目轮播) =================
 * 最终产品由 app_ai/电台管理模块下发列表; 局域网源保证供给≥10×实时 */
/* PCM 直通素材(48k/16bit/stereo, 与混音引擎同规格, 零解码零重采样):
 * 用于验证输出链路与听感; MP3 解码器攻坚(M1.5)完成后切回压缩流 */
/* 国内真实电台源(2026-11-09 实测可达, 调研报告见 docs/03):
 * 蜻蜓/喜马拉雅 = HLS+AAC裸段(64k); CNR卫星拉流 = HLS+TS段(AAC-ADTS 202k) */
/* 国内电台播放列表(2026-11-09 三族源全部真机验证):
 * 蜻蜓FM(ls.qingting.fm, AAC裸段) / 喜马拉雅(live.xmcdn.com, AAC裸段)
 * / CNR卫星拉流(ngcdn, TS段) / 本地HLS(MP3分段, 端到端测试用) */
static const char *s_playlist[] = {
    /* [0] 默认台(M1.8 定稿, 用户听感验收"音质极佳基本无卡顿"):
     * CNR卫星拉流 202k AAC-LC TS段, 48kHz 无SBR 无重采样。
     * 蜻蜓64k HE-AAC(经典947)留作[1]对照: 源码率低, SBR高频有压缩感 */
    "http://satellitepull.cnr.cn/live/wxgdyyzs/playlist.m3u8", /* 广东音乐之声(卫星) */
    "http://ls.qingting.fm/live/267.m3u8",          /* 上海经典947(64k HE-AAC) */
    "http://live.xmcdn.com/live/74/64.m3u8",        /* 广东音乐之声FM99.3(流行) */
    "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8",  /* CNR中国之声(新闻/TS段) */
};
/* 台名与 s_playlist 一一对应(UI 显示用; 电台列表单一来源在本组件) */
static const char *s_station_names[] = {
    "广东音乐之声·卫星",
    "上海经典947",
    "广东音乐之声FM993",
    "CNR中国之声",
};
#define PLAYLIST_NUM (sizeof(s_playlist) / sizeof(s_playlist[0]))
_Static_assert(sizeof(s_station_names) / sizeof(s_station_names[0]) ==
               PLAYLIST_NUM, "station names must match playlist");

/* ================= 播放任务 ================= */
#if CONFIG_ESP_CUBE_TEST_TONE
/* 440Hz 正弦测试音: 直接生成 PCM 注入混音器, 用于验证 DAC→功放→喇叭输出链路。
 * 幅度 12000(约-8.7dBFS), 双声道 48k, 与混音引擎实时节奏阻塞同步。 */
/* 440Hz@48k: 每样本相位推进 440/48000 周期 → 50样本恰好22整周期(无漂移),
 * 用50点LUT + 整数相位步进(22), 查表代替逐样本sinf(实测sinf软浮点仅575ms/s,
 * 供流速度低于实时消耗会导致测试音本身周期性断流) */
#define TONE_LUT_N   50
#define TONE_STEP    22
static int16_t s_tone_lut[TONE_LUT_N];

static void test_tone_init(void)
{
    for (int n = 0; n < TONE_LUT_N; n++) {
        s_tone_lut[n] = (int16_t)(12000.0f * sinf(2.0f * 3.14159265f * 440.0f
                                                  * (float)n / 48000.0f));
    }
}

static void test_tone_loop(void)
{
    static int16_t tone[4800 * 2];                  /* 100ms @48k(批量写, 排除每调用开销) */
    static int idx;
    test_tone_init();
    ESP_LOGI(TAG, "TEST TONE mode: 440Hz @-8.7dBFS (Kconfig ESP_CUBE_TEST_TONE)");
    uint32_t loops = 0, partial = 0;
    int64_t last_report = esp_timer_get_time();
    for (;;) {
        for (int i = 0; i < 4800; i++) {
            int16_t v = s_tone_lut[idx];
            tone[i * 2] = v;
            tone[i * 2 + 1] = v;
            idx += TONE_STEP;
            if (idx >= TONE_LUT_N) idx -= TONE_LUT_N;
        }
        size_t w = audio_mixer_source_write(s_player.src, tone, sizeof(tone),
                                            pdMS_TO_TICKS(50));
        if (w != sizeof(tone)) partial++;
        loops++;
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 2000000) {         /* 每2秒报告生产速率 */
            ESP_LOGI(TAG, "tone producer: %lu loops/2s (%lums audio/2s), partial=%u",
                     (unsigned long)loops, (unsigned long)loops * 100,
                     (unsigned)partial);
            loops = 0; partial = 0;
            last_report = now;
        }
    }
}
#endif

/* 解码会话清理(direct/HLS 两路径同构段; 调用方各自补传输层收尾) */
static void decoder_cleanup(decoder_t *dec)
{
    if (s_dec_calls > 0) {
        ESP_LOGD(TAG, "decode timing: calls=%u avg=%lldus max=%lldus",
                 s_dec_calls, s_dec_us_total / s_dec_calls, s_dec_us_max);
    }
    s_dec_us_total = 0; s_dec_calls = 0; s_dec_us_max = 0;
    ESP_LOGI(TAG, "stream end: frames=%u decoded=%uB",
             (unsigned)dec->frame_cnt, (unsigned)dec->total_decoded);
    audio_mixer_set_source_active(s_player.src, false);
    if (dec->dec) {
        esp_audio_dec_close(dec->dec);
        dec->dec = NULL;
    }
    if (dec->mp3) {
        MP3FreeDecoder(dec->mp3);
        dec->mp3 = NULL;
    }
    dec->info_valid = false;
    s_pending_len = 0;
}

/* ================= 直连流路径(HTTP/ICY → 解码) =================
 * 返回 true = 正常EOF(数据收完), false = 错误/超时(重连同一URL) */
static bool direct_stream_run(void)
{
    static uint8_t feed[FEED_BUF_SIZE];
    decoder_t dec = { 0 };
    bool eof_ok = false;

    esp_http_client_config_t http_cfg = {
        .url = s_player.url,
        .timeout_ms = 6000,
        .buffer_size = FEED_BUF_SIZE,
        .keep_alive_enable = true,
        .event_handler = http_event_cb,         /* 捕获响应头 */
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (http == NULL) {
        s_player.last_read_error = true;
        return false;
    }

    /* ICY 元数据请求头(响应头的 icy-metaint 经事件回调捕获) */
    esp_http_client_set_header(http, "Icy-MetaData", "1");
    s_player.content_type[0] = 0;
    s_player.icy_metaint = 0;
    esp_err_t err = esp_http_client_open(http, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed(%s)", esp_err_to_name(err));
        esp_http_client_cleanup(http);
        s_player.last_read_error = true;
        return false;
    }
    esp_http_client_fetch_headers(http);        /* 触发 ON_HEADER 事件流 */

    dec.is_raw = (strstr(s_player.url, ".raw") != NULL);
    esp_err_t derr = ESP_OK;
    if (dec.is_raw) {
        /* RAW: 无需解码器, 固定 48k/16bit/stereo(与混音引擎同规格) */
        memset(&dec.info, 0, sizeof(dec.info));
        dec.info.sample_rate = 48000;
        dec.info.channel = 2;
        dec.info_valid = true;
        dec.resample_needed = false;
    } else {
        derr = decoder_open(&dec, pick_decoder_type(s_player.content_type, s_player.url),
                            NULL, 0);
    }
    if (derr != ESP_OK) {
        esp_http_client_cleanup(http);
        s_player.last_read_error = true;
        return false;
    }
    audio_mixer_set_source_active(s_player.src, true);
    s_player.state = PLAYER_PLAYING;
    cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
    ESP_LOGI(TAG, "playing %s (%s) metaint=%d",
             s_player.url, s_player.content_type, s_player.icy_metaint);

    size_t total_read = 0;
    int read_calls = 0;
    /* ---- 主循环: 读网络 → ICY 切分(协议层) → 解码(音频层) → 推混音器 ---- */
    icy_state_t icy;
    icy_reset(&icy, s_player.icy_metaint);
    while (!s_player.stop_req) {
        bt_pause_gate();                    /* BT 音乐接管: 挂起读流与供流 */
        int n = esp_http_client_read(http, (char *)feed, FEED_BUF_SIZE);
        total_read += (n > 0) ? n : 0;
        read_calls++;
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
        if (read_calls <= 30) {
            ESP_LOGI(TAG, "read#%d: n=%d total=%u", read_calls, n, (unsigned)total_read);
        }
#endif
        if (n <= 0) {
            /* n==0 且数据未收完 = 读取超时/停滞(非真EOF) → 重连同一URL;
             * n==0 且数据收完 = 真EOF → 播放列表切下一首; n<0 → 错误重连 */
            bool complete = esp_http_client_is_complete_data_received(http);
            eof_ok = (n == 0) && complete;
            s_player.last_read_error = !eof_ok;
            ESP_LOGW(TAG, "read ended: total_read=%u n=%d complete=%d",
                     (unsigned)total_read, n, complete);
            break;
        }
        icy_feed_chunk(&icy, feed, (size_t)n, decoder_feed_sink, &dec);
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
        {
            static int probe;
            if (++probe >= 40) {
                probe = 0;
                ESP_LOGI(TAG, "probe: dec_calls=%u dec_avg=%lldus pending=%u",
                         (unsigned)s_dec_calls,
                         s_dec_calls ? s_dec_us_total / s_dec_calls : 0,
                         (unsigned)s_pending_len);
            }
        }
#endif
    }

    /* ---- 清理本次连接 ---- */
    decoder_cleanup(&dec);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    return eof_ok;
}

/* AAC 解码器配置(裸段 ADTS; aac_plus=SBR): HLS 首开与 SBR 回退共用 */
static esp_aac_dec_cfg_t aac_cfg_build(const esp_extractor_stream_info_t *si,
                                       bool aac_plus)
{
    esp_aac_dec_cfg_t cfg = {
        .sample_rate = (int32_t)si->audio_info.sample_rate,
        .channel = (uint8_t)si->audio_info.channel,
        .bits_per_sample = 16,
        .no_adts_header = false,
        .aac_plus_enable = aac_plus,
    };
    return cfg;
}

/* ================= HLS 路径(m3u8, esp_hls_stream extractor 插件) =================
 * 播放列表解析/分段调度/AES-128 解密/直播刷新全部由插件内置(纯 esp_extractor
 * 生态, 不依赖 GMF 管线)。输出为按帧的压缩音频(MP3/AAC 裸帧), 交现有解码链。
 * 返回 true = 流正常结束(EOS), false = 错误。
 * 注: 帧读取阻塞期 stop 依赖网络超时生效(直播场景 M2 接入 abort 控制命令)。 */
/* GMF IO 工厂(仅用于给 HLS 插件提供 http/file IO, 不构建管线):
 * HLS 插件自身管理播放列表/分段调度/AES/直播刷新, 我们只消费输出帧 */
static esp_gmf_pool_handle_t s_io_pool;
static bool ensure_io_pool(void)
{
    if (s_io_pool != NULL) {
        return true;
    }
    if (esp_gmf_pool_init(&s_io_pool) != ESP_GMF_ERR_OK || s_io_pool == NULL) {
        return false;
    }
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.event_handle = NULL;
    esp_gmf_io_handle_t http = NULL;
    if (esp_gmf_io_http_init(&http_cfg, &http) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_register_io(s_io_pool, http, NULL) != ESP_GMF_ERR_OK) {
        return false;
    }
    file_io_cfg_t fs_cfg = FILE_IO_CFG_DEFAULT();
    fs_cfg.dir = ESP_GMF_IO_DIR_READER;
    esp_gmf_io_handle_t fs = NULL;
    if (esp_gmf_io_file_init(&fs_cfg, &fs) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_register_io(s_io_pool, fs, NULL) != ESP_GMF_ERR_OK) {
        return false;
    }
    return true;
}

static bool hls_session_run(void)
{
    static bool registered;                     /* 解复用器一次性注册 */
    if (!registered) {
        ESP_RETURN_ON_FALSE(esp_extractor_register_default() == ESP_EXTRACTOR_ERR_OK,
                            false, TAG, "extractor default register");
        ESP_RETURN_ON_FALSE(esp_hls_extractor_register() == ESP_EXTRACTOR_ERR_OK,
                            false, TAG, "hls extractor register");
        registered = true;
    }
    ESP_RETURN_ON_FALSE(ensure_io_pool(), false, TAG, "io pool init");

    decoder_t dec = { 0 };
    bool eof_ok = false;
    esp_extractor_handle_t ext = NULL;
    esp_err_t ret = ESP_OK;

    /* 音频-only 提取; 帧输出池 48KB(内部RAM, 欠压时插件以 WAITING_OUTPUT 反压)。
     * 网络 URL 必须走 io_cfg 变体(file_cfg 仅本地文件, 内部走 VFS fopen) */
    esp_hls_extractor_cfg_t *cfg = esp_hls_extractor_io_cfg_init(
            s_player.url, s_io_pool, ESP_EXTRACT_MASK_AUDIO, 48 * 1024, 64);
    ESP_GOTO_ON_FALSE(cfg != NULL, ESP_FAIL, hls_end, TAG, "hls cfg init");

    ESP_GOTO_ON_FALSE(esp_hls_extractor_open_with_cfg(cfg, &ext) == ESP_EXTRACTOR_ERR_OK,
                      ESP_FAIL, hls_end, TAG, "hls open");
    ESP_GOTO_ON_FALSE(esp_extractor_parse_stream(ext) == ESP_EXTRACTOR_ERR_OK,
                      ESP_FAIL, hls_end, TAG, "hls parse stream");

    /* 解码器在【首帧读出后】按流信息懒初始化(流探测是惰性的,
     * parse_stream 后立即 get_stream_info 会失败 —— 官方示例同款时序) */
    bool dec_ready = false;
    bool aac_plus_first = true;
    audio_mixer_set_source_active(s_player.src, true);
    ESP_LOGI(TAG, "hls session opened: %s", s_player.url);

    /* ---- 帧循环: extractor 帧输出 → 解码 → 推混音器 ---- */
    while (!s_player.stop_req) {
        bt_pause_gate();                    /* BT 音乐接管: 挂起读帧与供流 */
        esp_extractor_frame_info_t frame = { 0 };
        esp_extractor_err_t fret = esp_extractor_read_frame(ext, &frame);
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
        {
            static int rprobe;
            if (rprobe < 20) {
                rprobe++;
                ESP_LOGI(TAG, "rd#%d rc=%d type=%d flag=%d size=%u",
                         rprobe, fret, frame.stream_type, frame.frame_flag,
                         frame.frame_size);
            }
        }
#endif
        if (fret == ESP_EXTRACTOR_ERR_OK) {
            if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO &&
                frame.frame_size > 0) {
                /* HE-AAC 自动降级: 国内平台源(蜻蜓/喜马)为 HE-AAC(核心22050Hz+SBR),
                 * 首开不带SBR; 若连续解码失败 → 带aac_plus_enable重开一次 */
                if (dec_ready && s_aac_fail_streak > 30 && !s_aac_sbr_retry_done) {
                    esp_extractor_stream_info_t sinfo2 = { 0 };
                    esp_extractor_get_stream_info(ext, ESP_EXTRACTOR_STREAM_TYPE_AUDIO,
                                                  0, &sinfo2);
                    esp_aac_dec_cfg_t aac_cfg =
                        aac_cfg_build(&sinfo2, !aac_plus_first);  /* 翻转SBR重试 */
                    esp_audio_dec_close(dec.dec);
                    dec.dec = NULL;
                    s_pending_len = 0;
                    s_aac_sbr_retry_done = true;
                    s_aac_fail_streak = 0;
                    if (esp_audio_dec_open(&(esp_audio_dec_cfg_t){
                            .type = ESP_AUDIO_TYPE_AAC,
                            .cfg = &aac_cfg, .cfg_sz = sizeof(aac_cfg)}, &dec.dec)
                            == ESP_AUDIO_ERR_OK) {
                        ESP_LOGW(TAG, "aac reopened with SBR (HE-AAC detected)");
                    } else {
                        ESP_LOGE(TAG, "aac SBR reopen failed");
                        goto hls_end;
                    }
                }
                if (!dec_ready) {
                    /* 首帧: 流探测是惰性的, 此时 get_stream_info 才有效 →
                     * 按流信息选型并打开解码器(此前 dec 必须保持零初始化) */
                    esp_extractor_stream_info_t sinfo = { 0 };
                    ESP_GOTO_ON_FALSE(
                        esp_extractor_get_stream_info(ext, ESP_EXTRACTOR_STREAM_TYPE_AUDIO,
                                                      0, &sinfo) == ESP_EXTRACTOR_ERR_OK,
                        ESP_FAIL, hls_end, TAG, "stream info");
                    uint32_t fmt = sinfo.audio_info.format;
                    ESP_LOGI(TAG, "hls stream: codec=%c%c%c%c %luHz %dch",
                             (char)(fmt & 0xff), (char)((fmt >> 8) & 0xff),
                             (char)((fmt >> 16) & 0xff), (char)((fmt >> 24) & 0xff),
                             (unsigned long)sinfo.audio_info.sample_rate,
                             sinfo.audio_info.channel);
                    if (fmt == ESP_EXTRACTOR_AUDIO_FORMAT_MP3) {
                        ESP_GOTO_ON_FALSE(
                            decoder_open(&dec, ESP_AUDIO_TYPE_MP3, NULL, 0) == ESP_OK,
                            ESP_FAIL, hls_end, TAG, "mp3 decoder open");
                    } else if (fmt == ESP_EXTRACTOR_AUDIO_FORMAT_AAC) {
                        /* 实测(蜻蜓/喜马): 分段 AAC 帧带 ADTS 头(ff f9), 解码器
                         * 自行解析头(采样率/声道以头为准); 国内平台源多为 HE-AAC,
                         * 启用 SBR; 若实际是 LC 造成异常, 由下方 SBR 回退链兜底 */
                        esp_aac_dec_cfg_t aac_cfg = aac_cfg_build(&sinfo, true);
                        ESP_GOTO_ON_FALSE(
                            decoder_open(&dec, ESP_AUDIO_TYPE_AAC,
                                         &aac_cfg, sizeof(aac_cfg)) == ESP_OK,
                            ESP_FAIL, hls_end, TAG, "aac decoder open");
                    } else {
                        ESP_LOGE(TAG, "unsupported codec in hls: %08lx", (unsigned long)fmt);
                        goto hls_end;
                    }
                    s_player.state = PLAYER_PLAYING;
                    cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
                    ESP_LOGI(TAG, "playing %s (hls)", s_player.url);
                    aac_plus_first = true;
                    dec_ready = true;
                }
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
                {
                    static int hp;
                    if (hp < 3) {
                        hp++;
                        ESP_LOGI(TAG, "frame head[%d]: %02x %02x %02x %02x (size=%u)",
                                 hp, frame.frame_buffer[0], frame.frame_buffer[1],
                                 frame.frame_buffer[2], frame.frame_buffer[3],
                                 frame.frame_size);
                    }
                }
#endif
                decoder_feed(&dec, frame.frame_buffer, frame.frame_size);
            }
            esp_extractor_release_frame(ext, &frame);
        } else if (fret == ESP_EXTRACTOR_ERR_EOS) {
            eof_ok = true;                      /* 正常结束(VOD ENDLIST) */
            break;
        } else if (fret == ESP_EXTRACTOR_ERR_WAITING_OUTPUT) {
            vTaskDelay(pdMS_TO_TICKS(20));      /* 输出池满, 稍候再取 */
            continue;
        } else {
            ESP_LOGW(TAG, "read frame failed ret=%d", fret);
            break;                              /* 网络/解析错误 → 重连 */
        }
    }

hls_end:
    decoder_cleanup(&dec);
    if (ext) {
        esp_extractor_close(ext);
    }
    if (cfg) {
        esp_hls_extractor_io_cfg_deinit(cfg);
    }
    s_player.last_read_error = !eof_ok;
    return eof_ok;
}

static void player_task(void *arg)
{
    int retries = 0;

#if CONFIG_ESP_CUBE_TEST_TONE
    audio_mixer_set_source_active(s_player.src, true);
    s_player.state = PLAYER_PLAYING;
    cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
    test_tone_loop();
    vTaskDelete(NULL);
    return;
#endif

    esp_audio_dec_register_default();           /* 注册内置 MP3/AAC 等解码器 */

    s_player.state = PLAYER_CONNECTING;
    cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));

    while (!s_player.stop_req && retries < PLAYER_MAX_RETRIES) {
        bt_pause_gate();                    /* BT 接管期间在此驻留, 恢复后重开会话 */
        /* 按 URL 协议分派: m3u8 → HLS; 其余 → HTTP/ICY 直连 */
        bool eof_ok = strstr(s_player.url, ".m3u8")
                      ? hls_session_run()
                      : direct_stream_run();
        audio_mixer_set_source_active(s_player.src, false);

        if (!s_player.stop_req) {
            /* BT 暂停期间会话被服务器断开属预期(踢空闲连接): 不计失败,
             * 回到循环顶部的 gate 驻留, 直到 BT 释放后重开会话 */
            if (s_player.bt_pause) {
                retries = 0;
                continue;
            }
            /* EOF → 播放列表轮播下一首; 读取错误 → 原地重试同一URL */
            if (s_player.playlist_idx >= 0 && eof_ok) {
                s_player.playlist_idx = (s_player.playlist_idx + 1) % PLAYLIST_NUM;
                strncpy(s_player.url, s_playlist[s_player.playlist_idx],
                        sizeof(s_player.url) - 1);
                ESP_LOGI(TAG, "playlist → [%d] %s", s_player.playlist_idx, s_player.url);
                retries = 0;
            } else {
                ESP_LOGW(TAG, "stream ended, reconnecting...");
                retries++;
            }
            vTaskDelay(pdMS_TO_TICKS(PLAYER_RETRY_STEP_MS * (retries > 0 ? retries : 1)));
        }
    }

    esp_audio_dec_unregister_default();

    if (!s_player.stop_req) {
        s_player.state = PLAYER_ERROR;
        cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
    }
    s_player.task = NULL;
    vTaskDelete(NULL);
}

/* ================= 对外接口 ================= */
esp_err_t app_player_start(const char *url)
{
    if (url == NULL) {                          /* 无参: 从播放列表头开始轮播 */
        s_player.playlist_idx = 0;
        url = s_playlist[0];
    } else {
        s_player.playlist_idx = -1;             /* 显式指定: 不参与轮播 */
    }
    ESP_RETURN_ON_FALSE(strlen(url) > 8, ESP_ERR_INVALID_ARG, TAG, "bad url");
    if (s_player.src == NULL) {
        audio_source_cfg_t scfg = {
            .name = "net",
            .type = AUDIO_SRC_NET,
            .buffer_ms = 10000,     /* 10秒 PSRAM 环形缓冲(~2MB): 实测直播边缘下
                                     * 4s 缓冲会被"分段发布等待"击穿(net_buf 周期性
                                     * 归零→约3分钟一次的1秒卡顿), 见 docs/03 */
            .prefill_ms = CONFIG_ESP_CUBE_NET_PREFILL_MS,
            .blocking = true,       /* 播放任务可阻塞, 按实时节奏供给混音器 */
        };
        s_player.src = audio_mixer_add_source(&scfg);
        ESP_RETURN_ON_FALSE(s_player.src != NULL, ESP_ERR_NO_MEM, TAG, "add source");
    }
    /* 请求切换: 先停旧任务(含等待退出, 避免两任务抢同一解码器) */
    app_player_stop();
    strncpy(s_player.url, url, sizeof(s_player.url) - 1);
    s_player.stop_req = false;
    /* 解码任务放 core1: core0 被 mixer(20)/WiFi(23)/BT(23+) 高优任务占据,
     * 解码(prio12)在 core0 抢不到 CPU → 实测供给仅 0.5×实时(40ms/帧)。
     * 栈放 PSRAM(静态创建, 2026-11-10): UI/LVGL 引入后内部 RAM 仅剩
     * ~15KB 且碎片化, 动态创建 12KB 栈失败 → 电台无声; 网络/解码任务
     * 对栈访问延迟不敏感, PSRAM 栈可行(SPIRAM_ALLOW_STACK_EXTERNAL=y)。
     * 双槽轮换: 静态任务自删除后 TCB 须待 idle 清理, 立即复用同槽有
     * 链表损坏风险; 轮换保证复用时 idle 必已处理(切台间隔远大于调度期)。 */
    static StackType_t *s_stacks[2];
    static StaticTask_t s_tcbs[2];
    static int s_slot;
    s_slot ^= 1;
    if (s_stacks[s_slot] == NULL) {
        s_stacks[s_slot] = heap_caps_malloc(PLAYER_TASK_STACK * sizeof(StackType_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_stacks[s_slot] != NULL, ESP_ERR_NO_MEM, TAG,
                            "stack alloc(psram) fail");
    }
    s_player.task = xTaskCreateStaticPinnedToCore(
            player_task, CUBE_TASK_PLAYER_NAME, PLAYER_TASK_STACK, NULL,
            CUBE_TASK_PLAYER_PRIO,
            s_stacks[s_slot], &s_tcbs[s_slot], CUBE_TASK_PLAYER_CORE);
    if (s_player.task == NULL) {
        ESP_LOGE(TAG, "player task create fail: internal free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_player_stop(void)
{
    if (s_player.task != NULL) {
        s_player.stop_req = true;
        for (int i = 0; i < 20 && s_player.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_player.state = PLAYER_IDLE;
    return ESP_OK;
}

app_player_state_t app_player_get_state(void)
{
    return s_player.state;
}

const char *app_player_get_url(void)
{
    return s_player.url;
}

/* ---- 播放列表接口(电台单一来源; 见 app_player.h) ---- */
int app_player_station_num(void)
{
    return (int)PLAYLIST_NUM;
}

const char *app_player_station_name(int idx)
{
    if (idx < 0 || idx >= (int)PLAYLIST_NUM) return NULL;
    return s_station_names[idx];
}

int app_player_station_idx(void)
{
    return s_player.playlist_idx;
}

esp_err_t app_player_play_index(int idx)
{
    int n = (int)PLAYLIST_NUM;
    idx = ((idx % n) + n) % n;                    /* 越界回绕 */
    esp_err_t err = app_player_start(s_playlist[idx]);
    if (err == ESP_OK) {
        s_player.playlist_idx = idx;              /* 显式点选仍参与后续轮播 */
    }
    return err;
}
