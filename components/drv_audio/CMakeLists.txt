# drv_audio [L1 驱动]: ES8311+NS4150B @48k 立体声(esp_codec_dev 1.6.2,
# Kaleido 同款板实证序列); 对上暴露与 firmware/drv_audio 相同 API,
# app_audio 混音器零改动复用
idf_component_register(
    SRCS "drv_audio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES boardcfg esp_driver_i2s esp_driver_i2c esp_driver_gpio
                  espressif__esp_codec_dev
)
