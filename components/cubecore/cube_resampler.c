/*
 * cube_resampler.c —— 线性插值重采样实现
 */
#include "cube_resampler.h"

static inline int16_t clamp16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void cube_resampler_init(cube_resampler_t *rs, float src_rate, float dst_rate)
{
    rs->ratio = dst_rate / src_rate;
    rs->phase = 0.0f;
    rs->prev_l = 0;
    rs->prev_r = 0;
}

/* 通用插值核心: ratio>1 上采样, ratio<1 下采样(简化处理, 未见明显混叠) */
size_t cube_resampler_process_stereo(cube_resampler_t *rs,
                                     const int16_t *in, size_t in_samples,
                                     int16_t *out, size_t out_max_samples)
{
    size_t o = 0;
    size_t i = 0;
    float step = 1.0f / rs->ratio;          /* 每个输出样本对应的输入步进 */
    float pos = rs->phase;                  /* 当前输出对应输入流位置(样本为单位) */
    int16_t last_l = rs->prev_l;
    int16_t last_r = rs->prev_r;

    while (i < in_samples && o < out_max_samples) {
        /* pos 落在 last(上一批末样本) 与 in[i] 之间时插值 */
        while (pos >= 1.0f) {
            last_l = in[i * 2];
            last_r = in[i * 2 + 1];
            pos -= 1.0f;
            i++;
            if (i >= in_samples) {
                goto done;                  /* 输入耗尽, 相位留给下一批 */
            }
        }
        float t = pos;
        int32_t l = (int32_t)last_l + (int32_t)(((int32_t)in[i * 2]     - last_l) * t);
        int32_t r = (int32_t)last_r + (int32_t)(((int32_t)in[i * 2 + 1] - last_r) * t);
        out[o * 2]     = clamp16(l);
        out[o * 2 + 1] = clamp16(r);
        o++;
        pos += step;
    }
done:
    rs->phase = pos;
    rs->prev_l = last_l;
    rs->prev_r = last_r;
    return o;
}

size_t cube_resampler_process_mono_to_stereo(cube_resampler_t *rs,
                                             const int16_t *in, size_t in_samples,
                                             int16_t *out, size_t out_max_frames)
{
    size_t o = 0;
    size_t i = 0;
    float step = 1.0f / rs->ratio;
    float pos = rs->phase;
    int16_t last = rs->prev_l;

    while (i < in_samples && o < out_max_frames) {
        while (pos >= 1.0f) {
            last = in[i];
            pos -= 1.0f;
            i++;
            if (i >= in_samples) {
                goto done;
            }
        }
        int32_t v = (int32_t)last + (int32_t)(((int32_t)in[i] - last) * pos);
        int16_t s = clamp16(v);
        out[o * 2]     = s;                 /* 单声道复制到双声道 */
        out[o * 2 + 1] = s;
        o++;
        pos += step;
    }
done:
    rs->phase = pos;
    rs->prev_l = last;
    return o;
}
