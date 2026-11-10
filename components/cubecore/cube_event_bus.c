/*
 * cube_event_bus.c —— 事件总线实现
 *
 * 实现: 固定容量订阅表 + spinlock 临界区同步派发。
 * 事件均为低频控制类(连接状态/元数据/命令), 同步派发的延迟可忽略;
 * 若未来出现高频事件, 再升级为队列化异步派发。
 */
#include "cube_event_bus.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"

#define CUBE_EVT_MAX_SUBSCRIBERS 16

typedef struct {
    cube_evt_cb_t cb;
    void *user_ctx;
} cube_sub_t;

static cube_sub_t s_subs[CUBE_EVT_MAX_SUBSCRIBERS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t cube_event_subscribe(cube_evt_cb_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CUBE_EVT_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].cb == NULL) {           /* 找到空槽位 */
            s_subs[i].cb = cb;
            s_subs[i].user_ctx = user_ctx;
            goto out_ok;
        }
    }
    ret = ESP_ERR_NO_MEM;                     /* 订阅表满 */
out_ok:
    portEXIT_CRITICAL(&s_mux);
    return ret;
}

esp_err_t cube_event_unsubscribe(cube_evt_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CUBE_EVT_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].cb == cb) {
            s_subs[i].cb = NULL;
            ret = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return ret;
}

esp_err_t cube_event_publish(cube_evt_id_t id, const void *data, size_t len)
{
    /* 快照订阅表后在锁外派发, 避免回调中再订阅导致死锁 */
    cube_sub_t snap[CUBE_EVT_MAX_SUBSCRIBERS];
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CUBE_EVT_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].cb != NULL) {
            snap[n++] = s_subs[i];
        }
    }
    portEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < n; i++) {
        snap[i].cb(id, data, len, snap[i].user_ctx);
    }
    return ESP_OK;
}
