
#include "platform/display/display_platform.h"

#include <stdio.h>
#include <stdint.h>
#include "esp_sleep.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "config/app_config.h"
#include "platform/power/cpu_power.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_platform";
/* All writes use the application's existing display ownership/barrier.
 * Invalidate at lifecycle boundaries because the BSP also writes brightness. */
static int s_last_backlight_percent = -1;

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
 * DISPLAY_OFF / SLEEP_IN / MIPI teardown. This update preserves the supplied
 * recovery bytes and its zero extra settling delay. The panel command waits
 * are handled separately in display_platform_panel_enter_full_sleep().
 */
#define DISPLAY_CTRL_I2C_ADDRESS              0x45U
#define DISPLAY_CTRL_STATE_REGISTER           0x95U
#define DISPLAY_CTRL_BRIGHTNESS_REGISTER      0x96U
#define DISPLAY_CTRL_RECOVERY_STATE           0x11U
#define DISPLAY_CTRL_ACTIVE_STATE             0x17U
#define DISPLAY_CTRL_I2C_CLOCK_HZ             100000U
#define DISPLAY_CTRL_I2C_TIMEOUT_MS           100
#define DISPLAY_CTRL_RECOVERY_SETTLE_MS       0U
#define DISPLAY_CTRL_ACTIVE_RESUME_SETTLE_MS  100U
#define DISPLAY_CTRL_ACTIVE_RESUME_RETRIES    3U
#define DISPLAY_CTRL_ACTIVE_RETRY_DELAY_MS    20U

/*
 * Deep-sleep entry deliberately calls rtc_gpio_isolate() on the shared I2C
 * pins (GPIO7/GPIO8). rtc_gpio_isolate() disables the pad input/output/pulls
 * and enables the RTC hold. A real Deep-sleep wake resets the application, but
 * the RTC pad state can still be retained long enough to prevent the first I2C
 * transaction from clearing the bus.
 *
 * Release that exact pre-sleep isolation BEFORE recreating the BSP I2C bus.
 * Without this, the address-0x45 wake sequence can fail with
 * ESP_ERR_INVALID_STATE even though bsp_i2c_init() itself succeeded.
 */
static esp_err_t display_platform_release_shared_i2c_after_deep_sleep(void)
{
    static const gpio_num_t i2c_pins[] = {
        APP_PWR_SHARED_I2C_SDA_GPIO,
        APP_PWR_SHARED_I2C_SCL_GPIO,
    };

    esp_err_t first_error = ESP_OK;

    for (size_t i = 0; i < sizeof(i2c_pins) / sizeof(i2c_pins[0]); ++i) {
        const gpio_num_t pin = i2c_pins[i];

        if (!rtc_gpio_is_valid_gpio(pin)) {
            ESP_LOGE(
                TAG,
                "[DISPLAY-CTRL-WAKE] GPIO%d is not RTC-capable; "
                "cannot release Deep-sleep I2C isolation",
                (int)pin);
            if (first_error == ESP_OK) {
                first_error = ESP_ERR_INVALID_ARG;
            }
            continue;
        }

        /*
         * rtc_gpio_isolate() enables the RTC hold. Release the hold first,
         * then route the pad back to the normal digital IO mux. bsp_i2c_init()
         * will install the final SDA/SCL peripheral configuration afterwards.
         */
        esp_err_t ret = rtc_gpio_hold_dis(pin);
        if (ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "[DISPLAY-CTRL-WAKE] rtc hold release failed on GPIO%d: %s",
                (int)pin,
                esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }

        ret = rtc_gpio_deinit(pin);
        if (ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "[DISPLAY-CTRL-WAKE] RTC mux release failed on GPIO%d: %s",
                (int)pin,
                esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }

    /*
     * The board has external 2.2K pull-ups on GPIO7/GPIO8. Give the released
     * pads a short settling window before the I2C peripheral starts toggling.
     */
    vTaskDelay(pdMS_TO_TICKS(2));

    if (first_error == ESP_OK) {
        ESP_LOGI(
            TAG,
            "[DISPLAY-CTRL-WAKE] released Deep-sleep RTC isolation on "
            "GPIO%d/GPIO%d",
            (int)APP_PWR_SHARED_I2C_SDA_GPIO,
            (int)APP_PWR_SHARED_I2C_SCL_GPIO);
    }

    return first_error;
}


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
 * Re-arm the Waveshare display/backlight controller after reversible
 * Light-sleep.
 *
 * The normal Waveshare power-on sequence for the I2C controller at address
 * 0x45 is:
 *
 *     0x95 <- 0x11
 *     0x95 <- 0x17
 *     0x96 <- 0x00
 *
 * The Deep/Light suspend optimization above deliberately leaves register 0x95
 * at 0x11 before MIPI-DSI is torn down. Recreating DSI/LVGL alone therefore
 * does not guarantee that the external controller has returned to its Active
 * state. Re-issue the board's normal controller sequence after BSP display
 * recreation, keep brightness at zero during reconstruction, and let the first
 * complete camera frame apply the configured Active brightness.
 */
static esp_err_t display_platform_apply_controller_light_sleep_wake_once(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(
            TAG,
            "[DISPLAY-CTRL-WAKE] shared BSP I2C bus unavailable");
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
            "[DISPLAY-CTRL-WAKE] could not open controller 0x%02X: %s",
            DISPLAY_CTRL_I2C_ADDRESS,
            esp_err_to_name(ret));
        return ret;
    }

    static const uint8_t wake_sequence[][2] = {
        {DISPLAY_CTRL_STATE_REGISTER,      DISPLAY_CTRL_RECOVERY_STATE},
        {DISPLAY_CTRL_STATE_REGISTER,      DISPLAY_CTRL_ACTIVE_STATE},
        {DISPLAY_CTRL_BRIGHTNESS_REGISTER, 0x00U},
    };

    for (size_t i = 0; i < sizeof(wake_sequence) / sizeof(wake_sequence[0]); ++i) {
        ret = i2c_master_transmit(
            device,
            wake_sequence[i],
            sizeof(wake_sequence[i]),
            DISPLAY_CTRL_I2C_TIMEOUT_MS);

        if (ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "[DISPLAY-CTRL-WAKE] write reg=0x%02X value=0x%02X failed: %s",
                wake_sequence[i][0],
                wake_sequence[i][1],
                esp_err_to_name(ret));
            break;
        }

        ESP_LOGI(
            TAG,
            "[DISPLAY-CTRL-WAKE] 0x45 reg 0x%02X <- 0x%02X",
            wake_sequence[i][0],
            wake_sequence[i][1]);
    }

    const esp_err_t remove_ret = i2c_master_bus_rm_device(device);
    if (ret == ESP_OK && remove_ret != ESP_OK) {
        ret = remove_ret;
    }

    if (ret == ESP_OK) {
        /*
         * Match the board's normal controller initialization settling interval
         * before allowing camera scanout/backlight brightness to resume.
         */
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_CTRL_ACTIVE_RESUME_SETTLE_MS));
        ESP_LOGI(
            TAG,
            "[DISPLAY-CTRL-WAKE] Active controller sequence restored; "
            "backlight intentionally held at 0 until first camera frame");
    }

    return ret;
}


