#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application-facing port implemented by src_p4/main/platform/.
 * Application Logic depends on this neutral contract, not on concrete
 * platform headers. The platform layer remains the owner of board/driver APIs.
 */

typedef void (*system_camera_frame_callback_t)(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data);

/* Storage */
esp_err_t system_storage_spiffs_mount(void);
esp_err_t system_storage_sdcard_ensure_mounted(void);

/* CPU power */
esp_err_t system_cpu_power_init(void);
esp_err_t system_cpu_face_boost_begin(void);
esp_err_t system_cpu_face_boost_end(void);
bool system_cpu_face_boost_is_active(void);
uint32_t system_cpu_face_detect_interval_frames(void);

/* Display */
lv_display_t *system_display_start(void);
void system_display_disable_lvgl_overlays(lv_display_t *display);
void system_display_backlight_on(void);
void system_display_backlight_off(void);
esp_err_t system_display_suspend_for_light_sleep(bool prepare_panel_for_deep);
lv_display_t *system_display_resume_from_light_sleep(void);
lv_indev_t *system_display_get_input_device(void);
esp_err_t system_display_poll_touch_for_light_sleep(bool *touched);
uint32_t system_display_width(void);
uint32_t system_display_height(void);

/* Camera/video */
esp_err_t system_camera_initialize(void);
int system_camera_open_default(void);
esp_err_t system_camera_set_buffers(int video_fd, uint32_t fb_num, const void **fb);
uint32_t system_camera_buffer_size(void);
esp_err_t system_camera_stream_start(int video_fd, int core_id, void *user_data);
esp_err_t system_camera_stream_restart(int video_fd);
esp_err_t system_camera_stream_stop(int video_fd);
esp_err_t system_camera_prepare_sensor_for_light_sleep(void);
esp_err_t system_camera_restore_sensor_after_light_sleep(void);
esp_err_t system_camera_register_frame_callback(system_camera_frame_callback_t callback);
bool system_camera_is_rgb565(void);
uint32_t system_camera_bytes_per_pixel(void);

#ifdef __cplusplus
}
#endif
