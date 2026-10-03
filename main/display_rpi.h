#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4D_LCD_H_RES 800
#define P4D_LCD_V_RES 480
#define P4D_LCD_BYTES_PER_PIXEL 3
#define P4D_LCD_FB_SIZE ((size_t)P4D_LCD_H_RES * P4D_LCD_V_RES * P4D_LCD_BYTES_PER_PIXEL)

/**
 * Bring up a Raspberry-Pi-style 800x480 MIPI-DSI panel using the
 * ATTINY88 (0x45) + TC358762 bridge sequence used by the RPi 7" V1
 * and compatible "driver-free" clones.
 *
 * The caller owns the shared I2C bus.
 */
esp_err_t p4d_display_init(
    i2c_master_bus_handle_t i2c_bus,
    uint8_t **ret_framebuffer,
    size_t *ret_framebuffer_size);

/** Flush CPU cache writes so the DSI/DPI engine sees the updated framebuffer. */
esp_err_t p4d_display_sync(void);

/** Backlight PWM handled by the panel control MCU, range 0..255. */
esp_err_t p4d_display_brightness_set(int brightness);

#ifdef __cplusplus
}
#endif
