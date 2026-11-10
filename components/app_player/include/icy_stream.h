/*
 * icy_stream.h —— ICY/Shoutcast 协议流解析(独立模块)
 *
 * 协议: 客户端请求头带 "Icy-MetaData: 1" 时, 服务器每发送 icy-metaint 字节
 * 音频数据后, 跟随 1 字节长度字段(×16 = 元数据字节数) + 元数据块
 * ("StreamTitle='..';StreamUrl='..';")。元数据块可跨网络读取块边界。
 *
 * 本模块只做【协议切分】: 音频字节经回调交还调用方(喂解码器),
 * 元数据解析出 StreamTitle 后发布 CUBE_EVT_STREAM_TITLE 事件。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音频字节回调(在 icy_feed_chunk 调用方上下文同步执行) */
typedef void (*icy_audio_sink_fn)(const void *data, size_t len, void *ctx);

/* ICY 状态机(调用方持有, 通常位于播放任务栈) */
typedef struct {
    int  metaint;       /* 0 = 服务器不带元数据 */
    int  state;         /* 内部状态(icy_state_e) */
    int  audio_left;    /* 距下一个元数据块的音频字节数 */
    int  meta_need;     /* 本次元数据块总字节数 */
    int  meta_got;
    char meta_buf[512];
    char title[128];    /* 最近一次解析出的 StreamTitle */
} icy_state_t;

/* 复位状态机(每次连接后调用; metaint 来自响应头 icy-metaint) */
void icy_reset(icy_state_t *st, int metaint);

/*
 * 喂入一个网络读取块: 内部切分音频/元数据, 音频字节经 sink(ctx) 交付。
 * 返回值无(块内数据全部被消费: 音频交回调, 元数据入状态机)。
 */
void icy_feed_chunk(icy_state_t *st, const uint8_t *data, size_t n,
                    icy_audio_sink_fn sink, void *ctx);

#ifdef __cplusplus
}
#endif
