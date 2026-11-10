# esp_net_radio —— ESP32 网络电台

基于 ESP-IDF 的极简网络电台播放器：上电连 WiFi，自动轮播内置电台列表。

- **主控**：ESP32-P4（v3.x）@400MHz，16MB Flash + 32MB HEX PSRAM
- **开发板**：[鱼鹰光电 OSPTEK ESP32-P4C5 开发板](https://gitee.com/osptek/esp32-p4c5-module-dev-board)
  （ESP32-P4 + ESP32-C5 共芯，WiFi 由 C5 经 SDIO/esp-hosted 提供）
- **音频**：板载 ES8311 codec + NS4150B 功放（48kHz 立体声，软件混音引擎）

## 功能

- HTTP/ICY 直连流 + HLS(m3u8) 直播流，MP3(Helix 定点) / AAC(esp_audio_codec) 解码
- 内置 4 个国内电台（CNR 卫星拉流 / 蜻蜓 FM / 喜马拉雅），断流指数退避重连
- 10ms 帧 Q15 定点混音引擎（ducking/多音源），网络抖动缓冲 10s(PSRAM)
- C5 共芯 esp_hosted 3.0.10（显式两段式初始化，同版本配对）

## 分层架构

```
main(装配) ─ app_player(解码/播放列表) ─ app_audio(混音引擎) ─ drv_audio(ES8311)
   └──────── app_net(WiFi over C5, esp_hosted) ─ cubecore(事件总线/环形缓冲/重采样)
                mp3_helix(定点 MP3 解码, vendored)   boardcfg(板级引脚)
```

## 构建与烧录

```bash
# 依赖: ESP-IDF v6.1 (esp32p4 target)
idf.py set-target esp32p4
idf.py menuconfig   # Radio P4C5 → 填 WiFi SSID/密码(凭据只存本地 sdkconfig)
idf.py build
idf.py -p /dev/ttyACM0 -b 230400 flash monitor   # CH343P, no-stub 烧写
```

C5 侧需刷 esp-hosted slave 固件（本板为 3.0.10；参考
[esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) 或鱼鹰官方
`ESP32-C5_ESP-Hosted_v2.12.9` 镜像与升级包）。P4 侧 sdkconfig 需
`CONFIG_SLAVE_IDF_TARGET_ESP32C5=y`（默认 C6 会芯片枚举失配）。

## 致谢与谱系

本项目从 [EspCube](https://github.com/embalmer-Y)（ESP32-S31 AI 音箱）的
电台链路拆分而来，板级初始化序列参考了同款开发板上真机验证过的
Kaleido 工程实践。

## 许可

[MIT](./LICENSE)。注意：`components/mp3_helix/` 为 vendored
[libhelix-mp3](https://github.com/ultraembedded/libhelix_mp3)，
沿用其原始许可；托管组件（esp-hosted/esp_codec_dev/esp_audio_codec 等）
遵循各自许可。
