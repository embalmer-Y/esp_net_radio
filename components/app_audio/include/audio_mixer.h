/*
 * audio_mixer.h —— 软件混音引擎(全机音频单一汇聚点)
 *
 * 数据流(设计文档 §3):
 *   [BT][NET][TTS][TONE] → 各自环形缓冲 → 混音器(10ms 帧, Q15 增益饱和加法)
 *     → master gain → drv_audio_output(I2S/ES8311)
 *     ├→ AEC 参考抽头(app_voice 注册的回调, 边放边听的回声消除参考)
 *     └→ 频谱抽头(UI 可视化)
 *
 * Ducking: AI 说话 → duckable 音源 300ms 淡降至 -14dB → 结束 → 500ms 恢复。
 * 实时性: 混音任务固定 core0 高优先级;唯一阻塞点是 drv_audio_output(节拍)。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "audio_source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;         /* 主时钟(默认 48000) */
    int channels;            /* 2 */
} audio_mixer_cfg_t;

typedef struct {
    const char *name;        /* 调试用名("bt"/"net"/"tts"...) */
    audio_src_type_t type;
    int buffer_ms;           /* 环形缓冲时长(蓝牙建议 300ms, 网络 400ms) */
    int prefill_ms;          /* 预填充: 缓冲攒够此时长才开始消耗(jitter buffer,
                              * 防止实时供给速率下缓冲在零点平衡→周期性欠载静音) */
    bool blocking;           /* true: 缓冲满时生产者等待(网络流/TTS 任务可阻塞);
                              * false: 满则丢弃新数据(BT 栈回调不可阻塞) */
} audio_source_cfg_t;

/* ============ 生命周期 ============ */
esp_err_t audio_mixer_init(const audio_mixer_cfg_t *cfg);
esp_err_t audio_mixer_start(void);              /* 创建混音任务并打开输出 */
void audio_mixer_stop(void);

/* ============ 音源管理 ============ */
audio_source_handle_t audio_mixer_add_source(const audio_source_cfg_t *cfg);
/* 注: 音源节点创建后运行期不释放(无 remove 接口), 移除需整体 audio_mixer_stop 后重建 */

/* 生产者推入 PCM(交错 int16, 已是主时钟采样率);返回实际接收字节数
 * 环形缓冲满时丢弃新数据(保护已缓冲内容), 返回值告知调用方 */
size_t audio_mixer_source_write(audio_source_handle_t src,
                                const void *pcm, size_t bytes, TickType_t wait);

/* ============ 控制 ============ */
void audio_mixer_set_master_volume(int percent);    /* 0-100(感知曲线) */
int  audio_mixer_get_master_volume(void);
void audio_mixer_set_ducking(bool ai_talking);      /* AI 说话/结束 */
void audio_mixer_set_source_active(audio_source_handle_t src, bool active);

/* ============ 观测 ============ */
/* 取最近输出的 frames 帧拷贝(UI 频谱), 无新数据返回 false */
bool audio_mixer_take_spectrum(int16_t *out, int frames);

/* 注册 AEC 参考抽头(app_voice 在里程碑2调用; fn 在混音任务上下文执行, 不得阻塞) */
typedef void (*audio_ref_sink_fn)(const int16_t *stereo, int frames, void *user_ctx);
void audio_mixer_set_ref_sink(audio_ref_sink_fn fn, void *user_ctx);

#ifdef __cplusplus
}
#endif
