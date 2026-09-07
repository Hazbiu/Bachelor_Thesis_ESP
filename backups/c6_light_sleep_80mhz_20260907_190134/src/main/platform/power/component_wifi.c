
#include "platform/power/component_wifi.h"

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
#define WIFI_C6_CHIP_PU_GPIO       APP_PWR_WIFI_C6_CHIP_PU_GPIO
#define WIFI_C6_DISABLED_LEVEL     APP_PWR_WIFI_C6_DISABLED_LEVEL
#define WIFI_C6_ENABLED_LEVEL      APP_PWR_WIFI_C6_ENABLED_LEVEL
#define WIFI_C6_MODE_GPIO          APP_PWR_WIFI_C6_MODE_GPIO
#define WIFI_C6_MODE_DEEP_LEVEL    APP_PWR_WIFI_C6_MODE_DEEP_LEVEL
#define WIFI_C6_MODE_LIGHT_LEVEL   APP_PWR_WIFI_C6_MODE_LIGHT_LEVEL

static const char *TAG = "component_wifi";

/*
 * Diagnostic state only. It must NEVER be used to skip GPIO54 programming:
 * the final Deep-sleep path must re-apply CHIP_PU LOW even if boot already
 * disabled the C6.
 */
static bool s_c6_disabled;
static bool s_user_enabled = true;

/*
 * GPIO6 is physically wired to ESP32-C6 GPIO2 through R52 (0 ohm).
 * It is intentionally used as a dedicated power-mode sideband by this build:
 *
 *   HIGH -> C6 boots into retained 80 MHz companion mode
 *   LOW  -> normal/deep policy
 *
 * GPIO6 belongs to the P4 VDD_LP domain. Keep the pad held at the requested
 * level across P4 Light-sleep, and keep LOW at the real Deep-sleep boundary so
 * the C6 self-sleep firmware can distinguish the two P4 states.
 */
static esp_err_t configure_c6_mode_pin(int level, bool hold_level)
{
    const gpio_num_t pin = WIFI_C6_MODE_GPIO;

    esp_err_t ret = gpio_hold_dis(pin);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Could not release C6 mode GPIO%d hold: %s",
                 pin, esp_err_to_name(ret));
        return ret;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(pin, level);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_sleep_sel_dis(pin);
    if (ret != ESP_OK) {
        return ret;
    }

    if (hold_level) {
        ret = gpio_hold_en(pin);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Could not hold C6 mode GPIO%d at level %d: %s",
                     pin, level, esp_err_to_name(ret));
            return ret;
        }
    }

    const int readback = gpio_get_level(pin);
    if (readback != level) {
        ESP_LOGE(TAG, "C6 mode GPIO%d reads %d; expected %d",
                 pin, readback, level);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG,
             "C6 mode sideband: P4 GPIO%d -> C6 GPIO2 level=%d hold=%s",
             pin, level, hold_level ? "YES" : "NO");
    return ESP_OK;
}

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

static esp_err_t configure_c6_chip_pu_high(void)
{
    const gpio_num_t pin = WIFI_C6_CHIP_PU_GPIO;

    esp_err_t ret = gpio_hold_dis(pin);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Could not release GPIO%d hold: %s",
                 pin, esp_err_to_name(ret));
        return ret;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(pin, WIFI_C6_ENABLED_LEVEL);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_sleep_sel_dis(pin);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t settle_ms = 0;
    const int level = wait_for_pad_level(
        pin,
        WIFI_C6_ENABLED_LEVEL,
        &settle_ms);

    if (level != WIFI_C6_ENABLED_LEVEL) {
        ESP_LOGE(TAG, "ESP32-C6 CHIP_PU reads %d; expected enabled level %d",
                 level, WIFI_C6_ENABLED_LEVEL);
        return ESP_ERR_INVALID_STATE;
    }

    s_c6_disabled = false;
    ESP_LOGI(TAG,
             "ESP32-C6 enabled: GPIO%d CHIP_PU HIGH (read-back=%d after %" PRIu32 " ms)",
             pin, level, settle_ms);
    return ESP_OK;
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

