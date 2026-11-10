/*
 * cube_fft.h —— 小型 FFT(频谱可视化用)
 *
 * 用途: 混音器输出的 PCM 抽样 → 频谱柱状图(设计文档 §4)。
 * 实现: 基-2 radix-2 就地 FFT, 仅支持 2 的幂点数; 输出幅度谱(线性)。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 实数序列 FFT(输入视为虚部=0), 输出前 n/2 个点的幅度 |X[k]|。
 *   in:  int16 PCM, n 点(n 为 2 的幂)
 *   out: float 幅度数组, 容量 n/2
 */
void cube_fft_magnitude(const int16_t *in, size_t n, float *out);

#ifdef __cplusplus
}
#endif
