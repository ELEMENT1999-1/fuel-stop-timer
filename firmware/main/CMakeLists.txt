idf_component_register(
    SRCS "main.c" "ui.c" "seg7.c" "fuel_timer.c" "buzzer.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2s
        esp_timer
        esp_lcd
        i2c
        io_extension
        rgb_lcd_port
        touch
        lvgl_port
        lvgl
        esp_codec_dev
)
