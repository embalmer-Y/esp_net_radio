/*
 * cube_fft.c —— radix-2 迭代法 FFT(单精度)
 * 256 点 @48kHz: 频率分辨率 187.5Hz, 满足 UI 频谱柱状图需求;
 * 耗时约 0.3ms @320MHz(30fps 更新无压力)。
 */
#include "cube_fft.h"

#include <math.h>
#include "esp_heap_caps.h"
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 支持的最大变换点数(工作区按此分配); 超限调用直接返回 */
#define FFT_MAX_N 1024

/* 就地复数 FFT: re/im 长度 n(n 为 2 的幂) */
static void fft_inplace(float *re, float *im, size_t n)
{
    /* 位反转重排 */
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    /* 蝶形运算 */
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float w_re = cosf(ang), w_im = sinf(ang);
        for (size_t i = 0; i < n; i += len) {
            float cur_re = 1.0f, cur_im = 0.0f;
            for (size_t k = 0; k < len / 2; k++) {
                size_t a = i + k, b = i + k + len / 2;
                float u_re = re[a], u_im = im[a];
                float v_re = re[b] * cur_re - im[b] * cur_im;
                float v_im = re[b] * cur_im + im[b] * cur_re;
                re[a] = u_re + v_re;
                im[a] = u_im + v_im;
                re[b] = u_re - v_re;
                im[b] = u_im - v_im;
                float nre = cur_re * w_re - cur_im * w_im;
                cur_im = cur_re * w_im + cur_im * w_re;
                cur_re = nre;
            }
        }
    }
}

/* 复数工作区(静态, 首次调用分配一次): 内部 RAM——LVGL 任务 30fps
 * 高频访问, PSRAM 有 cache 惩罚; 共 8KB 用量小无压力 */
static float *s_re, *s_im;

void cube_fft_magnitude(const int16_t *in, size_t n, float *out)
{
    if (s_re == NULL) {
        s_re = heap_caps_malloc(sizeof(float) * FFT_MAX_N, MALLOC_CAP_INTERNAL);
        s_im = heap_caps_malloc(sizeof(float) * FFT_MAX_N, MALLOC_CAP_INTERNAL);
        if (s_re == NULL || s_im == NULL) {
            /* 部分失败必须整体回收: 旧实现留下半初始化工作区,
             * 下次调用直接向 NULL 写(崩溃) */
            heap_caps_free(s_re);
            heap_caps_free(s_im);
            s_re = s_im = NULL;
            return;
        }
    }
    if (in == NULL || out == NULL || n == 0 || n > FFT_MAX_N) return;
    for (size_t i = 0; i < n; i++) {
        s_re[i] = (float)in[i];
        s_im[i] = 0.0f;
    }
    fft_inplace(s_re, s_im, n);
    for (size_t k = 0; k < n / 2; k++) {
        out[k] = sqrtf(s_re[k] * s_re[k] + s_im[k] * s_im[k]);
    }
}
