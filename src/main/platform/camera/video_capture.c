/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"

#include "linux/videodev2.h"
#include "platform/camera/video_capture.h"

static const char *TAG = "app_video";

#define MAX_BUFFER_COUNT                  3
#define MIN_BUFFER_COUNT                  2
#define VIDEO_TASK_STACK_SIZE             (4 * 1024)
#define VIDEO_TASK_PRIORITY               4
#define VIDEO_STOP_TIMEOUT_MS             1500
#define VIDEO_CLOSE_GRACE_TIMEOUT_MS      250
#define VIDEO_NO_FRAME_RETRY_DELAY_MS     2
#define VIDEO_ERROR_RETRY_DELAY_MS        10

typedef struct {
    uint8_t *camera_buffer[MAX_BUFFER_COUNT];
    size_t camera_buf_size;
    uint8_t camera_buf_count;
    uint32_t camera_buf_hes;
    uint32_t camera_buf_ves;

    struct v4l2_buffer v4l2_buf;
    uint8_t camera_mem_mode;

    app_video_frame_operation_cb_t user_camera_video_frame_operation_cb;

    TaskHandle_t video_stream_task_handle;
    uint8_t video_task_core_id;
    volatile bool video_task_delete;
    volatile bool video_streaming;
    SemaphoreHandle_t video_stopped_sem;

    void *video_task_user_data;
    int video_fd;
} app_video_t;

static app_video_t app_camera_video = {
    .video_fd = -1,
};

static void clear_binary_semaphore(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL) {
        return;
    }

    while (xSemaphoreTake(semaphore, 0) == pdTRUE) {
        /* Remove a signal left from an earlier stop operation. */
    }
}

static void reset_camera_runtime_state(void)
{
    app_camera_video.video_fd = -1;
    app_camera_video.video_streaming = false;
    app_camera_video.video_task_delete = false;
    app_camera_video.video_stream_task_handle = NULL;
    app_camera_video.camera_buf_count = 0;
    app_camera_video.camera_buf_size = 0;
    app_camera_video.camera_buf_hes = 0;
    app_camera_video.camera_buf_ves = 0;
    app_camera_video.video_task_user_data = NULL;

    for (int i = 0; i < MAX_BUFFER_COUNT; i++) {
        /*
         * Camera buffers are allocated and owned by app_main.
         * The driver association is removed here; this module does not free
         * application-owned PSRAM.
         */
        app_camera_video.camera_buffer[i] = NULL;
    }
}

esp_err_t app_video_main(i2c_master_bus_handle_t i2c_bus_handle)
{
    esp_video_init_csi_config_t csi_config[] = {
        {
            .sccb_config = {
                .init_sccb = false,
                .i2c_handle = i2c_bus_handle,
                .freq = CONFIG_BSP_I2C_CLK_SPEED_HZ,
            },
            /*
             * The board/BSP currently does not expose camera RESET or PWDN
             * control through this module.
             */
            .reset_pin = -1,
            .pwdn_pin = -1,
        },
    };

    esp_video_init_config_t cam_config = {
        .csi = csi_config,
    };

    return esp_video_init(&cam_config);
}

