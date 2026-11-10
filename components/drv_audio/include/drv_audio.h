/*
 * drv_audio.h —— 音频驱动层(ES8311 codec + I2S, P4C5 版)
 *
 * 与 firmware/components/drv_audio/include/drv_audio.h 保持同一 API 形状
 * (app_audio 混音器同源复用的边界); 差异仅在实现: 48k 立体声播放为
 * 主, 录音接口为空实现(电台 App 无语音)。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;         /* 播放采样率(混音引擎主时钟, 默认 48000) */
    int channels;            /* 2 */
} drv_audio_cfg_t;

/* 初始化 codec + I2S(幂等); 成功后即可以 drv_audio_output 播放 */
esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg);

/* 阻塞式输出 PCM(交错 int16, 帧数 frames; 返回实际写入帧数)
 * 混音器以本函数为节拍: DMA 缓冲欠载时阻塞, 天然形成 pacing */
int drv_audio_output(const int16_t *samples, int frames);

/* 录音读取: 本 App 无语音, 恒返回 0(audio_mic 服务的空闭包) */
int drv_audio_mic_read(int16_t *buf, int frames);

/* 音量(0-100): codec 输出级 */
esp_err_t drv_audio_set_volume(int vol);

/* 功放使能(NS4150B STD, 高有效) */
esp_err_t drv_audio_pa_set(bool on);

#ifdef __cplusplus
}
#endif
