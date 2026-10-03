/*
 * ESP32-P4 driver for Raspberry-Pi-style 800x480 DSI panels.
 *
 * Bring-up sequence is adapted from the working ESP32-P4 implementation:
 * https://github.com/oguzhanbaser/esp32-p4-waveshare-5inch-dsi-lcd
 * and from the Linux TC358762 / RPi panel control drivers.
 *
 * Target board here is Wireless-Tag WT9932P4-TINY.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_types.h"

#include "display_rpi.h"

static const char *TAG = "p4d_display";

#define P4D_DSI_PHY_LDO_CHAN  3
#define P4D_DSI_PHY_LDO_MV    2500

#define P4D_DSI_LANE_NUM      1
#define P4D_DSI_LANE_MBPS     600
#define P4D_DPI_CLK_MHZ       25.98

#define P4D_MODE_HFP          210
#define P4D_MODE_HSW          2
#define P4D_MODE_HBP          46
#define P4D_MODE_VFP          22
#define P4D_MODE_VSW          20
#define P4D_MODE_VBP          4

#define ATTINY_ADDR            0x45
#define ATTINY_SCL_HZ          100000
#define ATTINY_SCL_WAIT_US     50000
#define ATTINY_TIMEOUT_MS      100
#define ATTINY_RETRY           3

#define REG_ID                 0x80
#define REG_PORTA              0x81
#define REG_PORTB              0x82
#define REG_PORTC              0x83
#define REG_PWM                0x86
#define REG_ADDR_L             0x8c
#define REG_ADDR_H             0x8d
#define REG_WRITE_DATA_H       0x90
#define REG_WRITE_DATA_L       0x91

#define PA_LCD_LR              BIT(2)
#define PB_LCD_MAIN            BIT(7)
#define PC_LED_EN              BIT(0)
#define PC_RST_LCD_N           BIT(2)
#define PC_RST_BRIDGE_N        BIT(3)

#define TC_PPI_STARTPPI          0x0104
#define TC_PPI_LPTXTIMECNT       0x0114
#define TC_PPI_D0S_ATMR          0x0144
#define TC_PPI_D1S_ATMR          0x0148
#define TC_PPI_D0S_CLRSIPOCOUNT  0x0164
#define TC_PPI_D1S_CLRSIPOCOUNT  0x0168
#define TC_DSI_STARTDSI          0x0204
#define TC_DSI_LANEENABLE        0x0210
#define TC_LCDCTRL               0x0420
#define TC_LCD_HS_HBP            0x0424
#define TC_LCD_HDISP_HFP         0x0428
#define TC_LCD_VS_VBP            0x042c
#define TC_LCD_VDISP_VFP         0x0430
#define TC_SPICMR                0x0450
#define TC_SYSCTRL               0x0464

/*
 * esp_lcd_dsi_bus_t is opaque. The working reference driver accesses its HAL
 * context to emit Generic Long Write packets required by TC358762. These are
 * the first two fields in ESP-IDF 6.0.x.
 */
typedef struct {
    int bus_id;
    mipi_dsi_hal_context_t hal;
} p4d_dsi_bus_priv_t;

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_attiny;
static esp_ldo_channel_handle_t s_phy_ldo;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_handle_t s_panel;
static uint8_t *s_framebuffer;
static size_t s_framebuffer_size;
static bool s_panel_powered;

static esp_err_t attiny_write(uint8_t reg, uint8_t val)
{
    const uint8_t data[2] = {reg, val};
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < ATTINY_RETRY; ++i) {
        err = i2c_master_transmit(s_attiny, data, sizeof(data), ATTINY_TIMEOUT_MS);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    return err;
}

static esp_err_t attiny_read(uint8_t reg, uint8_t *value)
{
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < ATTINY_RETRY; ++i) {
        err = i2c_master_transmit(s_attiny, &reg, 1, ATTINY_TIMEOUT_MS);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            err = i2c_master_receive(s_attiny, value, 1, ATTINY_TIMEOUT_MS);
        }
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return err;
}

