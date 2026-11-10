/*
 * main.c —— radio-p4c5 装配层(网络电台最小闭环)
 *
 * 裁剪自 EspCube S31 firmware/main/main.c 的电台最小集:
 *   NVS → 音频链(ES8311@48k → 混音器) → 电台播放器 → WiFi(over C5)
 *   → WIFI_CONNECTED 事件 → 播放列表轮播。
 * 无 UI/BT/语音(P4 无蓝牙; 本 App 不含显示)。
 *
 * 失败域策略(docs/06 §6): 音频链 abort; 网络/播放器降级。
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "cubecore.h"
#include "nvs_flash.h"
#include "drv_audio.h"
#include "audio_mixer.h"
#include "app_player.h"
#include "app_net.h"

static const char *TAG = "main";

/* WiFi 就绪 → 启动播放列表(一次性任务: app_player_start 内含停止等待,
 * 不可在事件回调里同步调用 —— 会阻塞所有事件分发) */
static void player_start_task(void *arg)
{
    (void)arg;
    app_player_start(NULL);         /* 无参 = 播放列表轮播 */
    vTaskDelete(NULL);
}

static void radio_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                         void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id == CUBE_EVT_WIFI_CONNECTED) {
        ESP_LOGI(TAG, "EVT: WiFi connected, start radio");
        BaseType_t r = xTaskCreate(player_start_task, "pstart", 4096, NULL,
                                   8, NULL);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "player start task create failed");
        }
    }
}

static void app_start_banner(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " radio-p4c5 — EspCube 网络电台 App");
    ESP_LOGI(TAG, " Board : OSPTEK ESP32-P4C5 (P4 v3.x + C5, hosted 3.0.10)");
    ESP_LOGI(TAG, " Audio : ES8311 + NS4150B (board builtin)");
    ESP_LOGI(TAG, "=================================================");
}

void app_main(void)
{
    app_start_banner();

    /* ---- 1. NVS ---- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* ---- 2. 音频链(核心, 失败 abort; 混音任务先起, 成功后才开 PA) ---- */
    drv_audio_cfg_t acfg = {
        .sample_rate = CONFIG_ESP_CUBE_MIXER_RATE,
        .channels = 2,
    };
    ESP_ERROR_CHECK(drv_audio_init(&acfg));
    audio_mixer_cfg_t mcfg = {
        .sample_rate = CONFIG_ESP_CUBE_MIXER_RATE,
        .channels = 2,
    };
    ESP_ERROR_CHECK(audio_mixer_init(&mcfg));
    audio_mixer_set_master_volume(CONFIG_ESP_CUBE_DEFAULT_VOLUME);
    ESP_ERROR_CHECK(audio_mixer_start());
    ESP_ERROR_CHECK(drv_audio_pa_set(true));

    /* ---- 3. 电台播放器 ---- */
    ESP_ERROR_CHECK(app_player_init());

    /* ---- 4. WiFi over C5(降级不 abort; 失败仅无网络) ---- */
    esp_err_t nerr = app_net_start();
    ESP_LOGI(TAG, "wifi: %s", nerr == ESP_OK ? "starting" : esp_err_to_name(nerr));

    /* ---- 5. 事件桥 ---- */
    cube_event_subscribe(radio_evt_cb, NULL);

    ESP_LOGI(TAG, "startup complete, free heap=%u / psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
