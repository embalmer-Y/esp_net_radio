/*
 * cube_ringbuf.h —— 线程安全字节环形缓冲
 *
 * 用途: 音频生产者(BT/网络流/AI TTS)与混音器之间的 PCM 交接。
 *   - 缓冲内存默认从 PSRAM 分配(大容量), 分配失败自动回退内部 RAM;
 *   - 多生产者/单消费者场景由调用方保证(每通道一个生产者)。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cube_ringbuf *cube_ringbuf_handle_t;

/* 创建环形缓冲(容量 bytes; 内部含互斥锁) */
cube_ringbuf_handle_t cube_ringbuf_create(size_t capacity);
/* 销毁 */
void cube_ringbuf_destroy(cube_ringbuf_handle_t rb);

/* 写入 bytes 数据; 超出剩余空间时返回实际写入量(调用方按欠载策略处理) */
size_t cube_ringbuf_write(cube_ringbuf_handle_t rb, const void *data, size_t bytes);

/* 读出 bytes 数据到 dst; 数据不足时返回实际读出量 */
size_t cube_ringbuf_read(cube_ringbuf_handle_t rb, void *dst, size_t bytes);

/* 当前可读字节数 */
size_t cube_ringbuf_available(cube_ringbuf_handle_t rb);
/* 清空 */
void cube_ringbuf_reset(cube_ringbuf_handle_t rb);

#ifdef __cplusplus
}
#endif
