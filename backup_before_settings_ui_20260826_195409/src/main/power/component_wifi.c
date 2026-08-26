#include "power_save/component_wifi.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Waveshare ESP32-P4-NANO connection:
 *
 * ESP32-P4 GPIO54 -> R54 -> ESP32-C6 CHIP_PU
 *
 * LOW  = ESP32-C6 held in reset
 * HIGH = ESP32-C6 enabled
 */
#define WIFI_C6_CHIP_PU_GPIO    APP_PWR_WIFI_C6_CHIP_PU_GPIO
#define WIFI_C6_DISABLED_LEVEL  APP_PWR_WIFI_C6_DISABLED_LEVEL
#define WIFI_C6_ENABLED_LEVEL   APP_PWR_WIFI_C6_ENABLED_LEVEL

static const char *TAG = "component_wifi";

/*
 * Diagnostic state only. It must NEVER be used to skip GPIO54 programming:
 * the final Deep-sleep path must re-apply CHIP_PU LOW even if boot already
 * disabled the C6.
 */
static bool s_c6_disabled;

/*
 * Poll the pad until it reaches the expected level, or until the settle
 * window expires.
 *
 * A pad does not always report its new level on the instruction immediately
 * after gpio_hold_en(). On this board GPIO54 reads HIGH right after the hold
 * and LOW a few tens of milliseconds later, which previously produced a false
 * "coprocessor is still powered" error even though the pre-sleep rail audit
 * then reported the pin correctly held LOW.
 *
 * Returns the last level read and reports how long the pad took to settle.
 */
static int wait_for_pad_level(gpio_num_t gpio_num, int expected_level,
                              uint32_t *settle_ms_out)
{
    uint32_t waited_ms = 0;
    int level = gpio_get_level(gpio_num);

    while (level != expected_level && waited_ms < APP_PWR_HOLD_VERIFY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(APP_PWR_HOLD_VERIFY_POLL_MS));
        waited_ms += APP_PWR_HOLD_VERIFY_POLL_MS;
        level = gpio_get_level(gpio_num);
    }

    if (settle_ms_out != NULL) {
        *settle_ms_out = waited_ms;
    }

    return level;
}

static esp_err_t configure_c6_chip_pu_low(
    bool final_sleep_boundary,
    bool wait_for_settle)
{
    const gpio_num_t pin = WIFI_C6_CHIP_PU_GPIO;

    /*
     * Release any old latch first. The pin is immediately reconfigured and
     * driven LOW below, so the previous "disabled" software state is never
     * trusted as proof of the physical pad state.
     */
    esp_err_t ret = gpio_hold_dis(pin);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(
            TAG,
            "Could not release previous GPIO%d hold: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << pin,

        /*
         * Keep input enabled so gpio_get_level() and the pre-sleep audit can
         * measure the physical pad while it is actively driven LOW.
         */
        .mode = GPIO_MODE_INPUT_OUTPUT,

        /*
         * Never allow an internal pull-up on C6 CHIP_PU. Enable the internal
         * pull-down as a secondary safeguard while the HP GPIO block is alive.
         *
         * IMPORTANT: ESP32-P4 revision v1.3 cannot rely on this internal pull
         * after the HP GPIO domain powers down. A board-level pull-down remains
         * the only guaranteed Deep-sleep clamp.
         */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO%d configuration failed: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not set GPIO%d drive strength: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_level(pin, WIFI_C6_DISABLED_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not drive ESP32-C6 CHIP_PU LOW: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Do not let the normal GPIO sleep-selection mechanism substitute a
     * different pad configuration for GPIO54.
     */
    ret = gpio_sleep_sel_dis(pin);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable sleep switching for GPIO%d: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Latch output-enable, LOW output value, GPIO function and drive strength.
     * This is useful while the hardware supports the latch, but on this board
     * the silicon is ESP32-P4 v1.3 and the measured GPIO54 level still rises
     * after the real Deep-sleep transition. Do not treat this hold as a
     * replacement for the external C6 CHIP_PU pull-down.
     */
    ret = gpio_hold_en(pin);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d LOW: %s",
            pin,
            esp_err_to_name(ret));
        return ret;
    }

    s_c6_disabled = true;

    uint32_t settle_ms = 0;
    int level_after_hold;

    if (wait_for_settle) {
        level_after_hold = wait_for_pad_level(
            pin,
            WIFI_C6_DISABLED_LEVEL,
            &settle_ms);
    } else {
        /*
         * At the final boundary do not add another FreeRTOS delay. GPIO54 was
         * already clamped during STEP 6; this is only the final re-latch.
         */
        level_after_hold = gpio_get_level(pin);
    }

    if (level_after_hold != WIFI_C6_DISABLED_LEVEL) {
        ESP_LOGE(
            TAG,
            "ESP32-C6 CHIP_PU reads %d after final GPIO54 clamp; expected %d",
            level_after_hold,
            WIFI_C6_DISABLED_LEVEL);
        return ESP_ERR_INVALID_STATE;
    }

    if (final_sleep_boundary) {
        ESP_LOGW(
            TAG,
            "C6-FINAL: GPIO%d CHIP_PU re-clamped LOW immediately before "
            "Deep-sleep (read-back=%d, pullup=OFF pulldown=ON sleep-switch=OFF). "
            "ESP32-P4 silicon is v1.3: external CHIP_PU pull-down is still "
            "required for guaranteed LOW after HP GPIO power-down.",
            pin,
            level_after_hold);
    } else {
        ESP_LOGI(
            TAG,
            "ESP32-C6 disabled: GPIO%d CHIP_PU re-applied LOW "
            "(read-back=%d after %" PRIu32 " ms)",
            pin,
            level_after_hold,
            settle_ms);
    }

    return ESP_OK;
}

esp_err_t component_wifi_disable_for_deep_sleep(void)
{
    /*
     * Intentionally no "if (s_c6_disabled) return" shortcut.
     *
     * app_main() disables the unused C6 during boot, while the ordered sleep
     * sequence calls this function again at STEP 6. Reprogramming the pad on
     * every call guarantees that STEP 6 really does work instead of merely
     * reporting success from a stale software flag.
     */
    return configure_c6_chip_pu_low(false, true);
}

esp_err_t component_wifi_force_off_at_sleep_boundary(void)
{
    /*
     * Last application-controlled GPIO54 write before esp_deep_sleep_start().
     * No task delay is used here.
     */
    return configure_c6_chip_pu_low(true, false);
}

esp_err_t component_wifi_restore_after_failed_sleep(void)
{
    /* C6 is intentionally kept disabled permanently. */
    ESP_LOGI(
        TAG,
        "ESP32-C6 remains disabled; GPIO54 software state will be re-applied "
        "on the next sleep request");
    return ESP_OK;
}
