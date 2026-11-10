/*
 * board_pins.h —— OSPTEK ESP32-P4C5 开发板引脚(radio App 子集)
 *
 * 依据: 底板原理图 V1.3 + Kaleido 26 号调研三重验证(同款板真机实测),
 * 归档: project-archive ... Kaleido/docs/research/(26 号平台调研)。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 音频(ES8311 + NS4150B, 底板 U13) =====================
 * I2C 与 in-cell 触摸共总线(SDA=7/SCL=8), 内部上拉兜底 */
#define BOARD_PIN_CODEC_I2C_SDA   7       /* GPIO7 */
#define BOARD_PIN_CODEC_I2C_SCL   8       /* GPIO8 */
#define BOARD_PIN_CODEC_MCLK      13      /* I2S0_MCLK → ES8311 MCLK */
#define BOARD_PIN_CODEC_SCLK      12      /* I2S0_SCLK (BCLK) */
#define BOARD_PIN_CODEC_WS        10      /* I2S0_LRCK (WS) */
#define BOARD_PIN_CODEC_DOUT      9       /* I2S0_SDOUT: P4 → ES8311 DAC */
#define BOARD_PIN_CODEC_DIN       11      /* I2S0_DSDIN: ES8311 ADC → P4 */
#define BOARD_PIN_AMP_EN          53      /* NS4150B STD(高有效; 驱动配置
                                           * 在本板不生效, open 后手工拉高) */

/* I2C 地址(espressif/esp_codec_dev 1.6.2: ES8311_CODEC_DEFAULT_ADDR 即此) */
#define BOARD_CODEC_I2C_ADDR      0x30    /* ES8311 7位地址0x18 的 8位形式 */

/* ===================== C5 共芯(SDIO, esp_hosted P4 默认值) =====================
 * CLK=18 CMD=19 D0..D3=14..17 复位=GPIO54 —— 与 esp_hosted 在 P4 的
 * 无预设默认完全一致, app_net 无需引脚配置 */
#define BOARD_PIN_C5_SDIO_CLK     18
#define BOARD_PIN_C5_SDIO_CMD     19
#define BOARD_PIN_C5_SDIO_D0      14
#define BOARD_PIN_C5_SDIO_D1      15
#define BOARD_PIN_C5_SDIO_D2      16
#define BOARD_PIN_C5_SDIO_D3      17
#define BOARD_PIN_C5_RESET        54      /* 高有效, host 每次启动复位 C5 */

#ifdef __cplusplus
}
#endif
