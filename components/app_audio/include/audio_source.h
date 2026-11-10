/*
 * audio_source.h —— 音源抽象接口
 *
 * 模型(拉取式):
 *   生产者任务(蓝牙栈/网络流/AI TTS)通过 audio_mixer_source_write() 把
 *   PCM 推进音源专属环形缓冲;混音器每 10ms 从各源【拉取】一帧。
 *   好处: 混音节拍单一(I2S DMA), 源速率差异由环形缓冲吸收。
 *
 * 采样率契约(设计文档 §3.1): 源在【推入前】自行转换到混音主时钟
 *   (如 A2DP 44.1k→48k 由 app_bt 用 cube_resampler 转换后推入)。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音源类别: ducking 状态机的判别依据(音乐类可被压低, 系统音不压) */
typedef enum {
    AUDIO_SRC_BT = 0,     /* 蓝牙 A2DP —— duckable */
    AUDIO_SRC_NET,        /* 网络电台/流 —— duckable */
    AUDIO_SRC_TTS,        /* AI TTS —— 不 duck(它就是 duck 发起者) */
    AUDIO_SRC_TONE,       /* UI 提示音 —— 不 duck */
} audio_src_type_t;

/* 音源句柄(实现在 audio_mixer.c, 对上层不透明) */
typedef struct audio_source *audio_source_handle_t;

#ifdef __cplusplus
}
#endif