static esp_err_t panel_power_on(void)
{
    s_panel_powered = false;

    ESP_RETURN_ON_ERROR(attiny_write(REG_PORTC, 0x00), TAG, "hold resets");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(REG_PORTA, PA_LCD_LR), TAG, "set scan direction");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(REG_PORTB, PB_LCD_MAIN), TAG, "enable main rail");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(REG_PORTC, PC_LED_EN), TAG, "enable LED rail");
    vTaskDelay(pdMS_TO_TICKS(80));

    s_panel_powered = true;
    return ESP_OK;
}

static esp_err_t bridge_release_reset(void)
{
    ESP_RETURN_ON_ERROR(
        attiny_write(REG_PORTC, PC_LED_EN | PC_RST_LCD_N | PC_RST_BRIDGE_N),
        TAG, "release bridge reset");
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ATTINY SPI proxy: write TC358762 SYSPMCTRL (0x047c) = 0. */
    ESP_RETURN_ON_ERROR(attiny_write(REG_ADDR_H, 0x04), TAG, "proxy addr high");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(attiny_write(REG_ADDR_L, 0x7c), TAG, "proxy addr low");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(attiny_write(REG_WRITE_DATA_H, 0x00), TAG, "proxy data high");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(attiny_write(REG_WRITE_DATA_L, 0x00), TAG, "proxy data low");
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

static void tc358762_reg_write(uint16_t reg, uint32_t val)
{
    p4d_dsi_bus_priv_t *priv = (p4d_dsi_bus_priv_t *)s_dsi_bus;
    const uint8_t payload[6] = {
        (uint8_t)(reg >> 0),
        (uint8_t)(reg >> 8),
        (uint8_t)(val >> 0),
        (uint8_t)(val >> 8),
        (uint8_t)(val >> 16),
        (uint8_t)(val >> 24),
    };

    mipi_dsi_hal_host_gen_write_long_packet(
        &priv->hal,
        0,
        MIPI_DSI_DT_GENERIC_LONG_WRITE,
        payload,
        sizeof(payload));
}

static void tc358762_bridge_init(void)
{
    tc358762_reg_write(TC_DSI_LANEENABLE, BIT(0) | BIT(1));
    tc358762_reg_write(TC_PPI_D0S_CLRSIPOCOUNT, 0x05);
    tc358762_reg_write(TC_PPI_D1S_CLRSIPOCOUNT, 0x05);
    tc358762_reg_write(TC_PPI_D0S_ATMR, 0x00);
    tc358762_reg_write(TC_PPI_D1S_ATMR, 0x00);
    tc358762_reg_write(TC_PPI_LPTXTIMECNT, 0x03);

    tc358762_reg_write(TC_SPICMR, 0x00);
    tc358762_reg_write(TC_LCDCTRL, 0x00100150);
    tc358762_reg_write(TC_SYSCTRL, 0x040f);

    tc358762_reg_write(TC_LCD_HS_HBP, (P4D_MODE_HBP << 16) | P4D_MODE_HSW);
    tc358762_reg_write(TC_LCD_HDISP_HFP, (P4D_MODE_HFP << 16) | P4D_LCD_H_RES);
    tc358762_reg_write(TC_LCD_VS_VBP, (P4D_MODE_VBP << 16) | P4D_MODE_VSW);
    tc358762_reg_write(TC_LCD_VDISP_VFP, (P4D_MODE_VFP << 16) | P4D_LCD_V_RES);

    vTaskDelay(pdMS_TO_TICKS(100));
    tc358762_reg_write(TC_PPI_STARTPPI, 0x01);
    tc358762_reg_write(TC_DSI_STARTDSI, 0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t p4d_display_sync(void)
{
    if (s_framebuffer == NULL || s_framebuffer_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_cache_msync(
        s_framebuffer,
        s_framebuffer_size,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

esp_err_t p4d_display_brightness_set(int brightness)
{
    if (!s_panel_powered || s_attiny == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (brightness < 0) {
        brightness = 0;
    } else if (brightness > 255) {
        brightness = 255;
    }

    return attiny_write(REG_PWM, (uint8_t)brightness);
}

esp_err_t p4d_display_init(
    i2c_master_bus_handle_t i2c_bus,
    uint8_t **ret_framebuffer,
    size_t *ret_framebuffer_size)
{
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_ARG, TAG, "I2C bus is NULL");
    ESP_RETURN_ON_FALSE(ret_framebuffer != NULL, ESP_ERR_INVALID_ARG, TAG, "framebuffer pointer is NULL");
    ESP_RETURN_ON_FALSE(ret_framebuffer_size != NULL, ESP_ERR_INVALID_ARG, TAG, "framebuffer size pointer is NULL");

    s_i2c_bus = i2c_bus;

    esp_err_t probe = i2c_master_probe(s_i2c_bus, ATTINY_ADDR, 100);
    ESP_RETURN_ON_FALSE(
        probe == ESP_OK,
        ESP_ERR_NOT_FOUND,
        TAG,
        "panel controller 0x45 not found");

    const i2c_device_config_t attiny_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ATTINY_ADDR,
        .scl_speed_hz = ATTINY_SCL_HZ,
        .scl_wait_us = ATTINY_SCL_WAIT_US,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(s_i2c_bus, &attiny_cfg, &s_attiny),
        TAG,
        "add ATTINY88");

    uint8_t fw_id = 0;
    if (attiny_read(REG_ID, &fw_id) == ESP_OK) {
        ESP_LOGI(TAG, "panel MCU 0x45 firmware ID 0x%02X", fw_id);
    } else {
        ESP_LOGW(TAG, "panel MCU answered probe but firmware ID read failed");
    }

    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = P4D_DSI_PHY_LDO_CHAN,
        .voltage_mv = P4D_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(
        esp_ldo_acquire_channel(&ldo_cfg, &s_phy_ldo),
        TAG,
        "acquire MIPI PHY LDO");

    ESP_RETURN_ON_ERROR(panel_power_on(), TAG, "panel power sequence");

    const esp_lcd_dsi_bus_config_t dsi_bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = P4D_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = P4D_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_dsi_bus(&dsi_bus_cfg, &s_dsi_bus),
        TAG,
        "create DSI bus");

    esp_lcd_panel_io_handle_t dbi_io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &dbi_io),
        TAG,
        "create DBI command IO");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(dbi_io), TAG, "delete DBI command IO");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = P4D_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .out_color_format = LCD_COLOR_FMT_RGB888,
        .num_fbs = 1,
        .video_timing = {
            .h_size = P4D_LCD_H_RES,
            .v_size = P4D_LCD_V_RES,
            .hsync_back_porch = P4D_MODE_HBP,
            .hsync_pulse_width = P4D_MODE_HSW,
            .hsync_front_porch = P4D_MODE_HFP,
            .vsync_back_porch = P4D_MODE_VBP,
            .vsync_pulse_width = P4D_MODE_VSW,
            .vsync_front_porch = P4D_MODE_VFP,
        },
        .flags.disable_lp = 0,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &s_panel),
        TAG,
        "create DPI panel");

    p4d_dsi_bus_priv_t *priv = (p4d_dsi_bus_priv_t *)s_dsi_bus;
    mipi_dsi_host_ll_dpi_set_video_burst_type(
        priv->hal.host,
        MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
    mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, false);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init DPI panel");

    void *framebuffer = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &framebuffer),
        TAG,
        "get DPI framebuffer");

    s_framebuffer = (uint8_t *)framebuffer;
    s_framebuffer_size = P4D_LCD_FB_SIZE;
    memset(s_framebuffer, 0, s_framebuffer_size);
    ESP_RETURN_ON_ERROR(p4d_display_sync(), TAG, "clear framebuffer");

    mipi_dsi_host_ll_set_clock_lane_state(
        priv->hal.host,
        MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);

    ESP_RETURN_ON_ERROR(bridge_release_reset(), TAG, "release TC358762");
    tc358762_bridge_init();

    ESP_RETURN_ON_ERROR(p4d_display_brightness_set(255), TAG, "set backlight");

    *ret_framebuffer = s_framebuffer;
    *ret_framebuffer_size = s_framebuffer_size;

    ESP_LOGI(
        TAG,
        "display ready: %dx%d RGB888, framebuffer=%p, 1-lane DSI @ %d Mbps",
        P4D_LCD_H_RES,
        P4D_LCD_V_RES,
        s_framebuffer,
        P4D_DSI_LANE_MBPS);

    return ESP_OK;
}
