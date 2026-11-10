/*
 * icy_stream.c —— ICY 协议状态机实现
 *
 * 设计要点(与《02_详细设计.md》§7 对应):
 *   1. 元数据块可跨网络读取块边界 → 状态机保存部分收集的元数据;
 *   2. 音频字节零丢弃(早期实现曾丢弃元数据块之后的剩余数据, 导致
 *      MP3 流错位 → "Not supported format" 周期性错误, 实测教训);
 *   3. StreamTitle 解析失败不影响音频流(仅元数据语义, 容错处理)。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "icy_stream.h"

#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "cubecore.h"

static const char *TAG = "icy";

enum { ICY_AUDIO = 0, ICY_META_LEN, ICY_META_BODY };

void icy_reset(icy_state_t *st, int metaint)
{
    memset(st, 0, sizeof(*st));
    st->metaint = metaint;
    st->state = ICY_AUDIO;
    st->audio_left = metaint;           /* metaint<=0 时进入"直通"分支, 无意义 */
}

/* "StreamTitle='xxx';" → 提取标题, 存入 st->title 并发布事件 */
static void parse_title(icy_state_t *st, const char *meta)
{
    const char *p = strcasestr(meta, "StreamTitle='");
    if (p == NULL) return;
    p += strlen("StreamTitle='");
    const char *e = strchr(p, '\'');
    if (e == NULL) return;
    size_t n = e - p;
    if (n >= sizeof(st->title)) n = sizeof(st->title) - 1;
    memcpy(st->title, p, n);
    st->title[n] = 0;
    ESP_LOGI(TAG, "title: %s", st->title);
    cube_event_publish(CUBE_EVT_STREAM_TITLE, st->title, n + 1);
}

void icy_feed_chunk(icy_state_t *st, const uint8_t *data, size_t n,
                    icy_audio_sink_fn sink, void *ctx)
{
    const uint8_t *p = data;
    int remain = (int)n;
    while (remain > 0) {
        switch (st->state) {
        case ICY_AUDIO:
            if (st->metaint <= 0) {                 /* 无元数据的普通流: 直通 */
                sink(p, (size_t)remain, ctx);
                remain = 0;
                break;
            }
            if (remain < st->audio_left) {
                sink(p, (size_t)remain, ctx);
                st->audio_left -= remain;
                remain = 0;
            } else {
                sink(p, (size_t)st->audio_left, ctx);
                p += st->audio_left;
                remain -= st->audio_left;
                st->state = ICY_META_LEN;
            }
            break;

        case ICY_META_LEN: {                        /* 读 1B 长度(×16=元数据字节数) */
            st->meta_need = p[0] * 16;
            p++; remain--;
            st->meta_got = 0;
            st->state = (st->meta_need > 0) ? ICY_META_BODY : ICY_AUDIO;
            if (st->state == ICY_AUDIO) {
                st->audio_left = st->metaint;
            }
            break;
        }

        case ICY_META_BODY: {                       /* 收集元数据(可跨块) */
            int take = remain < (st->meta_need - st->meta_got)
                       ? remain : (st->meta_need - st->meta_got);
            if (st->meta_got < (int)sizeof(st->meta_buf) - 1) {
                int c = take < (int)sizeof(st->meta_buf) - 1 - st->meta_got
                        ? take : (int)sizeof(st->meta_buf) - 1 - st->meta_got;
                memcpy(st->meta_buf + st->meta_got, p, c);
            }
            st->meta_got += take;
            p += take; remain -= take;
            if (st->meta_got >= st->meta_need) {
                st->meta_buf[st->meta_got < (int)sizeof(st->meta_buf)
                             ? st->meta_got : (int)sizeof(st->meta_buf) - 1] = 0;
                parse_title(st, st->meta_buf);
                st->state = ICY_AUDIO;
                st->audio_left = st->metaint;
            }
            break;
        }
        }
    }
}
