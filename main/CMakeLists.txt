idf_component_register(
    SRCS
        "main.c"
        "display_rpi.c"
        "camera_stream.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_mm
        esp_timer
        esp_hw_support
        esp_driver_i2c
        esp_hal_lcd
        espressif__esp_video
        espressif__esp_cam_sensor
)

# esp_cam_sensor 2.0.1 uses link-time sensor registration.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov9281_detect")
