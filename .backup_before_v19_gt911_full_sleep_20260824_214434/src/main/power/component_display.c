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

/*
 * GT911 configuration area used by the safe software-only Green-mode policy.
 *
 * 0x8055 Low_Power_Control:
 *   low nibble = idle seconds before automatic lower-power mode (0..15 s)
 *
 * Any change inside 0x8047..0x80FE requires a new two's-complement checksum
 * at 0x80FF and Config_Fresh=1 at 0x8100.  The code below validates the
 * existing checksum before touching the configuration, so a corrupt/unknown
 * panel configuration is never rewritten blindly.
 */
#define GT911_CONFIG_START_REGISTER        0x8047
#define GT911_CONFIG_END_REGISTER          0x80FE
#define GT911_CONFIG_CHECKSUM_REGISTER     0x80FF
#define GT911_CONFIG_FRESH_REGISTER        0x8100
#define GT911_LOW_POWER_CONTROL_REGISTER   0x8055
#define GT911_CONFIG_LENGTH \
    (GT911_CONFIG_END_REGISTER - GT911_CONFIG_START_REGISTER + 1U)
#define GT911_LOW_POWER_INDEX \
    (GT911_LOW_POWER_CONTROL_REGISTER - GT911_CONFIG_START_REGISTER)

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


#if APP_PWR_GT911_GREEN_MODE_ENABLED

static bool s_gt911_green_mode_verified;
static uint8_t s_gt911_green_mode_address;

static esp_err_t gt911_open_device(
    i2c_master_bus_handle_t bus,
    uint8_t address,
    i2c_master_dev_handle_t *device_out)
{
    if (bus == NULL || device_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_out = NULL;

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = GT911_I2C_CLOCK_HZ,
    };

    return i2c_master_bus_add_device(bus, &device_config, device_out);
}

static esp_err_t gt911_read_bytes(
    i2c_master_dev_handle_t device,
    uint16_t reg,
    uint8_t *data,
    size_t length)
{
    if (device == NULL || data == NULL || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t address_bytes[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFFU),
    };

    return i2c_master_transmit_receive(
        device,
        address_bytes,
        sizeof(address_bytes),
        data,
        length,
        GT911_I2C_TIMEOUT_MS);
}

static esp_err_t gt911_write_byte(
    i2c_master_dev_handle_t device,
    uint16_t reg,
    uint8_t value)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t payload[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFFU),
        value,
    };

    return i2c_master_transmit(
        device,
        payload,
        sizeof(payload),
        GT911_I2C_TIMEOUT_MS);
}

static uint8_t gt911_calculate_config_checksum(
    const uint8_t *config,
    size_t length)
{
    uint8_t sum = 0U;

    for (size_t i = 0; i < length; ++i) {
        sum = (uint8_t)(sum + config[i]);
    }

    /* Goodix uses the two's complement of the configuration-byte sum. */
    return (uint8_t)(0U - sum);
}

