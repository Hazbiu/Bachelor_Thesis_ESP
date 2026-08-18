#include "power_save/component_display.h"

#include <stdbool.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "config/app_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "component_display";

/*
 * GT911 command register. Writing 0x05 places the controller in sleep mode,
 * where it stops scanning the capacitive matrix. The controller leaves sleep
 * when its INT line is driven HIGH for more than ~2 ms, which is why the INT
 * pin is pinned LOW below when its number is known.
 */
#define GT911_COMMAND_REGISTER      0x8040
#define GT911_COMMAND_SLEEP         0x05
#define GT911_I2C_CLOCK_HZ          400000
#define GT911_I2C_TIMEOUT_MS        100
#define GT911_PROBE_TIMEOUT_MS      50

static bool s_backlight_held;
static bool s_touch_int_held;
static bool s_touch_reset_held;

/*
 * __attribute__((unused)) keeps the build clean when every optional pin below
 * is left at -1, which is the default until the schematic pin numbers are
 * filled in.
 */
__attribute__((unused))
static esp_err_t drive_and_hold(gpio_num_t gpio_num, int level, const char *name)
{
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << gpio_num,
        /*
         * INPUT_OUTPUT keeps the input path alive so the pre-sleep rail audit
         * in deep_sleep.c can read the pad back and prove the hold took.
         */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s GPIO%d configuration failed: %s",
                 name, (int)gpio_num, esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_level(gpio_num, level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not drive %s GPIO%d to %d: %s",
                 name, (int)gpio_num, level, esp_err_to_name(ret));
        return ret;
    }

    /*
     * Prevent ESP-IDF from switching this pad to its sleep configuration; the
     * active level must survive into Deep-sleep unchanged.
     */
    ret = gpio_sleep_sel_dis(gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable sleep switching for %s GPIO%d: %s",
                 name, (int)gpio_num, esp_err_to_name(ret));
    }

    ret = gpio_hold_en(gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not hold %s GPIO%d: %s",
                 name, (int)gpio_num, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "%s GPIO%d driven to %d and held",
             name, (int)gpio_num, level);
    return ESP_OK;
}

__attribute__((unused))
static esp_err_t release_hold(gpio_num_t gpio_num, const char *name)
{
    esp_err_t ret = gpio_hold_dis(gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not release %s GPIO%d hold: %s",
                 name, (int)gpio_num, esp_err_to_name(ret));
    }
    return ret;
}

#if APP_PWR_GT911_SLEEP_ENABLED
static esp_err_t gt911_write_sleep_command(
    i2c_master_bus_handle_t bus,
    uint8_t address)
{
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = GT911_I2C_CLOCK_HZ,
    };

    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = i2c_master_bus_add_device(bus, &device_config, &device);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not attach GT911 at 0x%02X: %s",
                 address, esp_err_to_name(ret));
        return ret;
    }

    const uint8_t payload[3] = {
        (uint8_t)(GT911_COMMAND_REGISTER >> 8),
        (uint8_t)(GT911_COMMAND_REGISTER & 0xFF),
        GT911_COMMAND_SLEEP,
    };

    ret = i2c_master_transmit(
        device,
        payload,
        sizeof(payload),
        GT911_I2C_TIMEOUT_MS);

    (void)i2c_master_bus_rm_device(device);
    return ret;
}

static esp_err_t gt911_enter_sleep(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if (bus == NULL) {
        ESP_LOGW(TAG,
                 "Shared I2C bus is already released; GT911 keeps scanning. "
                 "Move this call before bsp_display_shutdown_for_deep_sleep() "
                 "if the BSP deinitializes I2C.");
        return ESP_ERR_INVALID_STATE;
    }

    static const uint8_t addresses[] = {
        APP_PWR_GT911_PRIMARY_ADDRESS,
        APP_PWR_GT911_SECONDARY_ADDRESS,
    };

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        const uint8_t address = addresses[i];

        if (i2c_master_probe(bus, address, GT911_PROBE_TIMEOUT_MS) != ESP_OK) {
            continue;
        }

        esp_err_t ret = gt911_write_sleep_command(bus, address);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 at 0x%02X entered sleep mode", address);
            /* Give the controller time to act before the bus is isolated. */
            vTaskDelay(pdMS_TO_TICKS(10));
            return ESP_OK;
        }

        ESP_LOGW(TAG, "GT911 at 0x%02X rejected the sleep command: %s",
                 address, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG,
             "No GT911 answered at 0x%02X or 0x%02X; touch controller may "
             "keep scanning during Deep-sleep",
             APP_PWR_GT911_PRIMARY_ADDRESS,
             APP_PWR_GT911_SECONDARY_ADDRESS);

    return ESP_ERR_NOT_FOUND;
}
#endif /* APP_PWR_GT911_SLEEP_ENABLED */

