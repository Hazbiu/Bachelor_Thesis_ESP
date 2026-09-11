#include "domain/ports/system_adapters_port.h"

#include "esp_video_init.h"
#include "platform/camera/camera_runtime.h"
#include "platform/camera/video_capture.h"
#include "platform/display/display_platform.h"
#include "platform/power/cpu_power.h"
#include "platform/storage/sd_card_storage.h"
#include "platform/storage/spiffs_storage.h"

esp_err_t system_storage_spiffs_mount(void)
{
    return spiffs_storage_mount();
}

esp_err_t system_storage_sdcard_ensure_mounted(void)
{
    return sd_card_storage_ensure_mounted();
}

esp_err_t system_cpu_power_init(void)
{
    return cpu_power_init();
}

esp_err_t system_cpu_face_boost_begin(void)
{
    return cpu_power_face_boost_begin();
}

esp_err_t system_cpu_face_boost_end(void)
{
    return cpu_power_face_boost_end();
}

bool system_cpu_face_boost_is_active(void)
{
    return cpu_power_is_face_boost_active();
}

uint32_t system_cpu_face_detect_interval_frames(void)
{
    return cpu_power_get_face_detect_interval_frames();
}

lv_display_t *system_display_start(void)
{
    return display_platform_start();
}

void system_display_disable_lvgl_overlays(lv_display_t *display)
{
    display_platform_disable_lvgl_overlays(display);
}

void system_display_backlight_on(void)
{
    display_platform_backlight_on();
}

void system_display_backlight_off(void)
{
    display_platform_backlight_off();
}

esp_err_t system_display_suspend_for_light_sleep(bool prepare_panel_for_deep)
{
    return display_platform_suspend_for_light_sleep(prepare_panel_for_deep);
}

lv_display_t *system_display_resume_from_light_sleep(void)
{
    return display_platform_resume_from_light_sleep();
}

lv_indev_t *system_display_get_input_device(void)
{
    return display_platform_get_input_device();
}

esp_err_t system_display_poll_touch_for_light_sleep(bool *touched)
{
    return display_platform_poll_touch_for_light_sleep(touched);
}

uint32_t system_display_width(void)
{
    return display_platform_width();
}

uint32_t system_display_height(void)
{
    return display_platform_height();
}

esp_err_t system_camera_initialize(void)
{
    return camera_runtime_initialize();
}

int system_camera_open_default(void)
{
    return app_video_open((char *)ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT);
}

esp_err_t system_camera_set_buffers(int video_fd, uint32_t fb_num, const void **fb)
{
    return app_video_set_bufs(video_fd, fb_num, fb);
}

uint32_t system_camera_buffer_size(void)
{
    return app_video_get_buf_size();
}

esp_err_t system_camera_stream_start(int video_fd, int core_id, void *user_data)
{
    return app_video_stream_task_start(video_fd, core_id, user_data);
}

esp_err_t system_camera_stream_restart(int video_fd)
{
    return app_video_stream_task_restart(video_fd);
}

esp_err_t system_camera_stream_stop(int video_fd)
{
    return app_video_stream_task_stop(video_fd);
}

esp_err_t system_camera_prepare_sensor_for_light_sleep(void)
{
    return app_video_prepare_sensor_for_light_sleep();
}

esp_err_t system_camera_restore_sensor_after_light_sleep(void)
{
    return app_video_restore_sensor_after_light_sleep();
}

esp_err_t system_camera_register_frame_callback(system_camera_frame_callback_t callback)
{
    return app_video_register_frame_operation_cb(callback);
}

bool system_camera_is_rgb565(void)
{
    return APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565;
}

uint32_t system_camera_bytes_per_pixel(void)
{
    return system_camera_is_rgb565() ? 2U : 3U;
}