int app_video_open(char *dev, video_fmt_t init_fmt)
{
    if (dev == NULL) {
        ESP_LOGE(TAG, "Video device path is NULL");
        return -1;
    }

    struct v4l2_format default_format;
    struct v4l2_capability capability;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];

    /*
     * Use non-blocking capture so the stream task periodically regains
     * execution and can observe video_task_delete. This prevents shutdown
     * from depending on VIDIOC_STREAMOFF waking a blocking VIDIOC_DQBUF.
     */
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        ESP_LOGE(TAG, "Open video failed, errno=%d", errno);
        return -1;
    }

    memset(&capability, 0, sizeof(capability));
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGE(TAG, "Failed to get video capability, errno=%d", errno);
        goto exit_error;
    }

    ESP_LOGI(
        TAG,
        "version: %d.%d.%d",
        (uint16_t)(capability.version >> 16),
        (uint8_t)(capability.version >> 8),
        (uint8_t)capability.version);
    ESP_LOGI(TAG, "driver:  %s", capability.driver);
    ESP_LOGI(TAG, "card:    %s", capability.card);
    ESP_LOGI(TAG, "bus:     %s", capability.bus_info);

    memset(&default_format, 0, sizeof(default_format));
    default_format.type = type;

    if (ioctl(fd, VIDIOC_G_FMT, &default_format) != 0) {
        ESP_LOGE(TAG, "Failed to get video format, errno=%d", errno);
        goto exit_error;
    }

    ESP_LOGI(
        TAG,
        "width=%" PRIu32 " height=%" PRIu32,
        default_format.fmt.pix.width,
        default_format.fmt.pix.height);

    app_camera_video.camera_buf_hes = default_format.fmt.pix.width;
    app_camera_video.camera_buf_ves = default_format.fmt.pix.height;

    if (default_format.fmt.pix.pixelformat != init_fmt) {
        struct v4l2_format format = {
            .type = type,
            .fmt.pix.width = default_format.fmt.pix.width,
            .fmt.pix.height = default_format.fmt.pix.height,
            .fmt.pix.pixelformat = init_fmt,
        };

        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
            ESP_LOGE(TAG, "Failed to set video format, errno=%d", errno);
            goto exit_error;
        }
    }

    memset(&controls, 0, sizeof(controls));
    memset(control, 0, sizeof(control));

    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 1;
    controls.controls = control;

    control[0].id = V4L2_CID_VFLIP;
    control[0].value = 0;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "Failed to configure vertical flip; continuing");
    }

    control[0].id = V4L2_CID_HFLIP;
    control[0].value = 0;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "Failed to configure horizontal flip; continuing");
    }

    app_camera_video.video_fd = fd;
    return fd;

exit_error:
    close(fd);
    return -1;
}

