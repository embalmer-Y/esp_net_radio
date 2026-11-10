/*
 * audio_mic.h —— 麦克风服务(L2 app_audio)
 *
 * 把 drv_audio 的 48k 立体声原始录音整流为 16k 单声道, 供语音前端
 * (app_voice WakeNet / app_ai_session ASR)统一取数 —— 应用层不直接
 * 触碰 drv_audio(docs/06 §2 越层禁令)。
 *
 * 单消费者协议(docs/06 §3): 同一时刻只允许一个任务调用;
 * 所有权切换: app_voice 订阅 CUBE_EVT_AI_STATE, 在 AI_LISTENING 期间
 * 暂停读取让渡给 ai_session, 其余状态恢复(WakeNet 常开供 barge-in)。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读取 16k 单声道 PCM(内部完成 48k 立体声采集 + 3:1 均值抽取, 跨调用
 * 带进位不丢帧; 返回实际样本数, 0=暂无数据/未初始化) */
int audio_mic_read_16k(int16_t *mono, int frames);

#ifdef __cplusplus
}
#endif
