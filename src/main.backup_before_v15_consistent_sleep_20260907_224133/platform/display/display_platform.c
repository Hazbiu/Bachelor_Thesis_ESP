#include "platform/display/display_platform.h"

#include <stdio.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_platform";

/*
 * External display-controller Deep-sleep recovery
 * ------------------------------------------------
 *
 * Controlled Joulescope tests on this exact board isolated the retained
 * current to an interaction between the external address-0x45 controller
 * state and the active MIPI-DPI display path:
 *
 *   raw DPI only                  -> ~41.63 mA Deep-sleep
 *   0x95 = 0x17 only             -> ~47.57 mA
 *   0x95 = 0x17 + raw DPI        -> ~64.99 mA
 *   then 0x95 = 0x11 recovery    -> ~41.82 mA
 *
 * The electrical meaning of register 0x95 is not documented here.  Do not
 * rename it as a "power" register.  We reproduce only the measured recovery
 * sequence that was experimentally effective:
 *
 *     0x96 <- 0x00
 *     0x95 <- 0x11
 *     0x96 <- 0x00
 *
 * It MUST run while the panel/DPI path is still alive and before the normal
 * DISPLAY_OFF / SLEEP_IN / MIPI teardown.  The 2 s settle interval deliberately
 * matches the successful Test13 experiment.  Shorten it only after the full
 * application has been re-measured.
 */
#define DISPLAY_CTRL_I2C_ADDRESS              0x45U
#define DISPLAY_CTRL_STATE_REGISTER           0x95U
#define DISPLAY_CTRL_BRIGHTNESS_REGISTER      0x96U
#define DISPLAY_CTRL_RECOVERY_STATE           0x11U
#define DISPLAY_CTRL_I2C_CLOCK_HZ             100000U
#define DISPLAY_CTRL_I2C_TIMEOUT_MS           100
#define DISPLAY_CTRL_RECOVERY_SETTLE_MS       0U

static esp_err_t display_platform_apply_controller_deep_sleep_recovery(void)
{
    /*
     * This project is the measured JD9365 ESP32-P4-NANO configuration.
     * Do not gate this recovery on optional BSP Kconfig macro spellings:
     * the previous V2 gate evaluated false on the real build and silently
     * compiled the recovery into a no-op.
     */
    printf("[DISPLAY-CTRL-RECOVERY] BEGIN address=0x45 sequence=96:00,95:11,96:00\n");
    fflush(stdout);

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(
            TAG,
            "[DISPLAY-CTRL-RECOVERY] shared BSP I2C bus unavailable; "
            "cannot apply address-0x45 recovery");
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DISPLAY_CTRL_I2C_ADDRESS,
        .scl_speed_hz = DISPLAY_CTRL_I2C_CLOCK_HZ,
    };

    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = i2c_master_bus_add_device(
        bus,
        &device_config,
        &device);

    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "[DISPLAY-CTRL-RECOVERY] could not open controller 0x%02X: %s",
            DISPLAY_CTRL_I2C_ADDRESS,
            esp_err_to_name(ret));
        return ret;
    }

    static const uint8_t sequence[][2] = {
        {DISPLAY_CTRL_BRIGHTNESS_REGISTER, 0x00U},
        {DISPLAY_CTRL_STATE_REGISTER,      DISPLAY_CTRL_RECOVERY_STATE},
        {DISPLAY_CTRL_BRIGHTNESS_REGISTER, 0x00U},
    };

    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        ret = i2c_master_transmit(
            device,
            sequence[i],
            sizeof(sequence[i]),
            DISPLAY_CTRL_I2C_TIMEOUT_MS);

        if (ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "[DISPLAY-CTRL-RECOVERY] write reg=0x%02X value=0x%02X failed: %s",
                sequence[i][0],
                sequence[i][1],
                esp_err_to_name(ret));
            break;
        }

        ESP_LOGI(
            TAG,
            "[DISPLAY-CTRL-RECOVERY] 0x45 reg 0x%02X <- 0x%02X",
            sequence[i][0],
            sequence[i][1]);
        printf("[DISPLAY-CTRL-RECOVERY] 0x45 reg 0x%02X <- 0x%02X\n",
               sequence[i][0], sequence[i][1]);
        fflush(stdout);
    }

    const esp_err_t remove_ret = i2c_master_bus_rm_device(device);
    if (ret == ESP_OK && remove_ret != ESP_OK) {
        ret = remove_ret;
    }

    if (ret == ESP_OK) {
        ESP_LOGW(
            TAG,
            "[DISPLAY-CTRL-RECOVERY] Test13 recovery sequence completed; "
            "keeping panel/DPI active for %u ms before DCS sleep",
            (unsigned)DISPLAY_CTRL_RECOVERY_SETTLE_MS);
        printf("[DISPLAY-CTRL-RECOVERY] COMPLETE settle_ms=%u before DCS sleep\n",
               (unsigned)DISPLAY_CTRL_RECOVERY_SETTLE_MS);
        fflush(stdout);
    }

    return ret;
}


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

    /*
     * Test13 proved that this recovery must be applied while the MIPI-DPI
     * display path is still alive.  Run it after backlight OFF and before the
     * normal DCS DISPLAY_OFF / SLEEP_IN sequence.
     */
    ret = display_platform_apply_controller_deep_sleep_recovery();
    if (ret == ESP_OK) {
        /*
         * V15: no artificial 2-second plateau. The controller recovery writes
         * are followed immediately by the normal DCS shutdown sequence.
         */
#if DISPLAY_CTRL_RECOVERY_SETTLE_MS > 0
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_CTRL_RECOVERY_SETTLE_MS));
#endif
    } else {
        ESP_LOGW(
            TAG,
            "LCD FULL SLEEP: display-controller recovery completed with "
            "errors: %s; continuing with DCS shutdown",
            esp_err_to_name(ret));

        if (first_error == ESP_OK) {
            first_error = ret;
        }
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
                "Light-sleep LCD SLEEP_IN completed with errors: %s; "
                "continuing with Light-sleep transport teardown",
                esp_err_to_name(panel_ret));
        } else {
            ESP_LOGI(
                TAG,
                "Light-sleep continuity: LCD SLEEP_IN committed before Light-sleep "
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
