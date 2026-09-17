
#include "platform/power/component_display.h"
#include "platform/display/display_platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "config/app_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
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

#if APP_PWR_GT911_SLEEP_ENABLED
static bool s_gt911_full_sleep_verified;
static uint8_t s_gt911_full_sleep_address;
#endif

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
static uint8_t s_gt911_green_idle_seconds;

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

/* Commit the existing panel configuration with one modified idle-time nibble.
 * Goodix guide 4.4 requires a new checksum and Config_Fresh. The full block
 * keeps all resolution, calibration and filtering bytes exactly as read. */
static esp_err_t gt911_commit_config(
    i2c_master_dev_handle_t device, const uint8_t *config)
{
    uint8_t payload[2U + GT911_CONFIG_LENGTH + 2U];
    payload[0] = (uint8_t)(GT911_CONFIG_START_REGISTER >> 8);
    payload[1] = (uint8_t)(GT911_CONFIG_START_REGISTER & 0xFFU);
    memcpy(&payload[2], config, GT911_CONFIG_LENGTH);
    payload[2U + GT911_CONFIG_LENGTH] =
        gt911_calculate_config_checksum(config, GT911_CONFIG_LENGTH);
    payload[3U + GT911_CONFIG_LENGTH] = 1U;
    return i2c_master_transmit(device, payload, sizeof(payload), GT911_I2C_TIMEOUT_MS);
}