static esp_err_t display_platform_apply_controller_light_sleep_wake(void)
{
    esp_err_t ret = ESP_FAIL;

    for (uint32_t attempt = 1;
         attempt <= DISPLAY_CTRL_ACTIVE_RESUME_RETRIES;
         ++attempt) {

        ret = display_platform_apply_controller_light_sleep_wake_once();
        if (ret == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(
                    TAG,
                    "[DISPLAY-CTRL-WAKE] recovered on attempt %u",
                    (unsigned)attempt);
            }
            return ESP_OK;
        }

        ESP_LOGW(
            TAG,
            "[DISPLAY-CTRL-WAKE] attempt %u/%u failed: %s",
            (unsigned)attempt,
            (unsigned)DISPLAY_CTRL_ACTIVE_RESUME_RETRIES,
            esp_err_to_name(ret));

        /*
         * ESP_ERR_INVALID_STATE/ESP_ERR_TIMEOUT after Deep-sleep commonly
         * indicates that the I2C bus-clear state machine saw a stale/stuck bus.
         * Reset the already-created BSP bus before the next retry.
         */
        if (attempt < DISPLAY_CTRL_ACTIVE_RESUME_RETRIES &&
            (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_TIMEOUT)) {
            i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
            if (bus != NULL) {
                const esp_err_t reset_ret = i2c_master_bus_reset(bus);
                ESP_LOGW(
                    TAG,
                    "[DISPLAY-CTRL-WAKE] I2C bus reset before retry: %s",
                    esp_err_to_name(reset_ret));
            }
        }

        if (attempt < DISPLAY_CTRL_ACTIVE_RESUME_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_CTRL_ACTIVE_RETRY_DELAY_MS));
        }
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


esp_err_t display_platform_backlight_set_percent(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent == s_last_backlight_percent) {
        return ESP_OK;
    }
    const esp_err_t ret = bsp_display_brightness_set(percent);
    if (ret == ESP_OK) {
        s_last_backlight_percent = percent;
        ESP_LOGI("PWR_STATE", "PWR-OPT-3: backlight request=%d%% accepted", percent);
    } else {
        /* Do not cache a failed write: the next frame must be able to retry. */
        s_last_backlight_percent = -1;
        ESP_LOGW("PWR_STATE", "PWR-OPT-3: backlight request=%d%% failed: %s",
                 percent, esp_err_to_name(ret));
    }
    return ret;
}


void display_platform_backlight_on(void)
{
    (void)display_platform_backlight_set_percent(
        cpu_power_active_optimization_is_enabled()
            ? APP_BACKLIGHT_ACTIVE_PERCENT : 100);
}


