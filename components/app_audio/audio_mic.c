/*
 * audio_mic.c —— 16k 单声道麦克风服务实现
 *
 * 数据路径: drv_audio_mic_read(48k 立体声, 与播放同 LRCK)
 *           → 逐样本 3:1 均值抽取(左声道) → 16k 单声道
 *
 * 实现要点:
 *   - 抽取状态(sum3/cnt3)跨调用保留, 块尾余数不丢帧;
 *   - ÷3 用乘法倒数替代((x*21845)>>16, 误差 ≤1 LSB);
 *   - 缓冲一次分配终身复用(约 1.9KB, 内部 RAM);
 *   - 暂停期(所有权让渡)I2S 仍在跑, 恢复后首先读到的是残留旧数据
 *     (一次块大小, ~10ms) —— 唤醒检测对这点陈旧不敏感, 不做冲刷。
 */
#include "audio_mic.h"

#include <string.h>
#include <stdlib.h>
#include "drv_audio.h"

#define MIC48_CHUNK_FRAMES 480            /* 一次拉取 10ms@48k 立体声 */

static int16_t *s_buf48;                  /* MIC48_CHUNK_FRAMES*2 交错样本 */
static int s_n, s_pos;                    /* 本块样本数 / 消费游标 */
static int32_t s_sum3;                    /* 3 样本均值累加器(跨调用保留) */
static int s_cnt3;

int audio_mic_read_16k(int16_t *mono, int frames)
{
    if (mono == NULL || frames <= 0) return 0;
    if (s_buf48 == NULL) {
        s_buf48 = malloc(MIC48_CHUNK_FRAMES * 2 * sizeof(int16_t));
        if (s_buf48 == NULL) return 0;
    }
    int out = 0;
    while (out < frames) {
        if (s_pos >= s_n) {
            int got = drv_audio_mic_read(s_buf48, MIC48_CHUNK_FRAMES);
            if (got <= 0) break;          /* 欠载: 交还已产出部分 */
            s_n = got * 2;
            s_pos = 0;
        }
        while (out < frames && s_pos < s_n) {
            if ((s_pos & 1) == 0) {       /* 左声道(板载单麦) */
                s_sum3 += s_buf48[s_pos];
                if (++s_cnt3 == 3) {
                    mono[out++] = (int16_t)((s_sum3 * 21845) >> 16);
                    s_sum3 = 0;
                    s_cnt3 = 0;
                }
            }
            s_pos++;
        }
    }
    return out;
}
