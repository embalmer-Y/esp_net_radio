# app_net [L2 服务]: WiFi STA over C5(esp_hosted 3.0.10, SDIO)
# esp_wifi API 经 esp_wifi_remote 透传, 与 S31 版同形; 头文件 API 与
# firmware/components/app_net/include/app_net.h 对齐(app_player 同源复用)
idf_component_register(
    SRCS "app_net.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore esp_wifi esp_netif esp_event nvs_flash esp_timer
                  espressif__esp_hosted
)
