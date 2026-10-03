#include "camera_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

static const char *TAG = "p4d_camera";

/*
 * CSI flow follows Wireless-Tag's WT9932P4-TINY BSP implementation:
 * components/wt_bsp/features/csi/wt_bsp_csi.c
 *
 * Only the camera parameters differ:
 * OV9281, 640x400, RAW8, 3 MMAP buffers.
 */
#define CAMERA_WIDTH             640
#define CAMERA_HEIGHT            400
#define CAMERA_BUFFER_COUNT      3
#define CAMERA_TASK_STACK_SIZE   8192
#define CAMERA_TASK_PRIORITY     5

typedef struct {
    uint8_t *buffers[CAMERA_BUFFER_COUNT];
    size_t buffer_size;
    int fd;
    bool initialized;
    bool streaming;
    TaskHandle_t task;
    p4d_camera_frame_cb_t frame_cb;
    void *user_ctx;
} p4d_camera_state_t;

static p4d_camera_state_t s_camera = {
    .fd = -1,
};

esp_err_t p4d_camera_init(i2c_master_bus_handle_t i2c_bus)
{
    if (i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_camera, 0, sizeof(s_camera));
    s_camera.fd = -1;

    esp_video_init_csi_config_t csi_config = {0};
    csi_config.sccb_config.init_sccb = false;
    csi_config.sccb_config.i2c_handle = i2c_bus;
    csi_config.sccb_config.freq = 400000;
    csi_config.reset_pin = -1;
    csi_config.pwdn_pin = -1;

    esp_video_init_csi_config_t csi_config_arr[] = { csi_config };

    esp_video_init_config_t video_config = {
        .csi = csi_config_arr,
    };

    /* Same startup delays used by the WT9932P4-TINY BSP CSI implementation. */
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t ret = esp_video_init(&video_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    s_camera.initialized = true;
    ESP_LOGI(TAG, "CSI initialized successfully");
    return ESP_OK;
}

static void camera_stream_task(void *arg)
{
    p4d_camera_state_t *camera = (p4d_camera_state_t *)arg;
    const int fd = camera->fd;

    while (camera->streaming) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == 0) {
            if (buf.index < CAMERA_BUFFER_COUNT && camera->frame_cb != NULL) {
                camera->frame_cb(
                    camera->buffers[buf.index],
                    buf.bytesused,
                    CAMERA_WIDTH,
                    CAMERA_HEIGHT,
                    camera->user_ctx);
            }

            if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF failed: %d", errno);
                break;
            }
        } else {
            if (errno != EAGAIN) {
                ESP_LOGE(TAG, "VIDIOC_DQBUF failed: %d", errno);
                break;
            }
            vTaskDelay(1);
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    camera->streaming = false;
    camera->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t p4d_camera_start(p4d_camera_frame_cb_t cb, void *user_ctx)
{
    if (!s_camera.initialized || cb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_camera.streaming) {
        return ESP_OK;
    }

    const int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_FAIL;
    }
    s_camera.fd = fd;

    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = CAMERA_WIDTH,
        .fmt.pix.height = CAMERA_HEIGHT,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR8,
    };

    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "failed to set RAW8 640x400 format");
        goto err;
    }

    struct v4l2_requestbuffers req = {
        .count = CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "failed to request buffers");
        goto err;
    }

    if (req.count > CAMERA_BUFFER_COUNT) {
        ESP_LOGE(TAG, "driver returned too many buffers: %u", (unsigned)req.count);
        goto err;
    }

    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to query buffer %u", (unsigned)i);
            goto err;
        }

        s_camera.buffers[i] = mmap(
            NULL,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            buf.m.offset);

        if (s_camera.buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "failed to mmap buffer %u", (unsigned)i);
            goto err;
        }

        s_camera.buffer_size = buf.length;

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to queue buffer %u", (unsigned)i);
            goto err;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "failed to start stream");
        goto err;
    }

    s_camera.frame_cb = cb;
    s_camera.user_ctx = user_ctx;
    s_camera.streaming = true;

    BaseType_t res = xTaskCreate(
        camera_stream_task,
        "csi_stream",
        CAMERA_TASK_STACK_SIZE,
        &s_camera,
        CAMERA_TASK_PRIORITY,
        &s_camera.task);

    if (res != pdPASS) {
        ESP_LOGE(TAG, "failed to create stream task");
        s_camera.streaming = false;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        goto err;
    }

    ESP_LOGI(TAG, "OV9281 capture started: 640x400 RAW8");
    return ESP_OK;

err:
    close(fd);
    s_camera.fd = -1;
    return ESP_FAIL;
}