static esp_err_t gt911_find_and_open(
    i2c_master_bus_handle_t bus,
    uint8_t *address_out,
    i2c_master_dev_handle_t *device_out)
{
    if (bus == NULL || address_out == NULL || device_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t addresses[] = {
        APP_PWR_GT911_PRIMARY_ADDRESS,
        APP_PWR_GT911_SECONDARY_ADDRESS,
    };

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        const uint8_t address = addresses[i];

        if (i2c_master_probe(bus, address, GT911_PROBE_TIMEOUT_MS) != ESP_OK) {
            continue;
        }

        const esp_err_t ret = gt911_open_device(bus, address, device_out);
        if (ret == ESP_OK) {
            *address_out = address;
            return ESP_OK;
        }

        return ret;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t gt911_configure_automatic_green_mode(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(
            TAG,
            "GT911 Green-mode configuration skipped: shared I2C bus unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t address = 0U;
    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = gt911_find_and_open(bus, &address, &device);

    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "GT911 Green-mode configuration skipped: no controller at "
            "0x%02X/0x%02X (%s)",
            APP_PWR_GT911_PRIMARY_ADDRESS,
            APP_PWR_GT911_SECONDARY_ADDRESS,
            esp_err_to_name(ret));
        return ret;
    }

    uint8_t config[GT911_CONFIG_LENGTH];
    uint8_t stored_checksum = 0U;

    ret = gt911_read_bytes(
        device,
        GT911_CONFIG_START_REGISTER,
        config,
        sizeof(config));

    if (ret == ESP_OK) {
        ret = gt911_read_bytes(
            device,
            GT911_CONFIG_CHECKSUM_REGISTER,
            &stored_checksum,
            1U);
    }

    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "GT911 configuration read failed at 0x%02X: %s",
            address,
            esp_err_to_name(ret));
        (void)i2c_master_bus_rm_device(device);
        return ret;
    }

    const uint8_t calculated_checksum =
        gt911_calculate_config_checksum(config, sizeof(config));

    if (calculated_checksum != stored_checksum) {
        ESP_LOGW(
            TAG,
            "GT911 configuration checksum invalid at 0x%02X: "
            "stored=0x%02X calculated=0x%02X; leaving configuration untouched",
            address,
            stored_checksum,
            calculated_checksum);

        (void)i2c_master_bus_rm_device(device);
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t old_value = config[GT911_LOW_POWER_INDEX];
    const uint8_t target_seconds =
        (uint8_t)(APP_PWR_GT911_GREEN_IDLE_SECONDS & 0x0FU);
    const uint8_t new_value =
        (uint8_t)((old_value & 0xF0U) | target_seconds);

    if (new_value != old_value) {
        config[GT911_LOW_POWER_INDEX] = new_value;
        const uint8_t new_checksum =
            gt911_calculate_config_checksum(config, sizeof(config));

        /*
         * This is deliberately NOT the GT911 0x8040 Sleep command.
         * Low_Power_Control selects the controller's automatic Green state,
         * which wakes itself on touch and therefore remains safe on the stock
         * board with no host-accessible GT911 INT/RESET pins.
         */
        ret = gt911_write_byte(
            device,
            GT911_LOW_POWER_CONTROL_REGISTER,
            new_value);

        if (ret == ESP_OK) {
            ret = gt911_write_byte(
                device,
                GT911_CONFIG_CHECKSUM_REGISTER,
                new_checksum);
        }

        if (ret == ESP_OK) {
            ret = gt911_write_byte(
                device,
                GT911_CONFIG_FRESH_REGISTER,
                0x01U);
        }

        if (ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "GT911 Green-mode update failed at 0x%02X: %s",
                address,
                esp_err_to_name(ret));
            (void)i2c_master_bus_rm_device(device);
            return ret;
        }

        /* Allow the controller to accept the refreshed configuration. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    uint8_t readback = 0xFFU;
    ret = gt911_read_bytes(
        device,
        GT911_LOW_POWER_CONTROL_REGISTER,
        &readback,
        1U);

    (void)i2c_master_bus_rm_device(device);

    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "GT911 Green-mode read-back failed at 0x%02X: %s",
            address,
            esp_err_to_name(ret));
        return ret;
    }

    if ((readback & 0x0FU) != target_seconds) {
        ESP_LOGW(
            TAG,
            "GT911 Green-mode verify failed at 0x%02X: "
            "requested_idle=%us readback=0x%02X",
            address,
            (unsigned)target_seconds,
            readback);
        return ESP_ERR_INVALID_STATE;
    }

    s_gt911_green_mode_verified = true;
    s_gt911_green_mode_address = address;

    ESP_LOGI(
        TAG,
        "GT911 automatic Green mode verified at 0x%02X: "
        "Low_Power_Control 0x%02X -> 0x%02X idle_to_green=%us "
        "(touch self-wake retained; full Sleep command remains disabled)",
        address,
        old_value,
        readback,
        (unsigned)target_seconds);

    return ESP_OK;
}

esp_err_t component_display_verify_deep_sleep_low_power(void)
{
    if (!s_gt911_green_mode_verified) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = gt911_open_device(
        bus,
        s_gt911_green_mode_address,
        &device);

    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t readback = 0xFFU;
    ret = gt911_read_bytes(
        device,
        GT911_LOW_POWER_CONTROL_REGISTER,
        &readback,
        1U);

    (void)i2c_master_bus_rm_device(device);

    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t target_seconds =
        (uint8_t)(APP_PWR_GT911_GREEN_IDLE_SECONDS & 0x0FU);

    if ((readback & 0x0FU) != target_seconds) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "GT911 Green-mode final audit OK: addr=0x%02X "
        "Low_Power_Control=0x%02X idle_to_green=%us",
        s_gt911_green_mode_address,
        readback,
        (unsigned)target_seconds);

    return ESP_OK;
}

#else

esp_err_t component_display_verify_deep_sleep_low_power(void)
{
    return ESP_OK;
}

#endif /* APP_PWR_GT911_GREEN_MODE_ENABLED */

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

#if APP_PWR_TOUCH_RESET_GPIO >= 0 || APP_PWR_TOUCH_INT_GPIO >= 0
/* Drive a pin as a plain push-pull output, releasing any Deep-sleep hold. */
static esp_err_t drive_pin(gpio_num_t gpio_num, int level)
{
    (void)gpio_hold_dis(gpio_num);

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        return ret;
    }

    return gpio_set_level(gpio_num, level);
}

/* Return a pin to a high-impedance input so the board's own bias decides it. */
static esp_err_t float_pin(gpio_num_t gpio_num)
{
    (void)gpio_hold_dis(gpio_num);

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&io_config);
}
#endif

