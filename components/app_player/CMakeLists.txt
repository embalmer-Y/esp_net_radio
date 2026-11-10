# app_player [L3 应用]: 网络音源(HTTP/ICY 流 + 解码 → 混音器)
idf_component_register(
    SRCS "app_player.c" "icy_stream.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore app_audio app_net espressif__esp_audio_codec mp3_helix espressif__esp_hls_stream espressif__esp_extractor espressif__gmf_core espressif__gmf_io espressif__media_lib_sal esp_timer
        esp_http_client esp-tls
)