esp_err_t component_display_disable_for_deep_sleep(void)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t ret = ESP_OK;

    /*
     * 1. Backlight first. The panel driver may still hold charge; removing the
     *    backlight enable is the single largest display-side saving.
     */
#if APP_PWR_DISPLAY_BACKLIGHT_GPIO >= 0
    ret = drive_and_hold(
        (gpio_num_t)APP_PWR_DISPLAY_BACKLIGHT_GPIO,
        APP_PWR_DISPLAY_BACKLIGHT_OFF_LEVEL,
        "backlight enable");

    if (ret == ESP_OK) {
        s_backlight_held = true;
    } else if (first_error == ESP_OK) {
        first_error = ret;
    }
#else
    ESP_LOGW(TAG,
             "APP_PWR_DISPLAY_BACKLIGHT_GPIO is unset; the backlight-enable "
             "pad is left floating during Deep-sleep. Fill it in from the "
             "Waveshare schematic.");
#endif

    /*
     * 2. Ask the GT911 to stop scanning while the I2C bus is still alive.
     */
#if APP_PWR_GT911_SLEEP_ENABLED
    ret = gt911_enter_sleep();
    if (ret != ESP_OK && first_error == ESP_OK) {
        /*
         * Deliberately not fatal. A touch controller that refuses to sleep
         * costs current, but must never block the shutdown sequence.
         */
        ESP_LOGW(TAG, "Continuing Deep-sleep shutdown without GT911 sleep");
    }
#endif

    /*
     * 3. Pin INT LOW so the controller cannot wake itself out of sleep, then
     *    optionally assert RESET, which is lower still.
     */
#if APP_PWR_TOUCH_INT_GPIO >= 0
    ret = drive_and_hold((gpio_num_t)APP_PWR_TOUCH_INT_GPIO, 0, "GT911 INT");
    if (ret == ESP_OK) {
        s_touch_int_held = true;
    } else if (first_error == ESP_OK) {
        first_error = ret;
    }
#endif

#if APP_PWR_TOUCH_RESET_GPIO >= 0
    ret = drive_and_hold(
        (gpio_num_t)APP_PWR_TOUCH_RESET_GPIO,
        APP_PWR_TOUCH_RESET_ACTIVE_LEVEL,
        "GT911 RESET");

    if (ret == ESP_OK) {
        s_touch_reset_held = true;
    } else if (first_error == ESP_OK) {
        first_error = ret;
    }
#endif

    (void)ret;

    ESP_LOGI(TAG,
             "Display side-channels prepared for Deep-sleep: "
             "backlight_held=%s touch_int_held=%s touch_reset_held=%s",
             s_backlight_held ? "yes" : "no",
             s_touch_int_held ? "yes" : "no",
             s_touch_reset_held ? "yes" : "no");

    return first_error;
}

esp_err_t component_display_restore_after_failed_sleep(void)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t ret = ESP_OK;

    (void)ret;

#if APP_PWR_TOUCH_RESET_GPIO >= 0
    if (s_touch_reset_held) {
        ret = release_hold((gpio_num_t)APP_PWR_TOUCH_RESET_GPIO, "GT911 RESET");
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        s_touch_reset_held = false;
    }
#endif

#if APP_PWR_TOUCH_INT_GPIO >= 0
    if (s_touch_int_held) {
        ret = release_hold((gpio_num_t)APP_PWR_TOUCH_INT_GPIO, "GT911 INT");
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        s_touch_int_held = false;
    }
#endif

#if APP_PWR_DISPLAY_BACKLIGHT_GPIO >= 0
    if (s_backlight_held) {
        ret = release_hold(
            (gpio_num_t)APP_PWR_DISPLAY_BACKLIGHT_GPIO,
            "backlight enable");
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        s_backlight_held = false;
    }
#endif

    if (first_error == ESP_OK) {
        ESP_LOGW(TAG, "Deep sleep failed; display side-channel holds released");
    }

    return first_error;
}