esp_err_t component_wifi_prepare_for_light_sleep(void)
{
    /*
     * HIGH must be visible on C6 GPIO2 before CHIP_PU is released. The C6
     * companion samples this sideband at boot and selects its 80 MHz policy.
     */
    esp_err_t ret = configure_c6_mode_pin(
        WIFI_C6_MODE_LIGHT_LEVEL,
        true);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Force a clean C6 boot into the new mode even if it was already sleeping. */
    ret = configure_c6_chip_pu_low(false, true);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(APP_PWR_WIFI_C6_RESET_PULSE_MS));

    ret = configure_c6_chip_pu_high();
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(APP_PWR_WIFI_C6_LIGHT_BOOT_SETTLE_MS));
    ESP_LOGI(
        TAG,
        "ESP32-C6 retained for P4 Light-sleep: CHIP_PU=HIGH mode_gpio=HIGH "
        "requested_cpu=80MHz");
    return ESP_OK;
}

esp_err_t component_wifi_restore_after_light_sleep(void)
{
    /*
     * LOW is the normal/deep sideband level. If the C6 is already running in
     * retained mode, its companion firmware sees this transition and restores
     * its 160 MHz ACTIVE policy. If Wi-Fi is disabled by the saved setting, we
     * then assert CHIP_PU LOW again so the user's persistent policy still wins.
     */
    esp_err_t ret = configure_c6_mode_pin(
        WIFI_C6_MODE_DEEP_LEVEL,
        false);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_user_enabled) {
        if (s_c6_disabled) {
            ret = configure_c6_chip_pu_high();
            if (ret != ESP_OK) {
                return ret;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APP_PWR_WIFI_C6_ACTIVE_SETTLE_MS));
        ESP_LOGI(
            TAG,
            "ESP32-C6 restored after P4 Light-sleep: CHIP_PU=HIGH "
            "mode_gpio=LOW requested_cpu=160MHz");
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Saved Wi-Fi policy is OFF; returning ESP32-C6 to CHIP_PU LOW after Light-sleep");
    return configure_c6_chip_pu_low(false, true);
}

esp_err_t component_wifi_prepare_mode_for_deep_sleep(void)
{
    return configure_c6_mode_pin(
        WIFI_C6_MODE_DEEP_LEVEL,
        true);
}

esp_err_t component_wifi_disable_for_deep_sleep(void)
{
    /*
     * Preserve the existing P4 Deep-sleep strategy, but make the C6 boot reason
     * deterministic. GPIO6/C6-GPIO2 LOW means that when GPIO54 later rises in
     * real P4 Deep-sleep, the companion firmware immediately self-Deep-sleeps.
     */
    esp_err_t ret = component_wifi_prepare_mode_for_deep_sleep();
    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * Intentionally no "if (s_c6_disabled) return" shortcut. Reprogramming
     * GPIO54 on every call guarantees the final Deep-sleep boundary is real.
     */
    return configure_c6_chip_pu_low(false, true);
}

esp_err_t component_wifi_force_off_at_sleep_boundary(void)
{
    /* Last application-controlled mode + CHIP_PU writes before Deep-sleep. */
    esp_err_t ret = component_wifi_prepare_mode_for_deep_sleep();
    if (ret != ESP_OK) {
        return ret;
    }

    return configure_c6_chip_pu_low(true, false);
}

esp_err_t component_wifi_restore_after_failed_sleep(void)
{
    /* Remove the Deep-sleep mode request before restoring the saved policy. */
    esp_err_t mode_ret = configure_c6_mode_pin(
        WIFI_C6_MODE_DEEP_LEVEL,
        false);
    if (mode_ret != ESP_OK) {
        return mode_ret;
    }

    if (s_user_enabled) {
        ESP_LOGI(TAG, "Restoring ESP32-C6 because the saved Wi-Fi policy is ON");
        return configure_c6_chip_pu_high();
    }

    ESP_LOGI(TAG, "ESP32-C6 remains disabled because the saved Wi-Fi policy is OFF");
    return configure_c6_chip_pu_low(false, true);
}

esp_err_t component_wifi_set_enabled(bool enabled)
{
    /*
     * Keep the mode sideband LOW during normal application operation. This also
     * makes an ordinary C6 boot select the same self-Deep-sleep behavior as the
     * previous firmware until P4 Light-sleep explicitly requests 80 MHz mode.
     */
    esp_err_t ret = configure_c6_mode_pin(
        WIFI_C6_MODE_DEEP_LEVEL,
        false);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = enabled
        ? configure_c6_chip_pu_high()
        : configure_c6_chip_pu_low(false, true);

    if (ret == ESP_OK) {
        s_user_enabled = enabled;
    }

    return ret;
}

bool component_wifi_is_enabled(void)
{
    return s_user_enabled && !s_c6_disabled;
}
