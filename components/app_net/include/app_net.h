/*
 * app_net.h —— WiFi STA 管理(P4C5: 射频在 C5 共芯, 经 esp_hosted)
 *
 * API 与 firmware/components/app_net/include/app_net.h 对齐;
 * suspend/resume(BT 让渡)在 P4 上无意义, 保留为空实现以兼容链接。
 */
#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 esp_hosted 链路(P4↔C5 SDIO, 阻塞 ~1.5s) + WiFi STA 异步连接 */
esp_err_t app_net_start(void);

/* 是否已拿到 IP */
bool app_net_is_up(void);

/* 等待拿到 IP(轮询, 100ms 步进); 超时返回 ESP_ERR_TIMEOUT */
esp_err_t app_net_wait_up(int timeout_ms);

/* BT 射频让渡: P4 无蓝牙, 空实现(兼容链接) */
esp_err_t app_net_suspend(void);
esp_err_t app_net_resume(void);

#ifdef __cplusplus
}
#endif
