/*
 * drv_audio.c —— ES8311 + NS4150B 音频驱动(P4C5 板, esp_codec_dev 1.6.2)
 *
 * 初始化序列逐行移植自 Kaleido voice-loop-p4/main/audio_io.c(同款鱼鹰
 * P4C5 板真机实证, V-38 点板记录), 差异仅三点:
 *   - 采样率 16000 → 48000(电台混音主时钟), dma 8×480帧(80ms);
 *   - 只开 OUT 设备(电台 App 无录音, IN 不创建);
 *   - PA(GPIO53) 驱动内配置在本板不生效, open 后手工推挽拉高(V-38 实证)。
 */
#include "drv_audio.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "board_pins.h"

static const char *TAG = "drv_audio";

static esp_codec_dev_handle_t s_out;

esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg)
{
    if (s_out != NULL) return ESP_OK;
    ESP_RETURN_ON_FALSE(cfg != NULL && cfg->sample_rate > 0,
                        ESP_ERR_INVALID_ARG, TAG, "cfg");

    /* ---- 1. I2C 主控(与触摸共总线, 内部上拉兜底) ---- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = BOARD_PIN_CODEC_I2C_SDA,
        .scl_io_num = BOARD_PIN_CODEC_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    /* ---- 2. I2S 标准模式全双工 ----
     * auto_clear 必开: 欠载时 DMA 默认循环重发尾缓冲(实测语音末字无限
     * 复读), 开自动清零则发静音; 8 描述符×480帧@48k ≈ 80ms 吸收抢占抖动 */
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s chan");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_PIN_CODEC_MCLK,
            .bclk = BOARD_PIN_CODEC_SCLK,
            .ws = BOARD_PIN_CODEC_WS,
            .dout = BOARD_PIN_CODEC_DOUT,
            .din = BOARD_PIN_CODEC_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "tx en");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "rx en");

    /* ---- 3. codec 数据/控制接口 + ES8311 ---- */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = 0, .rx_handle = rx, .tx_handle = tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_FAIL, TAG, "i2s data if");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
        .clock_speed_hz = 400000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if != NULL, ESP_FAIL, TAG, "i2c ctrl if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .pa_pin = BOARD_PIN_AMP_EN,
        .pa_reverted = false,
        .hw_gain = { .pa_voltage = 5.0 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_FAIL, TAG, "es8311 new");

    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_out = esp_codec_dev_new(&out_cfg);
    ESP_RETURN_ON_FALSE(s_out != NULL, ESP_FAIL, TAG, "codec dev");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = cfg->sample_rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_codec_dev_set_out_vol(s_out, 70.0);
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_out, &fs), TAG, "codec open");

    /* PA 使能脚手工补配(V-38): es8311 驱动对 pa_pin 的 gpio 配置在本板
     * 不生效(疑与启动期全脚 sleep 隔离冲突), open 后手动推挽输出拉高 */
    gpio_config_t g = {
        .pin_bit_mask = 1ULL << BOARD_PIN_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&g);
    gpio_set_level(BOARD_PIN_AMP_EN, 0);          /* 先关, 待混音任务起 */

    ESP_LOGI(TAG, "ES8311 ready: %dHz/2ch (PA=%d manual, NS4150B)",
             cfg->sample_rate, BOARD_PIN_AMP_EN);
    return ESP_OK;
}

int drv_audio_output(const int16_t *samples, int frames)
{
    if (s_out == NULL || samples == NULL) return 0;
    esp_err_t r = esp_codec_dev_write(s_out, (void *)samples,
                                      (size_t)frames * 2 * sizeof(int16_t));
    return r == ESP_OK ? frames : 0;
}

int drv_audio_mic_read(int16_t *buf, int frames)
{
    (void)buf; (void)frames;
    return 0;                   /* 本 App 无语音 */
}

esp_err_t drv_audio_set_volume(int vol)
{
    if (s_out == NULL) return ESP_ERR_INVALID_STATE;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return esp_codec_dev_set_out_vol(s_out, (float)vol);
}

esp_err_t drv_audio_pa_set(bool on)
{
    gpio_set_level(BOARD_PIN_AMP_EN, on ? 1 : 0);
    return ESP_OK;
}
