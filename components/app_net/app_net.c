/*
 * app_net.c —— WiFi STA over C5(esp_hosted 3.0.10, P4C5 版)
 *
 * 与 S31 版(firmware/components/app_net)的差异:
 *   - P4 无射频: esp_wifi API 全部经 esp_wifi_remote → SDIO RPC 到 C5;
 *   - 初始化两段式(Kaleido 缺陷 H 终局): AUTO_CALL_INIT_BEFORE_APP_MAIN=n,
 *     app_main 里显式 esp_hosted_init() → esp_hosted_connect_to_slave()
 *     (阻塞 ~1.5s, 含 C5 复位+枚举+RPC 建链), 失败降级不 abort;
 *   - 删 BT 让渡/PS/coex(P4 无蓝牙); 重连策略(事件不阻塞+指数退避)照搬。
 */
#include "app_net.h"

#include <stdbool.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"
#include "esp_hosted.h"

static const char *TAG = "net";

#define WIFI_RETRY_MS_BASE  2000
#define WIFI_RETRY_MAX_SHIFT 4      /* 最大退避 = 基准 × 2^4 = 32s */

static esp_timer_handle_t s_reconnect_timer;
static volatile int s_retry_count = 0;
static volatile bool s_got_ip;
static volatile bool s_inited;

/* esp_timer 回调上下文(定时器任务): 只做一次非阻塞 API 调用 */
static void wifi_reconnect_cb(void *arg)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();             /* 事件驱动首次连接 */
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            int shift = s_retry_count > WIFI_RETRY_MAX_SHIFT
                        ? WIFI_RETRY_MAX_SHIFT : s_retry_count;
            int delay_ms = WIFI_RETRY_MS_BASE << shift;
            s_retry_count++;
            s_got_ip = false;
            ESP_LOGW(TAG, "disconnected, retry in %dms(#%d)", delay_ms,
                     s_retry_count);
            cube_event_publish(CUBE_EVT_WIFI_DISCONNECTED, NULL, 0);
            (void)esp_timer_stop(s_reconnect_timer);   /* 未激活时无害 */
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;
        s_got_ip = true;
        cube_event_publish(CUBE_EVT_WIFI_CONNECTED, NULL, 0);
    }
}

bool app_net_is_up(void)
{
    return s_got_ip;
}

esp_err_t app_net_wait_up(int timeout_ms)
{
    int waited = 0;
    while (!s_got_ip && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_got_ip ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_net_suspend(void) { return ESP_OK; }   /* P4 无 BT */
esp_err_t app_net_resume(void) { return ESP_OK; }

esp_err_t app_net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "nvs init");
    }

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_reconnect_cb,
        .name = "wifi_reconn",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer),
                        TAG, "timer");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    /* ---- esp_hosted 两段式(Kaleido 缺陷 H 终局口径) ----
     * SDIO slot1/4bit/40MHz/复位54 = esp_hosted 在 P4 的默认值, 零配置 */
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "hosted init");
    esp_err_t cerr = esp_hosted_connect_to_slave();   /* 阻塞 ~1.5s */
    if (cerr != ESP_OK) {
        /* 降级: 无线不可用不阻断音频框架启动(C5 可能未就绪/线序异常) */
        ESP_LOGE(TAG, "hosted connect failed: %s — wifi degraded",
                 esp_err_to_name(cerr));
        return cerr;
    }
    ESP_LOGI(TAG, "hosted link up (C5 slave 3.0.10)");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "reg wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "reg ip evt");

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_ESP_CUBE_WIFI_SSID,
            .password = CONFIG_ESP_CUBE_WIFI_PASSWORD,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG,
                        "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "wifi sta started, connecting to \"%s\"",
             CONFIG_ESP_CUBE_WIFI_SSID);
    s_inited = true;
    return ESP_OK;
}
