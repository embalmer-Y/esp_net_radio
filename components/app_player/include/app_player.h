/*
 * app_player.h —— 网络音频播放器(HTTP/ICY 流 → 解码 → 混音器)
 *
 * 支持: MP3 / AAC(ADTS) 网络电台与点播流;ICY StreamTitle 元数据上报 UI。
 * 健壮性: 断流自动重连(指数退避, ≤5 次);解码欠载由混音器侧静音保护。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_CONNECTING,
    PLAYER_PLAYING,
    PLAYER_ERROR,
} app_player_state_t;

/* 开始播放(url 为 http/https 流地址; 传 NULL = 按内部播放列表轮播) */
esp_err_t app_player_start(const char *url);

/* 初始化(订阅 BT 接管事件);幂等, 须在 app_bt_start 之前调用 */
esp_err_t app_player_init(void);

/* 停止播放 */
esp_err_t app_player_stop(void);

/* ---- 播放列表(电台单一来源; UI/装配层经此枚举, 不自带列表副本) ---- */
int app_player_station_num(void);               /* 列表长度(≥1) */
const char *app_player_station_name(int idx);   /* 台名; NULL=越界 */
int app_player_station_idx(void);               /* 当前下标; -1=列表外单次播放 */
esp_err_t app_player_play_index(int idx);       /* 播放列表指定项(越界回绕) */

/* 当前状态与元数据 */
app_player_state_t app_player_get_state(void);
const char *app_player_get_url(void);

#ifdef __cplusplus
}
#endif
