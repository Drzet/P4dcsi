#include <stdint.h>
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

#include "camera_stream.h"
#include "display_rpi.h"

static const char *TAG = "P4dcsi";

#define BOARD_I2C_PORT       I2C_NUM_0
#define BOARD_I2C_SDA        7
#define BOARD_I2C_SCL        8

#define CAM_W                1280
#define CAM_H                720

/* 1280x720 -> 800x450 exactly preserves 16:9. */
#define PREVIEW_W            800
#define PREVIEW_H            450
#define PREVIEW_X_OFFSET     0
#define PREVIEW_Y_OFFSET     ((P4D_LCD_V_RES - PREVIEW_H) / 2)

static uint8_t *s_display_fb;
static size_t s_display_fb_size;
static uint32_t s_capture_frames;
static uint32_t s_display_frames;

static esp_err_t shared_i2c_init(i2c_master_bus_handle_t *ret_bus)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&cfg, ret_bus);
}

static void probe_addr(i2c_master_bus_handle_t bus, uint8_t addr, const char *name)
{
    esp_err_t err = i2c_master_probe(bus, addr, 50);
    ESP_LOGI(
        TAG,
        "I2C 0x%02X %-18s : %s",
        addr,
        name,
        err == ESP_OK ? "present" : "not found");
}

static void diagnostic_i2c_probe(i2c_master_bus_handle_t bus)
{
    probe_addr(bus, 0x45, "panel MCU");
    probe_addr(bus, 0x38, "touch");
    probe_addr(bus, 0x60, "OV9281 SID-low");
    probe_addr(bus, 0x30, "alternate camera");
}

static void camera_frame(
    const uint8_t *data,
    size_t len,
    uint32_t width,
    uint32_t height,
    void *user_ctx)
{
    (void)user_ctx;

    ++s_capture_frames;

    if (width != CAM_W || height != CAM_H || len < (size_t)(CAM_W * CAM_H)) {
        static bool warned = false;
        if (!warned) {
            ESP_LOGE(
                TAG,
                "unexpected camera frame: %ux%u len=%u",
                (unsigned)width,
                (unsigned)height,
                (unsigned)len);
            warned = true;
        }
        return;
    }

    /*
     * 1280->800 and 720->450 are both exactly 5/8.
     * Use nearest-neighbour sampling for the bring-up firmware.
     * There is deliberately no frame limiter: every captured frame updates
     * the framebuffer; the DSI engine displays whichever image is current.
     */
    for (uint32_t y = 0; y < PREVIEW_H; ++y) {
        const uint32_t src_y = (y * 8U) / 5U;
        const uint8_t *src = data + (size_t)src_y * CAM_W;
        uint8_t *dst = s_display_fb +
            (((size_t)(y + PREVIEW_Y_OFFSET) * P4D_LCD_H_RES + PREVIEW_X_OFFSET) *
             P4D_LCD_BYTES_PER_PIXEL);

        for (uint32_t x = 0; x < PREVIEW_W; ++x) {
            const uint32_t src_x = (x * 8U) / 5U;
            const uint8_t gray = src[src_x];
            dst[0] = gray;
            dst[1] = gray;
            dst[2] = gray;
            dst += P4D_LCD_BYTES_PER_PIXEL;
        }
    }

    if (p4d_display_sync() != ESP_OK) {
        ESP_LOGE(TAG, "display cache sync failed");
        return;
    }

    ++s_display_frames;
    if ((s_display_frames % 50U) == 0) {
        ESP_LOGI(
            TAG,
            "live: capture=%lu display=%lu, 1280x720 RAW8 -> 800x450 RGB888",
            (unsigned long)s_capture_frames,
            (unsigned long)s_display_frames);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "WT9932P4-TINY + OV9281 + iPistBit 800x480 DSI test");

    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t err = shared_i2c_init(&i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    err = p4d_display_init(i2c_bus, &s_display_fb, &s_display_fb_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        diagnostic_i2c_probe(i2c_bus);
        return;
    }

    ESP_LOGI(TAG, "display framebuffer size=%u", (unsigned)s_display_fb_size);

    err = p4d_camera_init(i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        diagnostic_i2c_probe(i2c_bus);
        return;
    }

    diagnostic_i2c_probe(i2c_bus);

    err = p4d_camera_start(camera_frame, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera stream start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "end-to-end test running");
}
