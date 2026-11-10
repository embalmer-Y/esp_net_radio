/*
 * cube_resampler.h —— 轻量线性插值重采样器(立体声 int16)
 *
 * 用途: 音源采样率统一到混音引擎主时钟 48kHz(设计文档 §3.1)。
 *   - A2DP 44.1k → 48k;网络流 MP3 44.1k → 48k;TTS 24k mono → 48k stereo。
 *   - 线性插值质量对听感足够(M1);优化路径: S31 硬件 ASRC / 多相滤波(M4)。
 *
 * 注意: 每个 PCM 流需要独立的重采样器实例(内部有相位状态)。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float ratio;      /* 输出采样率 / 输入采样率 */
    float phase;      /* 插值相位 [0,1) */
    int16_t prev_l;   /* 上一对输入样本(插值基点) */
    int16_t prev_r;
} cube_resampler_t;

/* 初始化: src_rate 为输入采样率, dst_rate 为输出采样率(如 44100 → 48000) */
void cube_resampler_init(cube_resampler_t *rs, float src_rate, float dst_rate);

/*
 * 立体声重采样(交错 int16, LRLR...):
 *   输入 in[in_samples*2], 输出写 out, out 容量 out_max_samples(帧数)。
 *   返回实际输出帧数。未消费的输入样本会保留在内部状态(跨调用连续)。
 */
size_t cube_resampler_process_stereo(cube_resampler_t *rs,
                                     const int16_t *in, size_t in_samples,
                                     int16_t *out, size_t out_max_samples);

/* 单声道 → 立体声重采样(24k TTS → 48k stereo 场景) */
size_t cube_resampler_process_mono_to_stereo(cube_resampler_t *rs,
                                             const int16_t *in, size_t in_samples,
                                             int16_t *out, size_t out_max_frames);

#ifdef __cplusplus
}
#endif
