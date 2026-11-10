# app_audio [L2 服务]: 音频引擎(混音器 + ducking + AEC 参考抽头 + 麦克风服务)
idf_component_register(
    SRCS "audio_mixer.c" "audio_mic.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore boardcfg drv_audio
        esp_common freertos
)