esp_err_t component_display_wake_touch_after_reset(void)
{
#if APP_PWR_TOUCH_RESET_GPIO >= 0
    /*
     * RESET is the reliable wake: it restores the controller from sleep and
     * from any confused state.
     *
     * The GT911 latches its I2C address from the INT level while RESET is
     * released - INT low selects 0x5D, INT high selects 0x14. This board
     * answers at 0x5D, so INT is held LOW across the release when that pin is
     * also known, and only afterwards returned to a floating input. Getting
     * this backwards would move the controller to 0x14 and the BSP probe would
     * still fail.
     */
#if APP_PWR_TOUCH_INT_GPIO >= 0
    (void)drive_pin((gpio_num_t)APP_PWR_TOUCH_INT_GPIO, 0);
#endif

    esp_err_t ret = drive_pin(
        (gpio_num_t)APP_PWR_TOUCH_RESET_GPIO,
        APP_PWR_TOUCH_RESET_ACTIVE_LEVEL);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not assert GT911 RESET: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(APP_PWR_GT911_RESET_ASSERT_MS));

    ret = drive_pin(
        (gpio_num_t)APP_PWR_TOUCH_RESET_GPIO,
        !APP_PWR_TOUCH_RESET_ACTIVE_LEVEL);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not release GT911 RESET: %s", esp_err_to_name(ret));
        return ret;
    }

#if APP_PWR_TOUCH_INT_GPIO >= 0
    /* Hold the address selection valid, then hand INT back to the driver. */
    vTaskDelay(pdMS_TO_TICKS(APP_PWR_GT911_INT_PULSE_MS));
    (void)float_pin((gpio_num_t)APP_PWR_TOUCH_INT_GPIO);
#endif

    vTaskDelay(pdMS_TO_TICKS(APP_PWR_GT911_BOOT_MS));
    ESP_LOGI(TAG, "GT911 reset toggled; controller is awake for the BSP probe");
    return ESP_OK;

#elif APP_PWR_TOUCH_INT_GPIO >= 0
    /*
     * No RESET pin available. The documented alternative is an INT pulse held
     * HIGH for more than ~2 ms, after which the pin must be released so the
     * controller can drive it as its own interrupt output again.
     */
    esp_err_t ret = drive_pin((gpio_num_t)APP_PWR_TOUCH_INT_GPIO, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not pulse GT911 INT: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(APP_PWR_GT911_INT_PULSE_MS));
    (void)float_pin((gpio_num_t)APP_PWR_TOUCH_INT_GPIO);
    vTaskDelay(pdMS_TO_TICKS(APP_PWR_GT911_BOOT_MS));

    ESP_LOGI(TAG, "GT911 INT pulsed; controller is awake for the BSP probe");
    return ESP_OK;

#else
    /*
     * No touch pin is configured, so APP_PWR_GT911_SLEEP_ENABLED is refused at
     * build time and the controller is never put to sleep. Nothing to wake.
     */
    return ESP_OK;
#endif
}

#if APP_PWR_TOUCH_RESET_GPIO >= 0 || APP_PWR_TOUCH_INT_GPIO >= 0
/*
 * ESP-IDF runs global constructors from main_task, after the scheduler is
 * started and before app_main(). That is exactly the window this wake needs:
 * after vTaskDelay() is usable, before bsp_display_start() probes the GT911.
 *
 * Doing it here rather than in app_main() means enabling the touch sleep can
 * never be half-applied - there is no second edit to forget, and no ordering
 * for a future change to get wrong.
 */
static void __attribute__((constructor)) component_display_early_touch_wake(void)
{
    (void)component_display_wake_touch_after_reset();
}
#endif

esp_err_t component_display_disable_for_deep_sleep(void)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t ret = ESP_OK;

    /*
     * 1. Optional GPIO backlight hold.
     *
     * The stock ESP32-P4-NANO BSP intentionally defines BSP_LCD_BACKLIGHT as
     * GPIO_NUM_NC and controls brightness through the display-side I2C device
     * instead. Therefore no GPIO hold is required on the stock board; the BSP
     * display shutdown has already switched the backlight off before this
     * component-level cleanup runs.
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
    ESP_LOGI(TAG,
             "No dedicated backlight GPIO on ESP32-P4-NANO; "
             "BSP I2C backlight control already handled display-off");
#endif

    /*
     * 2. Put the GT911 into the lowest safe SOFTWARE-ONLY policy available on
     *    the stock board.
     *
     * Full GT911 Sleep cannot be used safely because INT/RESET are not exposed
     * to the P4. V18 therefore shortens Low_Power_Control to the configured
     * interval so the GT911 automatically enters Green mode when idle and
     * self-wakes on the next touch.
     */
#if APP_PWR_GT911_GREEN_MODE_ENABLED
    ret = gt911_configure_automatic_green_mode();
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Continuing Deep-sleep without GT911 Green-mode optimization: %s",
            esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }
#endif

    /*
     * Optional full GT911 Sleep remains available only for a future hardware
     * revision that exposes a verified INT or RESET wake pin.
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
