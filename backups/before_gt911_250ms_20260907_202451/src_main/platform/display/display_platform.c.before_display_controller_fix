#include "platform/display/display_platform.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_platform";

/*
 * True only after the currently active panel instance accepted SLEEP_IN before
 * its transport was destroyed. A successful resume creates/reinitializes the
 * panel again and clears this flag.
 */
static bool s_panel_sleep_committed;


void display_platform_disable_lvgl_overlays(
    lv_display_t *display)
{
#if LV_USE_SYSMON
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not lock LVGL to hide system overlays");
        return;
    }

#if LV_USE_PERF_MONITOR && LV_VERSION_CHECK(9, 2, 0)
    lv_sysmon_hide_performance(display);
#endif

#if LV_USE_MEM_MONITOR && LV_VERSION_CHECK(9, 2, 0)
    lv_sysmon_hide_memory(display);
#endif

    bsp_display_unlock();
#else
    (void)display;
#endif
}


void display_platform_backlight_on(void)
{
    (void)bsp_display_backlight_on();
}


void display_platform_backlight_off(void)
{
    (void)bsp_display_backlight_off();
}


lv_display_t *display_platform_start(void)
{
    lv_display_t *display = bsp_display_start();

    if (display != NULL) {
        s_panel_sleep_committed = false;
    }

    return display;
}


esp_err_t display_platform_panel_enter_full_sleep(void)
{
    esp_lcd_panel_handle_t panel = bsp_display_get_panel_handle();

    /*
     * Hybrid Light->Deep intentionally destroys the DSI/panel handle during
     * Light-sleep. If SLEEP_IN was sent before that teardown, the later
     * destructive Deep-sleep stage must treat the missing handle as success
     * rather than falsely reporting that the LCD sleep command was skipped.
     */
    if (panel == NULL) {
        if (s_panel_sleep_committed) {
            ESP_LOGI(
                TAG,
                "LCD FULL SLEEP already committed before display teardown; "
                "no live panel handle is required at the Deep-sleep boundary");
            return ESP_OK;
        }

        ESP_LOGW(
            TAG,
            "LCD FULL SLEEP unavailable: no BSP panel handle and no prior "
            "SLEEP_IN commit");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t first_error = ESP_OK;

    esp_err_t ret = bsp_display_backlight_off();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LCD FULL SLEEP: backlight OFF requested");
    } else {
        ESP_LOGW(
            TAG,
            "LCD FULL SLEEP: backlight OFF request failed: %s",
            esp_err_to_name(ret));
        first_error = ret;
    }

    ret = esp_lcd_panel_disp_on_off(panel, false);
    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "LCD FULL SLEEP: DISPLAY_OFF (DCS 0x28) accepted");
    } else {
        ESP_LOGW(
            TAG,
            "LCD FULL SLEEP: DISPLAY_OFF failed: %s",
            esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    ret = esp_lcd_panel_disp_sleep(panel, true);
    if (ret == ESP_OK) {
        s_panel_sleep_committed = true;
        ESP_LOGI(
            TAG,
            "LCD FULL SLEEP accepted: SLEEP_IN (DCS 0x10); "
            "panel scan/oscillator shutdown committed before transport teardown");
    } else {
        ESP_LOGW(
            TAG,
            "LCD FULL SLEEP: SLEEP_IN failed: %s",
            esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    return first_error;
}


bool display_platform_panel_sleep_committed(void)
{
    return s_panel_sleep_committed;
}


esp_err_t display_platform_suspend_for_light_sleep(
    bool prepare_panel_for_deep)
{
    /*
     * Critical Hybrid continuity fix:
     * issue SLEEP_IN while the panel handle is alive, then tear DSI down.
     */
    if (prepare_panel_for_deep) {
        const esp_err_t panel_ret =
            display_platform_panel_enter_full_sleep();

        if (panel_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Hybrid pre-Deep LCD SLEEP_IN completed with errors: %s; "
                "continuing with Light-sleep transport teardown",
                esp_err_to_name(panel_ret));
        } else {
            ESP_LOGI(
                TAG,
                "Hybrid continuity: LCD SLEEP_IN committed before Light-sleep "
                "MIPI-DSI teardown");
        }
    }

    return bsp_display_suspend_for_light_sleep();
}


lv_display_t *display_platform_resume_from_light_sleep(void)
{
    lv_display_t *display = bsp_display_resume_from_light_sleep();

    if (display != NULL) {
        s_panel_sleep_committed = false;
    }

    return display;
}


lv_indev_t *display_platform_get_input_device(void)
{
    return bsp_display_get_input_dev();
}


esp_err_t display_platform_poll_touch_for_light_sleep(bool *touched)
{
    return bsp_touch_poll_for_light_sleep(touched);
}


uint32_t display_platform_width(void)
{
    return (uint32_t)BSP_LCD_H_RES;
}


uint32_t display_platform_height(void)
{
    return (uint32_t)BSP_LCD_V_RES;
}
