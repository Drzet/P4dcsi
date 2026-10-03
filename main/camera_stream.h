#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*p4d_camera_frame_cb_t)(
    const uint8_t *data,
    size_t len,
    uint32_t width,
    uint32_t height,
    void *user_ctx);

/**
 * Initialize OV9281 through esp_video using the caller's shared I2C bus.
 * On WT9932P4-TINY GPIO0 enables the camera power/reset rail.
 */
esp_err_t p4d_camera_init(i2c_master_bus_handle_t i2c_bus);

/** Start the MIPI-CSI capture task. */
esp_err_t p4d_camera_start(p4d_camera_frame_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
