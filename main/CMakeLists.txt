idf_component_register(
    SRCS
        "main.c"
        "display_rpi.c"
        "camera_stream.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_hw_support
        esp_driver_i2c
        esp_driver_gpio
        esp_hal_lcd
        espressif__esp_lvgl_port
        espressif__esp_video
        espressif__esp_cam_sensor
        lvgl__lvgl
)
