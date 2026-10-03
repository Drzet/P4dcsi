#include "camera_stream.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

static const char *TAG = "p4d_camera";

#define CAMERA_POWER_GPIO      GPIO_NUM_0
#define CAMERA_BUFFER_COUNT    3
#define CAMERA_TASK_STACK      (16 * 1024)
#define CAMERA_TASK_PRIORITY   8
#define CAMERA_TASK_CORE       0

#define CAMERA_WIDTH           640
#define CAMERA_HEIGHT          400

typedef struct {
    uint8_t *ptr;
    size_t len;
} camera_buffer_t;

static int s_fd = -1;
static camera_buffer_t s_buffers[CAMERA_BUFFER_COUNT];
static uint32_t s_buffer_count;
static uint32_t s_width;
static uint32_t s_height;
static p4d_camera_frame_cb_t s_frame_cb;
static void *s_frame_ctx;
static TaskHandle_t s_task;

static esp_err_t camera_power_enable(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CAMERA_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure camera power GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(CAMERA_POWER_GPIO, 1), TAG, "enable camera power");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t p4d_camera_init(i2c_master_bus_handle_t i2c_bus)
{
    if (i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(camera_power_enable(), TAG, "camera power");

    const esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus,
            .freq = 400000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };

    const esp_video_init_config_t video_cfg = {
        .csi = &csi_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_video_init(&video_cfg), TAG, "esp_video_init");

    ESP_LOGI(TAG, "esp_video initialized; expecting OV9281 on MIPI CSI");
    return ESP_OK;
}

static esp_err_t camera_open_and_map(void)
{
    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_FAIL;
    }

    struct v4l2_capability cap = {0};
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "driver=%s card=%s bus=%s", cap.driver, cap.card, cap.bus_info);

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = CAMERA_WIDTH,
            .height = CAMERA_HEIGHT,
            .pixelformat = V4L2_PIX_FMT_SBGGR8,
        },
    };

    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT RAW8 640x400 failed");
        return ESP_FAIL;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        return ESP_FAIL;
    }

    s_width = fmt.fmt.pix.width;
    s_height = fmt.fmt.pix.height;

    ESP_LOGI(
        TAG,
        "capture format %ux%u fourcc=%c%c%c%c",
        (unsigned)s_width,
        (unsigned)s_height,
        (char)(fmt.fmt.pix.pixelformat & 0xff),
        (char)((fmt.fmt.pix.pixelformat >> 8) & 0xff),
        (char)((fmt.fmt.pix.pixelformat >> 16) & 0xff),
        (char)((fmt.fmt.pix.pixelformat >> 24) & 0xff));

    struct v4l2_requestbuffers req = {
        .count = CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed/count=%u", (unsigned)req.count);
        return ESP_FAIL;
    }

    s_buffer_count = req.count;
    if (s_buffer_count > CAMERA_BUFFER_COUNT) {
        s_buffer_count = CAMERA_BUFFER_COUNT;
    }

    for (uint32_t i = 0; i < s_buffer_count; ++i) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };

        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF %u failed", (unsigned)i);
            return ESP_FAIL;
        }

        void *mapped = mmap(
            NULL,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            s_fd,
            buf.m.offset);

        if (mapped == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap buffer %u failed", (unsigned)i);
            return ESP_FAIL;
        }

        s_buffers[i].ptr = (uint8_t *)mapped;
        s_buffers[i].len = buf.length;

        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF %u failed", (unsigned)i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static void camera_task(void *arg)
{
    (void)arg;

    while (true) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (buf.index < s_buffer_count && s_frame_cb != NULL) {
            size_t len = buf.bytesused ? buf.bytesused : s_buffers[buf.index].len;
            s_frame_cb(
                s_buffers[buf.index].ptr,
                len,
                s_width,
                s_height,
                s_frame_ctx);
        }

        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t p4d_camera_start(p4d_camera_frame_cb_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(camera_open_and_map(), TAG, "open/map camera");

    s_frame_cb = cb;
    s_frame_ctx = user_ctx;

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        return ESP_FAIL;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        camera_task,
        "ov9281_capture",
        CAMERA_TASK_STACK,
        NULL,
        CAMERA_TASK_PRIORITY,
        &s_task,
        CAMERA_TASK_CORE);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create capture task");
        ioctl(s_fd, VIDIOC_STREAMOFF, &type);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OV9281 capture started");
    return ESP_OK;
}
