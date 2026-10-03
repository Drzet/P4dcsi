idf_component_register(
    SRCS
        "main.c"
        "display_rpi.c"
        "camera_stream.c"
        "camera_diag.c"
        "isp_clock_test.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_mm
        esp_timer
        esp_hw_support
        esp_driver_isp
        esp_driver_i2c
        esp_hal_lcd
        espressif__esp_video
        espressif__esp_cam_sensor
)

# esp_cam_sensor 2.0.1 uses link-time sensor registration.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov9281_detect")

# Override only the HD RAW8 ISP creation call; leave managed components intact.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_isp_new_processor"
    "-u __wrap_esp_isp_new_processor"
)