static esp_err_t gt911_wait_config_applied(i2c_master_dev_handle_t device)
{
    for (unsigned attempt = 0; attempt < 10U; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
        uint8_t fresh = 1U;
        esp_err_t ret = gt911_read_bytes(device, GT911_CONFIG_FRESH_REGISTER, &fresh, 1U);
        if (ret != ESP_OK) {
            return ret;
        }
        if (fresh == 0U) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t gt911_check_config(
    i2c_master_dev_handle_t device, const uint8_t *expected)
{
    uint8_t readback[GT911_CONFIG_LENGTH + 1U];
    esp_err_t ret = gt911_read_bytes(device, GT911_CONFIG_START_REGISTER,
                                    readback, sizeof(readback));
    if (ret != ESP_OK) {
        return ret;
    }
    if (memcmp(readback, expected, GT911_CONFIG_LENGTH) != 0 ||
        readback[GT911_CONFIG_LENGTH] !=
            gt911_calculate_config_checksum(readback, GT911_CONFIG_LENGTH)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t gt911_configure_automatic_green_mode(void)
{
    s_gt911_green_mode_verified = false;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t address = 0U;
    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = gt911_find_and_open(bus, &address, &device);
    if (ret != ESP_OK) {
        return ret;
    }

    /* A driver named GT911 can also serve other Goodix parts. Never write a
     * GT911 configuration layout to an unverified GT9271/GT9xx controller. */
    uint8_t identity[6] = {0};
    ret = gt911_read_bytes(device, 0x8140, identity, sizeof(identity));
    const uint16_t firmware = (uint16_t)identity[4] | ((uint16_t)identity[5] << 8);
    if (ret == ESP_OK &&
        (identity[0] != '9' || identity[1] != '1' || identity[2] != '1' ||
         (identity[3] != 0 && identity[3] != ' ') || firmware < 0x1040U)) {
        ESP_LOGW(TAG, "Green tuning skipped: product=%.4s firmware=0x%04x; "
                 "GT911 firmware >= 0x1040 required", (const char *)identity, firmware);
        ret = ESP_ERR_NOT_SUPPORTED;
    }
    if (ret != ESP_OK) {
        (void)i2c_master_bus_rm_device(device);
        return ret;
    }

    ret = gt911_wait_config_applied(device);
    uint8_t original[GT911_CONFIG_LENGTH + 1U];
    if (ret == ESP_OK) {
        ret = gt911_read_bytes(device, GT911_CONFIG_START_REGISTER,
                               original, sizeof(original));
    }
    if (ret == ESP_OK && original[GT911_CONFIG_LENGTH] !=
            gt911_calculate_config_checksum(original, GT911_CONFIG_LENGTH)) {
        ESP_LOGW(TAG, "Green tuning skipped: existing GT911 checksum invalid");
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (ret != ESP_OK) {
        (void)i2c_master_bus_rm_device(device);
        return ret;
    }

    const uint8_t old_value = original[GT911_LOW_POWER_INDEX];
    const uint8_t old_seconds = old_value & 0x0FU;
    /* Do not lengthen an existing shorter idle timeout (0 is valid). */
    const uint8_t target_seconds = old_seconds < APP_PWR_GT911_GREEN_IDLE_SECONDS
        ? old_seconds : APP_PWR_GT911_GREEN_IDLE_SECONDS;
    uint8_t desired[GT911_CONFIG_LENGTH];
    memcpy(desired, original, sizeof(desired));
    desired[GT911_LOW_POWER_INDEX] = (old_value & 0xF0U) | target_seconds;
    if (desired[GT911_LOW_POWER_INDEX] != old_value) {
        ret = gt911_commit_config(device, desired);
        if (ret == ESP_OK) {
            ret = gt911_wait_config_applied(device);
        }
        if (ret == ESP_OK) {
            ret = gt911_check_config(device, desired);
        }
        if (ret != ESP_OK) {
            /* A partial I2C transfer may have changed configuration RAM. */
            const esp_err_t failure = ret;
            esp_err_t rollback = gt911_commit_config(device, original);
            if (rollback == ESP_OK) {
                rollback = gt911_wait_config_applied(device);
            }
            if (rollback == ESP_OK) {
                rollback = gt911_check_config(device, original);
            }
            ESP_LOGW(TAG, "Green tuning failed (%s); original config restore=%s",
                     esp_err_to_name(failure), esp_err_to_name(rollback));
            (void)i2c_master_bus_rm_device(device);
            return rollback == ESP_OK ? failure : rollback;
        }
    }
    const esp_err_t remove_ret = i2c_master_bus_rm_device(device);
    if (remove_ret != ESP_OK) {
        return remove_ret;
    }
    s_gt911_green_mode_verified = true;
    s_gt911_green_mode_address = address;
    s_gt911_green_idle_seconds = target_seconds;
    ESP_LOGI(TAG, "SLEEP-PWR: GT911 automatic Green idle=%us (previous=%us) "
             "config verified; touch self-wake retained; current not measured",
             (unsigned)target_seconds, (unsigned)old_seconds);
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

    const uint8_t target_seconds = s_gt911_green_idle_seconds;

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
#if APP_PWR_GT911_SLEEP_ENABLED
    if (!s_gt911_full_sleep_verified) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * A sleeping GT911 should not ACK I2C. Prove that the shared bus itself is
     * still healthy by probing the ES8311, which is on the same SDA/SCL pair.
     * This prevents a dead bus from being mistaken for successful GT911 Sleep.
     */
    if (i2c_master_probe(
            bus,
            APP_PWR_ES8311_I2C_ADDRESS,
            GT911_PROBE_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GT911 full-Sleep audit cannot prove bus health: ES8311 0x%02X "
            "did not ACK",
            APP_PWR_ES8311_I2C_ADDRESS);
        return ESP_ERR_INVALID_STATE;
    }

    if (i2c_master_probe(
            bus,
            s_gt911_full_sleep_address,
            GT911_PROBE_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGE(
            TAG,
            "GT911 full-Sleep final audit FAILED: controller at 0x%02X still "
            "ACKs I2C",
            s_gt911_full_sleep_address);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "GT911 FULL SLEEP final audit OK: addr=0x%02X no I2C ACK, "
        "shared bus healthy via ES8311",
        s_gt911_full_sleep_address);

    return ESP_OK;
#else
    return ESP_OK;
#endif
}

#endif /* APP_PWR_GT911_GREEN_MODE_ENABLED */

esp_err_t component_display_prepare_touch_for_sleep(void)
{
#if APP_PWR_GT911_GREEN_MODE_ENABLED
    return gt911_configure_automatic_green_mode();
#else
    return ESP_OK;
#endif
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

    s_gt911_full_sleep_verified = false;
    s_gt911_full_sleep_address = 0U;

    if (bus == NULL) {
        ESP_LOGW(TAG,
                 "Shared I2C bus is already released; cannot issue GT911 "
                 "full-Sleep command");
        return ESP_ERR_INVALID_STATE;
    }

#if APP_PWR_TOUCH_INT_GPIO < 0
    /*
     * The Goodix sequence asks the host to hold INT LOW before Command 0x05.
     * The stock ESP32-P4-NANO does not route INT to a P4 GPIO. We therefore
     * cannot force that prerequisite. V19 is intentionally experimental: send
     * the command and accept it only if the controller demonstrably stops
     * ACKing while another device proves the same I2C bus is still healthy.
     */
    ESP_LOGW(
        TAG,
        "GT911 FULL SLEEP measurement mode: host INT control is unavailable; "
        "sleep will be accepted only after no-ACK verification");
#endif

    static const uint8_t addresses[] = {
        APP_PWR_GT911_PRIMARY_ADDRESS,
        APP_PWR_GT911_SECONDARY_ADDRESS,
    };

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        const uint8_t address = addresses[i];

        if (i2c_master_probe(bus, address, GT911_PROBE_TIMEOUT_MS) != ESP_OK) {
            continue;
        }

        esp_err_t ret = gt911_write_sleep_command(bus, address);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GT911 at 0x%02X rejected full-Sleep command: %s",
                     address, esp_err_to_name(ret));
            return ret;
        }

        /*
         * Goodix requires >58 ms between issuing the Sleep command and a wake.
         * Waiting 70 ms also gives the device enough time to stop responding
         * before we verify the state.
         */
        vTaskDelay(pdMS_TO_TICKS(APP_PWR_GT911_SLEEP_VERIFY_DELAY_MS));

        /*
         * First prove the bus itself is alive. The ES8311 is still awake at
         * this point because audio suspend happens in the following stage.
         */
        if (i2c_master_probe(
                bus,
                APP_PWR_ES8311_I2C_ADDRESS,
                GT911_PROBE_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGE(
                TAG,
                "GT911 full-Sleep verification aborted: shared I2C bus health "
                "check failed at ES8311 0x%02X",
                APP_PWR_ES8311_I2C_ADDRESS);
            return ESP_ERR_INVALID_STATE;
        }

        if (i2c_master_probe(bus, address, GT911_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGE(
                TAG,
                "GT911 FULL SLEEP FAILED at 0x%02X: controller still ACKs "
                "after %u ms",
                address,
                (unsigned)APP_PWR_GT911_SLEEP_VERIFY_DELAY_MS);
            return ESP_ERR_INVALID_STATE;
        }

        s_gt911_full_sleep_verified = true;
        s_gt911_full_sleep_address = address;

        ESP_LOGI(
            TAG,
            "GT911 FULL SLEEP verified at 0x%02X: Command 0x05 accepted, "
            "controller no longer ACKs I2C after %u ms; capacitive scanning "
            "stopped",
            address,
            (unsigned)APP_PWR_GT911_SLEEP_VERIFY_DELAY_MS);

#if APP_PWR_TOUCH_INT_GPIO < 0 && APP_PWR_TOUCH_RESET_GPIO < 0
        ESP_LOGW(
            TAG,
            "GT911 FULL SLEEP is persistent across P4 Deep-sleep reset on "
            "stock wiring; GPIO3 can wake the P4, but a board power-cycle is "
            "required to restore touch");
#endif
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "No GT911 answered at 0x%02X or 0x%02X; full Sleep was not "
             "applied",
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
#if APP_PWR_GT911_SLEEP_ENABLED
    ESP_LOGW(
        TAG,
        "GT911 cannot be host-woken from FULL SLEEP on stock wiring: "
        "INT/RESET are not connected to ESP32-P4. Full board power-cycle "
        "required before touch initialization.");
    return ESP_ERR_NOT_SUPPORTED;
#else
    return ESP_OK;
#endif
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


esp_err_t component_display_panel_enter_full_sleep(void)
{
    /*
     * The display adapter owns the physical panel/DSI lifecycle. Delegating
     * here keeps the power component idempotent across both supported paths:
     *
     *   Deep-only      : live panel -> send DISPLAY_OFF + SLEEP_IN now.
     *   Light -> Deep  : SLEEP_IN was already sent before Light-sleep teardown.
     */
    return display_platform_panel_enter_full_sleep();
}

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
     * 2. GT911 low-power policy.
     *
     * Keep touch self-wake available on the stock board. Green mode changes
     * only the no-touch idle interval; it is not the destructive 0x05 command.
     * Successful register checks verify configuration, not physical current.
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

#if APP_PWR_GT911_SLEEP_ENABLED
    ret = gt911_enter_sleep();
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "GT911 did not reach verified FULL SLEEP: %s; continuing P4 "
            "Deep-sleep so the failure is visible in the final audit",
            esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
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

#if APP_PWR_GT911_SLEEP_ENABLED && \
    (APP_PWR_TOUCH_INT_GPIO < 0) && (APP_PWR_TOUCH_RESET_GPIO < 0)
    if (s_gt911_full_sleep_verified) {
        ESP_LOGE(
            TAG,
            "Deep sleep failed after GT911 entered FULL SLEEP; stock wiring "
            "has no software wake path. Power-cycle the board to restore touch.");
        if (first_error == ESP_OK) {
            first_error = ESP_ERR_NOT_SUPPORTED;
        }
    }
#endif

    if (first_error == ESP_OK) {
        ESP_LOGW(TAG, "Deep sleep failed; display side-channel holds released");
    }

    return first_error;
}
