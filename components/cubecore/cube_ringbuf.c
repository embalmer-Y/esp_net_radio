/*
 * cube_ringbuf.c —— 环形缓冲实现(互斥锁版)
 *
 * 性能说明: 10ms 音频帧粒度下锁开销 <1% CPU, 换取实现简单可靠;
 * 若未来成为瓶颈(如 96kHz 多通道), 再改为单生产者/单消费者无锁版。
 */
#include "cube_ringbuf.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

struct cube_ringbuf {
    uint8_t *buf;
    size_t capacity;
    size_t head;      /* 写位置 */
    size_t tail;      /* 读位置 */
    size_t used;      /* 已用字节 */
    SemaphoreHandle_t lock;
};

cube_ringbuf_handle_t cube_ringbuf_create(size_t capacity)
{
    struct cube_ringbuf *rb = calloc(1, sizeof(*rb));
    if (rb == NULL) {
        return NULL;
    }
    /* 音频缓冲优先 PSRAM(容量大), 失败回退内部 RAM */
    rb->buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rb->buf == NULL) {
        rb->buf = malloc(capacity);
    }
    rb->capacity = capacity;
    rb->lock = xSemaphoreCreateMutex();
    if (rb->buf == NULL || rb->lock == NULL) {
        cube_ringbuf_destroy(rb);
        return NULL;
    }
    return rb;
}

void cube_ringbuf_destroy(cube_ringbuf_handle_t rb)
{
    if (rb == NULL) {
        return;
    }
    if (rb->lock) {
        vSemaphoreDelete(rb->lock);
    }
    free(rb->buf);
    free(rb);
}

size_t cube_ringbuf_write(cube_ringbuf_handle_t rb, const void *data, size_t bytes)
{
    size_t written = 0;
    xSemaphoreTake(rb->lock, portMAX_DELAY);
    size_t free_space = rb->capacity - rb->used;
    size_t n = bytes < free_space ? bytes : free_space;

    /* 环形写入: 尾部一段 + 回绕一段 */
    size_t tail_room = rb->capacity - rb->head;
    size_t first = n < tail_room ? n : tail_room;
    memcpy(rb->buf + rb->head, data, first);
    if (n > first) {
        memcpy(rb->buf, (const uint8_t *)data + first, n - first);
    }
    rb->head = (rb->head + n) % rb->capacity;
    rb->used += n;
    written = n;
    xSemaphoreGive(rb->lock);
    return written;
}

size_t cube_ringbuf_read(cube_ringbuf_handle_t rb, void *dst, size_t bytes)
{
    size_t n;
    xSemaphoreTake(rb->lock, portMAX_DELAY);
    n = bytes < rb->used ? bytes : rb->used;
    size_t tail_room = rb->capacity - rb->tail;
    size_t first = n < tail_room ? n : tail_room;
    memcpy(dst, rb->buf + rb->tail, first);
    if (n > first) {
        memcpy((uint8_t *)dst + first, rb->buf, n - first);
    }
    rb->tail = (rb->tail + n) % rb->capacity;
    rb->used -= n;
    xSemaphoreGive(rb->lock);
    return n;
}

size_t cube_ringbuf_available(cube_ringbuf_handle_t rb)
{
    xSemaphoreTake(rb->lock, portMAX_DELAY);
    size_t used = rb->used;
    xSemaphoreGive(rb->lock);
    return used;
}

void cube_ringbuf_reset(cube_ringbuf_handle_t rb)
{
    xSemaphoreTake(rb->lock, portMAX_DELAY);
    rb->head = rb->tail = rb->used = 0;
    xSemaphoreGive(rb->lock);
}
