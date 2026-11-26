/**
 * @file buzzer.c
 * @brief ES8311 + 板载功放发声：2kHz 方波、音量 100
 *
 * 关键点：
 *  - 功放（EXIO3）与 codec 静音有几十 ms 的唤醒延迟，所以支持"预热"：
 *    最后 5 秒前就打开功放并解除静音（I2S 自动送静音数据，不会出声），
 *    这样 3/2/1 的短音是即时的，不会被吞掉。
 *  - 同时驱动隔离输出 DO0（外接 5~24V 有源蜂鸣器/警灯）。
 */

#include "buzzer.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c.h"
#include "io_extension.h"

static const char *TAG = "buzzer";

#define BZ_I2S_PORT       I2S_NUM_1
#define BZ_MCLK           GPIO_NUM_6
#define BZ_BCLK           GPIO_NUM_44
#define BZ_WS             GPIO_NUM_16
#define BZ_DOUT           GPIO_NUM_15
#define BZ_SAMPLE_RATE    16000
#define BZ_TONE_HZ        2000
#define BZ_PA_IO          IO_EXTENSION_IO_3
#define BZ_BUF_FRAMES     256

static esp_codec_dev_handle_t s_dev = NULL;
static i2s_chan_handle_t      s_tx = NULL;
static volatile int64_t       s_beep_until_us = 0;
static volatile bool          s_armed = false;
static volatile bool          s_do0 = false;

static inline int16_t square_step(uint32_t *phase)
{
    const uint32_t period = BZ_SAMPLE_RATE / BZ_TONE_HZ;
    int16_t v = (*phase < period / 2) ? 30000 : -30000;
    *phase = (*phase + 1) % period;
    return v;
}

static void beep_task(void *arg)
{
    (void)arg;
    int16_t *buf = heap_caps_malloc(BZ_BUF_FRAMES * 2 * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no tone buffer");
        vTaskDelete(NULL);
        return;
    }

    uint32_t phase = 0;
    bool amp_on = false;
    bool do0_on = false;

    while (1) {
        bool sounding = (esp_timer_get_time() < s_beep_until_us);
        bool want_amp = sounding || s_armed;

        if (want_amp != amp_on) {
            amp_on = want_amp;
            if (amp_on) {
                IO_EXTENSION_Output(BZ_PA_IO, 1);
                esp_codec_dev_set_out_mute(s_dev, false);
                ESP_LOGI(TAG, "amp on%s", sounding ? " (beep)" : " (warmup)");
            } else {
                esp_codec_dev_set_out_mute(s_dev, true);
                IO_EXTENSION_Output(BZ_PA_IO, 0);
                ESP_LOGI(TAG, "amp off");
            }
        }

        if (sounding != do0_on) {
            do0_on = sounding;
            IO_EXTENSION_Output(DO0, do0_on ? 1 : 0);
        }

        if (sounding) {
            for (int i = 0; i < BZ_BUF_FRAMES; i++) {
                int16_t v = square_step(&phase);
                buf[2 * i]     = v;
                buf[2 * i + 1] = v;
            }
            esp_codec_dev_write(s_dev, buf, BZ_BUF_FRAMES * 2 * sizeof(int16_t));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void buzzer_init(void)
{
    esp_err_t ret;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BZ_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ret = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(ret));
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BZ_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BZ_MCLK,
            .bclk = BZ_BCLK,
            .ws   = BZ_WS,
            .dout = BZ_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std: %s", esp_err_to_name(ret));
        return;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BZ_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = EXAMPLE_I2C_MASTER_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = DEV_I2C_Get_Bus_Device(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) {
        ESP_LOGE(TAG, "codec interfaces not created");
        return;
    }

    esp_codec_dev_hw_gain_t hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hw_gain,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = BZ_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    ret = esp_codec_dev_open(s_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec open: %s", esp_err_to_name(ret));
        return;
    }
    esp_codec_dev_set_out_vol(s_dev, 100);
    esp_codec_dev_set_out_mute(s_dev, true);

    IO_EXTENSION_Output(DO0, 0);
    IO_EXTENSION_Output(BZ_PA_IO, 0);

    xTaskCreatePinnedToCore(beep_task, "beep", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "ready (tone %d Hz, vol 100, warmup capable)", BZ_TONE_HZ);
}

void buzzer_beep(uint32_t ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    if (until > s_beep_until_us) {
        s_beep_until_us = until;
    }
}

void buzzer_arm(bool on)
{
    s_armed = on;
}

bool buzzer_is_active(void)
{
    return esp_timer_get_time() < s_beep_until_us;
}

void buzzer_stop(void)
{
    s_beep_until_us = 0;
}