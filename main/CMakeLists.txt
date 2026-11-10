# radio-p4c5 main —— 装配层(电台最小闭环, 裁剪自 S31 firmware/main)
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        cubecore boardcfg
        drv_audio app_audio app_player app_net
        nvs_flash
)
