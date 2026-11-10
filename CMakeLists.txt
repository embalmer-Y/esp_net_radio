# radio-p4c5 —— 网络电台独立 App(ESP32-P4 + C5, 鱼鹰 P4C5 开发板)
#
# 与 firmware/(S31 主工程)同源复用: components/ 下 4 个 symlink
# (cubecore/mp3_helix/app_player/app_audio 平台无关, 盘点见
# docs/06 分层与 2026-11-12 移植调研); 板差异集中在本地
# boardcfg/drv_audio(ES8311@48k)/app_net(esp_hosted WiFi over C5)。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(radio_p4c5)