void display_platform_backlight_off(void)
{
    /* OFF is never suppressed by the cache: external/BSP code may have
     * changed the controller since the last application brightness request. */
    s_last_backlight_percent = -1;
    (void)display_platform_backlight_set_percent(0);
}


lv_display_t *display_platform_start(void)
{
    s_last_backlight_percent = -1;

    const esp_sleep_wakeup_cause_t wake_cause =
        esp_sleep_get_wakeup_cause();

    /*
     * A true Deep-sleep wake is a CPU/application reset, but the external
     * Waveshare display controller is still powered and the pre-sleep code
     * explicitly isolated GPIO7/GPIO8 with rtc_gpio_isolate().
     *
     * Correct order:
     *   1. release GPIO7/GPIO8 RTC hold/isolation;
     *   2. recreate the BSP I2C bus;
     *   3. restore the address-0x45 controller to ACTIVE;
     *   4. let bsp_display_start() rebuild JD9365/MIPI-DSI/LVGL/touch.
     */
    if (wake_cause == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(
            TAG,
            "Deep-sleep GPIO wake: releasing I2C isolation and restoring "
            "display controller before BSP startup");

        esp_err_t ret =
            display_platform_release_shared_i2c_after_deep_sleep();

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not fully release Deep-sleep I2C isolation: %s",
                esp_err_to_name(ret));
        }

        ret = bsp_i2c_init();

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not initialize BSP I2C before Deep-sleep display recovery: %s",
                esp_err_to_name(ret));
        } else {
            /*
             * bsp_i2c_init() may return ESP_OK only after the driver object is
             * installed. Prove that the handle is usable before attempting the
             * controller sequence so failures are explicit in the serial log.
             */
            i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

            if (bus == NULL) {
                ESP_LOGE(
                    TAG,
                    "BSP I2C initialized but returned a NULL bus handle");
                ret = ESP_ERR_INVALID_STATE;
            } else {
                ret = display_platform_apply_controller_light_sleep_wake();
            }

            if (ret != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Deep-sleep display-controller wake failed: %s",
                    esp_err_to_name(ret));
            } else {
                ESP_LOGI(
                    TAG,
                    "Deep-sleep display controller restored before BSP startup");
            }
        }
    }

    lv_display_t *display = bsp_display_start();

    if (display != NULL) {
        s_panel_sleep_committed = false;
    } else {
        ESP_LOGE(
            TAG,
            "bsp_display_start() failed after Deep-sleep/cold-boot initialization");
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

    s_last_backlight_percent = -1;
    esp_err_t ret = display_platform_backlight_set_percent(0);
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

    /* Datasheet section 9.5.3: tDISOFF >= 50 ms when DISPLAY_OFF is used. */
    vTaskDelay(pdMS_TO_TICKS(APP_LCD_DISPLAY_OFF_SETTLE_MS) + 1);

    ret = esp_lcd_panel_disp_sleep(panel, true);
    if (ret == ESP_OK) {
        /* JD9365DA-H3 datasheet section 10.2.15 (p.144): Sleep-In takes
         * 120 ms, and DPI timing continues for two frames after SLPIN.
         * Keep the transport alive here. The extra tick prevents tick-phase
         * rounding from making a nominal 120 ms delay shorter than 120 ms.
         * This wait also applies when the installed panel driver waits too. */
        vTaskDelay(pdMS_TO_TICKS(APP_LCD_SLEEP_IN_SETTLE_MS) + 1);
        s_panel_sleep_committed = true;
        ESP_LOGI(
            "PWR_STATE",
            "PWR-OPT-3: LCD SLEEP_IN accepted; >=%u ms settling complete "
            "before transport teardown; panel current not measured here",
            (unsigned)APP_LCD_SLEEP_IN_SETTLE_MS);
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

    s_last_backlight_percent = -1;
    return bsp_display_suspend_for_light_sleep();
}


lv_display_t *display_platform_resume_from_light_sleep(void)
{
    s_last_backlight_percent = -1;

    /*
     * Wake the external address-0x45 controller BEFORE recreating MIPI/panel
     * resources. The shared BSP I2C bus is intentionally retained for GT911
     * Light-sleep polling, so it is available at this point even though
     * LVGL/MIPI-DSI are down.
     *
     * This ordering mirrors the Waveshare board initialization requirement:
     * the external controller must be returned to its Active state before the
     * JD9365/MIPI path is initialized.
     */
    const esp_err_t controller_ret =
        display_platform_apply_controller_light_sleep_wake();

    if (controller_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Display controller wake sequence failed before MIPI restoration: %s",
            esp_err_to_name(controller_ret));

        /*
         * Continue with BSP recreation as a best-effort fallback. The error is
         * explicit in the serial monitor, and the normal first-frame brightness
         * request remains able to retry register 0x96.
         */
    }

    lv_display_t *display = bsp_display_resume_from_light_sleep();
    if (display == NULL) {
        ESP_LOGE(
            TAG,
            "BSP display recreation failed during Light-sleep wake");
        return NULL;
    }

    s_panel_sleep_committed = false;
    s_last_backlight_percent = -1;

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
