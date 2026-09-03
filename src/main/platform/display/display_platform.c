#include "platform/display/display_platform.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "display_platform";


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
    return bsp_display_start();
}


esp_err_t display_platform_suspend_for_light_sleep(void)
{
    return bsp_display_suspend_for_light_sleep();
}


lv_display_t *display_platform_resume_from_light_sleep(void)
{
    return bsp_display_resume_from_light_sleep();
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

