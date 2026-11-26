/**
 * @file main.c
 * @brief 赛车加油计时器 —— ESP32-S3-Touch-LCD-4.3C
 *
 * 硬件（与原理图/官方 BSP 一致）：
 *   LCD   : 800x480 RGB565, ST7262 面板，RGB 并行接口
 *   Touch : GT911 (I2C 0x5D)
 *   IO 扩展: CH422G (I2C 0x24) —— 背光 / 触摸复位 / 功放使能 / 隔离 IO
 *   音频  : ES8311 + 板载功放（I2S1）
 *   传感器: 隔离数字输入 DI0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "rgb_lcd_port.h"
#include "gt911.h"
#include "io_extension.h"
#include "lvgl_port.h"

#include "ui.h"
#include "fuel_timer.h"
#include "buzzer.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "boot: fuel stop timer");

    /* 1) I2C 总线 + IO 扩展（背光、触摸复位、功放、隔离 IO 都挂在它上面） */
    DEV_I2C_Init();
    IO_EXTENSION_Init();

    /* 2) DI0 / DI1 设为输入，其余 EXIO 保持输出
     *    注意：模式寄存器里「0 = 输入」 */
    IO_EXTENSION_IO_Mode((uint8_t)~((1U << DI0) | (1U << DI1)));

    /* 3) 先关背光，等界面画好再点亮，避免开机花屏 */
    waveshare_rgb_lcd_bl_off();

    esp_lcd_touch_handle_t tp = touch_gt911_init(DEV_I2C_Get_Bus_Device());
    if (!tp) {
        ESP_LOGE(TAG, "GT911 init failed");
    }
    esp_lcd_panel_handle_t panel = waveshare_esp32_s3_rgb_lcd_init();
    ESP_ERROR_CHECK(lvgl_port_init(panel, tp));

    /* 4) 建界面 */
    if (lvgl_port_lock(-1)) {
        ui_create();
        lvgl_port_unlock();
    }

    /* 5) 蜂鸣器（ES8311 音源） */
    buzzer_init();

    /* 开机自检音：听到一声"嘟"= 音频通路正常；听不到 = 喇叭没接或功放有问题 */
    buzzer_beep(300);
    ESP_LOGI(TAG, "audio self-test: 300ms tone -> listen now");

    waveshare_rgb_lcd_bl_on();

    /* 6) 启动计时逻辑 */
    fuel_timer_start();

    ESP_LOGI(TAG, "ready, free heap=%u, psram free=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