esp_err_t app_video_set_bufs(
    int video_fd,
    uint32_t fb_num,
    const void **fb)
{
    if (video_fd < 0) {
        ESP_LOGE(TAG, "Invalid video file descriptor");
        return ESP_ERR_INVALID_ARG;
    }

    if (fb_num > MAX_BUFFER_COUNT) {
        ESP_LOGE(TAG, "Buffer count is too large");
        return ESP_ERR_INVALID_ARG;
    }

    if (fb_num < MIN_BUFFER_COUNT) {
        ESP_LOGE(TAG, "At least two camera buffers are required");
        return ESP_ERR_INVALID_ARG;
    }

    struct v4l2_requestbuffers req;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    memset(&req, 0, sizeof(req));
    req.count = fb_num;
    req.type = type;
    req.memory = fb ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;

    app_camera_video.camera_buf_count = fb_num;
    app_camera_video.camera_mem_mode = req.memory;

    if (ioctl(video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "Requesting video buffers failed, errno=%d", errno);
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < fb_num; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = req.memory;
        buf.index = i;

        if (ioctl(video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Querying camera buffer %" PRIu32 " failed", i);
            return ESP_FAIL;
        }

        if (req.memory == V4L2_MEMORY_MMAP) {
            void *mapped = mmap(
                NULL,
                buf.length,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                video_fd,
                buf.m.offset);

            /*
             * ESP-IDF's mmap compatibility layer does not define MAP_FAILED
             * for every target/configuration. Accept both common failure
             * representations without depending on that macro.
             */
            if (mapped == NULL || mapped == (void *)(intptr_t)-1) {
                ESP_LOGE(TAG, "Mapping camera buffer %" PRIu32 " failed", i);
                return ESP_FAIL;
            }

            app_camera_video.camera_buffer[i] = mapped;
        } else {
            if (fb[i] == NULL) {
                ESP_LOGE(TAG, "Camera buffer %" PRIu32 " is NULL", i);
                return ESP_ERR_INVALID_ARG;
            }

            buf.m.userptr = (unsigned long)fb[i];
            app_camera_video.camera_buffer[i] = (uint8_t *)fb[i];
        }

        app_camera_video.camera_buf_size = buf.length;

        if (ioctl(video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Queueing camera buffer %" PRIu32 " failed", i);
            return ESP_FAIL;
        }
    }

    for (uint32_t i = fb_num; i < MAX_BUFFER_COUNT; i++) {
        app_camera_video.camera_buffer[i] = NULL;
    }

    return ESP_OK;
}

esp_err_t app_video_get_bufs(int fb_num, void **fb)
{
    if (fb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fb_num > MAX_BUFFER_COUNT || fb_num < MIN_BUFFER_COUNT) {
        ESP_LOGE(TAG, "Invalid camera buffer count: %d", fb_num);
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < fb_num; i++) {
        if (app_camera_video.camera_buffer[i] == NULL) {
            ESP_LOGE(TAG, "Camera buffer %d is NULL", i);
            return ESP_FAIL;
        }

        fb[i] = app_camera_video.camera_buffer[i];
    }

    return ESP_OK;
}

uint32_t app_video_get_buf_size(void)
{
    const uint32_t bytes_per_pixel =
        APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2U : 3U;

    return app_camera_video.camera_buf_hes *
           app_camera_video.camera_buf_ves *
           bytes_per_pixel;
}

static esp_err_t video_receive_video_frame(int video_fd)
{
    memset(
        &app_camera_video.v4l2_buf,
        0,
        sizeof(app_camera_video.v4l2_buf));

    app_camera_video.v4l2_buf.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;
    app_camera_video.v4l2_buf.memory =
        app_camera_video.camera_mem_mode;

    if (ioctl(
            video_fd,
            VIDIOC_DQBUF,
            &app_camera_video.v4l2_buf) != 0) {
        /*
         * O_NONBLOCK makes EAGAIN/EWOULDBLOCK the normal "no completed frame
         * yet" condition. It is not a camera failure.
         */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ESP_ERR_NOT_FOUND;
        }

        /*
         * A signal/interruption is transient. Let the task retry unless a
         * shutdown request is already pending.
         */
        if (errno == EINTR) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGE(TAG, "VIDIOC_DQBUF failed, errno=%d", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void video_operation_video_frame(void)
{
    uint32_t buffer_index = app_camera_video.v4l2_buf.index;

    if (buffer_index >= app_camera_video.camera_buf_count ||
        buffer_index >= MAX_BUFFER_COUNT) {
        ESP_LOGE(
            TAG,
            "Driver returned invalid camera buffer index %" PRIu32,
            buffer_index);
        return;
    }

    uint8_t *buffer =
        app_camera_video.camera_buffer[buffer_index];

    if (buffer == NULL) {
        ESP_LOGE(
            TAG,
            "Camera buffer %" PRIu32 " is NULL",
            buffer_index);
        return;
    }

    app_camera_video.v4l2_buf.m.userptr =
        (unsigned long)buffer;
    app_camera_video.v4l2_buf.length =
        app_camera_video.camera_buf_size;

    if (app_camera_video.user_camera_video_frame_operation_cb != NULL) {
        app_camera_video.user_camera_video_frame_operation_cb(
            buffer,
            (uint8_t)buffer_index,
            app_camera_video.camera_buf_hes,
            app_camera_video.camera_buf_ves,
            app_camera_video.camera_buf_size,
            app_camera_video.video_task_user_data);
    }
}

static esp_err_t video_free_video_frame(int video_fd)
{
    if (ioctl(
            video_fd,
            VIDIOC_QBUF,
            &app_camera_video.v4l2_buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QBUF failed, errno=%d", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t video_stream_start(int video_fd)
{
    ESP_LOGI(TAG, "Video Stream Start");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(
            TAG,
            "Failed to start video stream, errno=%d",
            errno);
        return ESP_FAIL;
    }

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = type;

    if (ioctl(video_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "Failed to read format after stream start");
        (void)ioctl(video_fd, VIDIOC_STREAMOFF, &type);
        return ESP_FAIL;
    }

    app_camera_video.video_streaming = true;
    return ESP_OK;
}

static esp_err_t video_stream_stop(int video_fd)
{
    if (!app_camera_video.video_streaming) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Video Stream Stop");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(video_fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGE(
            TAG,
            "Failed to stop video stream, errno=%d",
            errno);
        return ESP_FAIL;
    }

    app_camera_video.video_streaming = false;
    return ESP_OK;
}

static void video_stream_task(void *arg)
{
    int video_fd = *((int *)arg);

    ESP_LOGI(
        TAG,
        "[CORE-PROOF] Video task running: actual_cpu=%d task=%s",
        xPortGetCoreID(),
        pcTaskGetName(NULL));

    while (!app_camera_video.video_task_delete) {
        esp_err_t ret =
            video_receive_video_frame(video_fd);

        if (ret == ESP_ERR_NOT_FOUND) {
            /*
             * No frame is ready yet. Yield briefly and then check the
             * shutdown flag again.
             */
            vTaskDelay(
                pdMS_TO_TICKS(
                    VIDEO_NO_FRAME_RETRY_DELAY_MS));
            continue;
        }

        if (ret == ESP_ERR_INVALID_STATE) {
            if (app_camera_video.video_task_delete) {
                break;
            }

            taskYIELD();
            continue;
        }

        if (ret != ESP_OK) {
            if (app_camera_video.video_task_delete) {
                break;
            }

            vTaskDelay(
                pdMS_TO_TICKS(
                    VIDEO_ERROR_RETRY_DELAY_MS));
            continue;
        }

        /*
         * Once shutdown is requested, do not process or requeue another
         * completed frame.
         */
        if (app_camera_video.video_task_delete) {
            break;
        }

        video_operation_video_frame();

        if (app_camera_video.video_task_delete) {
            break;
        }

        ret = video_free_video_frame(video_fd);
        if (ret != ESP_OK) {
            if (!app_camera_video.video_task_delete) {
                ESP_LOGE(
                    TAG,
                    "Failed to return video frame to the driver");
            }
            break;
        }
    }

    /*
     * During a requested shutdown, app_video_stream_task_stop()
     * already executes VIDIOC_STREAMOFF. Do not execute it again
     * from this task, because two simultaneous STREAMOFF calls can
     * race and produce EBUSY (errno 16).
     *
     * If the task exits independently because of a streaming error,
     * it remains responsible for stopping the stream.
     */
    if (!app_camera_video.video_task_delete) {
        esp_err_t stop_ret = video_stream_stop(video_fd);

        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "Video stream did not stop cleanly");
        }
    }
    
    app_camera_video.video_task_delete = false;
    app_camera_video.video_stream_task_handle = NULL;

    if (app_camera_video.video_stopped_sem != NULL) {
        xSemaphoreGive(app_camera_video.video_stopped_sem);
    }

    ESP_LOGI(TAG, "Video stream task stopped");
    vTaskDelete(NULL);
}

esp_err_t app_video_stream_task_start(
    int video_fd,
    int core_id,
    void *user_data)
{
    if (video_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (app_camera_video.video_stream_task_handle != NULL) {
        ESP_LOGW(
            TAG,
            "Video stream task is already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (app_camera_video.video_stopped_sem == NULL) {
        app_camera_video.video_stopped_sem =
            xSemaphoreCreateBinary();

        if (app_camera_video.video_stopped_sem == NULL) {
            ESP_LOGE(
                TAG,
                "Failed to create video-stop semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

    clear_binary_semaphore(
        app_camera_video.video_stopped_sem);

    app_camera_video.video_task_delete = false;
    app_camera_video.video_task_core_id = core_id;
    app_camera_video.video_task_user_data = user_data;
    app_camera_video.video_fd = video_fd;

    esp_err_t ret = video_stream_start(video_fd);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(
        TAG,
        "[CORE-PROOF] Creating video stream task: requested_core=%d",
        core_id);

    BaseType_t result = xTaskCreatePinnedToCore(
        video_stream_task,
        "video stream task",
        VIDEO_TASK_STACK_SIZE,
        &app_camera_video.video_fd,
        VIDEO_TASK_PRIORITY,
        &app_camera_video.video_stream_task_handle,
        core_id);

    if (result != pdPASS) {
        ESP_LOGE(
            TAG,
            "Failed to create video stream task");

        app_camera_video.video_stream_task_handle = NULL;
        (void)video_stream_stop(video_fd);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_video_stream_task_restart(int video_fd)
{
    if (video_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (app_camera_video.video_stream_task_handle != NULL) {
        ESP_LOGE(
            TAG,
            "Cannot restart while video stream task is running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = app_video_set_bufs(
        video_fd,
        app_camera_video.camera_buf_count,
        (const void **)app_camera_video.camera_buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore camera buffers");
        return ret;
    }

    ret = app_video_stream_task_start(
        video_fd,
        app_camera_video.video_task_core_id,
        app_camera_video.video_task_user_data);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to restart video stream task");
        return ret;
    }

    return ESP_OK;
}

esp_err_t app_video_stream_task_stop(int video_fd)
{
    if (video_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (app_camera_video.video_stream_task_handle == NULL) {
        return ESP_OK;
    }

    if (xTaskGetCurrentTaskHandle() ==
        app_camera_video.video_stream_task_handle) {
        ESP_LOGE(
            TAG,
            "Video task cannot synchronously stop itself");
        return ESP_ERR_INVALID_STATE;
    }

    if (app_camera_video.video_stopped_sem == NULL) {
        ESP_LOGE(
            TAG,
            "Video-stop semaphore is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    clear_binary_semaphore(
        app_camera_video.video_stopped_sem);

    /*
     * Set this before STREAMOFF. With non-blocking DQBUF, the task observes
     * the flag within a few milliseconds even if STREAMOFF does not wake the
     * driver immediately.
     */
    app_camera_video.video_task_delete = true;

    esp_err_t streamoff_ret =
        video_stream_stop(video_fd);

    if (streamoff_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "STREAMOFF failed while requesting task shutdown");
    }

    if (xSemaphoreTake(
            app_camera_video.video_stopped_sem,
            pdMS_TO_TICKS(VIDEO_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(
            TAG,
            "Timed out while stopping video stream task");

        /*
         * Keep video_task_delete asserted. app_video_shutdown() will close
         * the file descriptor and perform one final bounded wait.
         */
        return ESP_ERR_TIMEOUT;
    }

    return streamoff_ret;
}

esp_err_t app_video_shutdown(void)
{
    int video_fd = app_camera_video.video_fd;

    if (video_fd < 0) {
        ESP_LOGI(TAG, "Camera is already closed");
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Shutting down camera before deep sleep");

    esp_err_t stop_ret =
        app_video_stream_task_stop(video_fd);

    if (stop_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Graceful camera-task stop failed: %s; "
            "closing the device to force release",
            esp_err_to_name(stop_ret));
    }

    /*
     * Always close the camera file descriptor, even when the normal task-stop
     * handshake times out. The previous implementation returned early and
     * therefore left the CSI device open during deep sleep.
     */
    esp_err_t close_ret = ESP_OK;

    if (close(video_fd) != 0) {
        ESP_LOGE(
            TAG,
            "Closing camera device failed, errno=%d",
            errno);
        close_ret = ESP_FAIL;
    }

    app_camera_video.video_fd = -1;
    app_camera_video.video_streaming = false;

    /*
     * Closing the descriptor should release a task that was still inside a
     * driver operation. Give it one final bounded interval to signal exit.
     */
    if (app_camera_video.video_stream_task_handle != NULL &&
        app_camera_video.video_stopped_sem != NULL) {
        if (xSemaphoreTake(
                app_camera_video.video_stopped_sem,
                pdMS_TO_TICKS(
                    VIDEO_CLOSE_GRACE_TIMEOUT_MS)) == pdTRUE) {
            ESP_LOGW(
                TAG,
                "Video task exited after forced device close");
        } else {
            /*
             * Deep sleep follows immediately and resets the complete CSI
             * subsystem. Delete the application task so no task can access
             * the now-closed descriptor during the remaining shutdown steps.
             */
            TaskHandle_t stuck_task =
                app_camera_video.video_stream_task_handle;

            app_camera_video.video_stream_task_handle = NULL;
            app_camera_video.video_task_delete = false;

            if (stuck_task != NULL) {
                ESP_LOGW(
                    TAG,
                    "Force deleting video task after shutdown timeout");
                vTaskDelete(stuck_task);
            }
        }
    }

    reset_camera_runtime_state();

    if (close_ret != ESP_OK) {
        return close_ret;
    }

    ESP_LOGI(
        TAG,
        "Camera stream stopped and camera device closed");

    return ESP_OK;
}

esp_err_t app_video_register_frame_operation_cb(
    app_video_frame_operation_cb_t operation_cb)
{
    if (operation_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_camera_video.user_camera_video_frame_operation_cb =
        operation_cb;

    return ESP_OK;
}
