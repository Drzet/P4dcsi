#include <stdint.h>
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_stream.h"
#include "display_rpi.h"

static const char *TAG = "P4dcsi";

#define BOARD_I2C_PORT       I2C_NUM_0
#define BOARD_I2C_SDA        7
#define BOARD_I2C_SCL        8

#define CAM_W                640
#define CAM_H                400
#define DISPLAY_EVERY_N      4
#define CANVAS_BUF_BYTES     (CAM_W * CAM_H * sizeof(uint16_t))

static lv_obj_t *s_canvas;
static lv_obj_t *s_status;
static uint16_t *s_canvas_buf[2];
static int s_front_buf;
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
    probe_addr(bus, 0x30, "alt camera addr");
}

static void ui_set_status(const char *text)
{
    if (s_status == NULL) {
        return;
    }

    if (lvgl_port_lock(1000)) {
        lv_label_set_text(s_status, text);
        lvgl_port_unlock();
    }
}

static esp_err_t ui_init(lv_display_t *disp)
{
    s_canvas_buf[0] = heap_caps_malloc(
        CANVAS_BUF_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_canvas_buf[1] = heap_caps_malloc(
        CANVAS_BUF_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_canvas_buf[0] == NULL || s_canvas_buf[1] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < CAM_W * CAM_H; ++i) {
        s_canvas_buf[0][i] = 0;
        s_canvas_buf[1][i] = 0;
    }

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);

    s_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(
        s_canvas,
        s_canvas_buf[0],
        CAM_W,
        CAM_H,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);

    s_status = lv_label_create(screen);
    lv_label_set_text(s_status, "P4dcsi: display ready, starting OV9281...");
    lv_obj_set_style_text_color(s_status, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 8);

    lvgl_port_unlock();
    return ESP_OK;
}

static inline uint16_t gray_to_rgb565(uint8_t gray)
{
    return (uint16_t)(
        ((uint16_t)(gray >> 3) << 11) |
        ((uint16_t)(gray >> 2) << 5) |
        (uint16_t)(gray >> 3));
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

    if ((s_capture_frames % DISPLAY_EVERY_N) != 0) {
        return;
    }

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

    const int back = 1 - s_front_buf;
    uint16_t *dst = s_canvas_buf[back];

    for (size_t i = 0; i < (size_t)(CAM_W * CAM_H); ++i) {
        dst[i] = gray_to_rgb565(data[i]);
    }

    if (lvgl_port_lock(1000)) {
        lv_canvas_set_buffer(
            s_canvas,
            dst,
            CAM_W,
            CAM_H,
            LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s_canvas);

        ++s_display_frames;
        if ((s_display_frames % 25) == 0) {
            lv_label_set_text_fmt(
                s_status,
                "OV9281 live  %ux%u  capture=%lu  display=%lu",
                (unsigned)width,
                (unsigned)height,
                (unsigned long)s_capture_frames,
                (unsigned long)s_display_frames);
        }

        s_front_buf = back;
        lvgl_port_unlock();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "WT9932P4-TINY + OV9281 + 800x480 Pi-style DSI test");

    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t err = shared_i2c_init(&i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    lv_display_t *disp = NULL;
    err = p4d_display_init(i2c_bus, &disp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }

    err = ui_init(disp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
        return;
    }

    err = p4d_camera_init(i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        ui_set_status("OV9281 init FAILED - see serial log");
        diagnostic_i2c_probe(i2c_bus);
        return;
    }

    diagnostic_i2c_probe(i2c_bus);

    err = p4d_camera_start(camera_frame, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera stream start failed: %s", esp_err_to_name(err));
        ui_set_status("OV9281 stream FAILED - see serial log");
        return;
    }

    ui_set_status("OV9281 stream started - waiting for frames");
    ESP_LOGI(TAG, "end-to-end test running");
}
