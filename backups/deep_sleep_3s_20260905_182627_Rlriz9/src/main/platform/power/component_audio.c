#include "diagnostics/sleep_power_profile.h"

#include "platform/power/component_audio.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "config/app_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_POWER_AMP_GPIO APP_PWR_AUDIO_AMP_GPIO
#define AUDIO_AMP_ENABLED    APP_PWR_AUDIO_AMP_ENABLED_LEVEL
#define AUDIO_AMP_DISABLED   APP_PWR_AUDIO_AMP_DISABLED_LEVEL

#define ES8311_I2C_ADDRESS   APP_PWR_ES8311_I2C_ADDRESS
#define ES8311_I2C_CLOCK_HZ  APP_PWR_ES8311_I2C_CLOCK_HZ
#define ES8311_TIMEOUT_MS    APP_PWR_ES8311_I2C_TIMEOUT_MS

/*
 * ES8311 register addresses used by Espressif's esp_codec_dev suspend path.
 *
 * The project already depends on esp_codec_dev 1.5.x through the Waveshare BSP.
 * We intentionally perform the small suspend transaction directly on the BSP
 * I2C bus because this application does not own an esp_codec_dev handle.
 */
#define ES8311_RESET_REG00         0x00
#define ES8311_CLK_MANAGER_REG01   0x01
#define ES8311_CLK_MANAGER_REG02   0x02
#define ES8311_SYSTEM_REG0D        0x0D
#define ES8311_SYSTEM_REG0E        0x0E
#define ES8311_SYSTEM_REG12        0x12
#define ES8311_SYSTEM_REG14        0x14
#define ES8311_ADC_REG15           0x15
#define ES8311_ADC_REG17           0x17
#define ES8311_DAC_REG32           0x32
#define ES8311_GP_REG45            0x45

static const char *TAG = "component_audio";

typedef struct {
    uint8_t reg;
    uint8_t value;
} es8311_reg_value_t;

typedef struct {
    uint8_t reg;
    uint8_t expected;
    uint8_t mask;
} es8311_verify_value_t;

/*
 * Espressif esp_codec_dev ES8311 suspend sequence (release/v2.x).
 *
 * Keep this order exact. In particular, REG02 is first written 0x10 while the
 * codec state machine is being stopped and finally written 0x00 after the
 * remaining power/clock blocks have been shut down.
 */
static const es8311_reg_value_t s_es8311_suspend_sequence[] = {
    {ES8311_DAC_REG32,         0x00},
    {ES8311_ADC_REG17,         0x00},
    {ES8311_SYSTEM_REG0E,      0xFF},
    {ES8311_SYSTEM_REG12,      0x02},
    {ES8311_SYSTEM_REG14,      0x00},
    {ES8311_SYSTEM_REG0D,      0xFA},
    {ES8311_ADC_REG15,         0x00},
    {ES8311_CLK_MANAGER_REG02, 0x10},
    {ES8311_RESET_REG00,       0x00},
    {ES8311_RESET_REG00,       0x1F},
    {ES8311_CLK_MANAGER_REG01, 0x30},
    {ES8311_CLK_MANAGER_REG01, 0x00},
    {ES8311_GP_REG45,          0x00},
    {ES8311_SYSTEM_REG0D,      0xFC},
    {ES8311_CLK_MANAGER_REG02, 0x00},
};

/*
 * Final readable register state produced by the suspend sequence.
 *
 * ES8311 SYSTEM_REG0E bit7 is reserved/not readable on this silicon.
 * Espressif's official suspend sequence intentionally writes 0xFF, while the
 * real device reads the implemented lower seven bits back as 0x7F.  Verify
 * only implemented bits instead of incorrectly treating that read-as-zero
 * reserved bit as a suspend failure.
 */
static const es8311_verify_value_t s_es8311_suspend_verify[] = {
    {ES8311_DAC_REG32,         0x00, 0xFF},
    {ES8311_ADC_REG17,         0x00, 0xFF},
    {ES8311_SYSTEM_REG0E,      0x7F, 0x7F},
    {ES8311_SYSTEM_REG12,      0x02, 0xFF},
    {ES8311_SYSTEM_REG14,      0x00, 0xFF},
    {ES8311_SYSTEM_REG0D,      0xFC, 0xFF},
    {ES8311_ADC_REG15,         0x00, 0xFF},
    {ES8311_CLK_MANAGER_REG02, 0x00, 0xFF},
    {ES8311_RESET_REG00,       0x1F, 0xFF},
    {ES8311_CLK_MANAGER_REG01, 0x00, 0xFF},
    {ES8311_GP_REG45,          0x00, 0xFF},
};

/*
 * Snapshot every unique register touched by the suspend sequence. If
 * Deep-sleep unexpectedly fails we can reconstruct the exact pre-suspend state
 * instead of assuming an audio sample rate, master/slave mode or gain.
 */
static const uint8_t s_es8311_snapshot_registers[] = {
    ES8311_DAC_REG32,
    ES8311_ADC_REG17,
    ES8311_SYSTEM_REG0E,
    ES8311_SYSTEM_REG12,
    ES8311_SYSTEM_REG14,
    ES8311_SYSTEM_REG0D,
    ES8311_ADC_REG15,
    ES8311_CLK_MANAGER_REG02,
    ES8311_RESET_REG00,
    ES8311_CLK_MANAGER_REG01,
    ES8311_GP_REG45,
};

typedef struct {
    bool valid;
    uint8_t values[sizeof(s_es8311_snapshot_registers)];
} es8311_snapshot_t;

static bool s_audio_amp_disabled;
static bool s_codec_powered_down;
static es8311_snapshot_t s_codec_snapshot;

/* A held pad may need a short settle window before its read-back is stable. */
static int wait_for_pad_level(
    gpio_num_t gpio_num,
    int expected_level,
    uint32_t *settle_ms_out)
{
    uint32_t waited_ms = 0;
    int level = gpio_get_level(gpio_num);

    while (level != expected_level &&
           waited_ms < APP_PWR_HOLD_VERIFY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(APP_PWR_HOLD_VERIFY_POLL_MS));
        waited_ms += APP_PWR_HOLD_VERIFY_POLL_MS;
        level = gpio_get_level(gpio_num);
    }

    if (settle_ms_out != NULL) {
        *settle_ms_out = waited_ms;
    }

    return level;
}

static esp_err_t audio_amp_disable(void)
{
    if (s_audio_amp_disabled) {
        return ESP_OK;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << AUDIO_POWER_AMP_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO%d configuration failed: %s",
            AUDIO_POWER_AMP_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_level(AUDIO_POWER_AMP_GPIO, AUDIO_AMP_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not disable NS4150B amplifier: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /*
     * Preserve the old, already-proven GPIO53 behavior. On this board the
     * physical level is also audited immediately before Deep-sleep.
     */
    ret = gpio_sleep_sel_dis(AUDIO_POWER_AMP_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable sleep switching for GPIO%d: %s",
            AUDIO_POWER_AMP_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_hold_en(AUDIO_POWER_AMP_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold GPIO%d LOW: %s",
            AUDIO_POWER_AMP_GPIO,
            esp_err_to_name(ret));
        return ret;
    }

    uint32_t settle_ms = 0;
    const int level_after_hold = wait_for_pad_level(
        AUDIO_POWER_AMP_GPIO,
        AUDIO_AMP_DISABLED,
        &settle_ms);

    if (level_after_hold != AUDIO_AMP_DISABLED) {
        ESP_LOGE(
            TAG,
            "NS4150B amplifier EN still reads %d after %" PRIu32
            " ms (expected %d)",
            level_after_hold,
            settle_ms,
            AUDIO_AMP_DISABLED);
        return ESP_ERR_INVALID_STATE;
    }

    s_audio_amp_disabled = true;

    ESP_LOGI(
        TAG,
        "NS4150B amplifier disabled: GPIO%d LOW "
        "(read-back=%d after %" PRIu32 " ms)",
        AUDIO_POWER_AMP_GPIO,
        level_after_hold,
        settle_ms);

    return ESP_OK;
}

static esp_err_t audio_amp_restore(void)
{
    if (!s_audio_amp_disabled) {
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;

    esp_err_t ret = gpio_hold_dis(AUDIO_POWER_AMP_GPIO);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(
            TAG,
            "Could not release GPIO%d hold: %s",
            AUDIO_POWER_AMP_GPIO,
            esp_err_to_name(ret));
        first_error = ret;
    }

    ret = gpio_set_level(AUDIO_POWER_AMP_GPIO, AUDIO_AMP_ENABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not restore NS4150B amplifier: %s",
                 esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    } else {
        s_audio_amp_disabled = false;
        ESP_LOGI(TAG, "NS4150B amplifier restored");
    }

    return first_error;
}

static esp_err_t es8311_open(i2c_master_dev_handle_t *device_out)
{
    if (device_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_out = NULL;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(
            TAG,
            "Shared BSP I2C bus is unavailable; cannot access ES8311");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_master_probe(
        bus,
        ES8311_I2C_ADDRESS,
        ES8311_TIMEOUT_MS);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "ES8311 not found at I2C address 0x%02X: %s",
            ES8311_I2C_ADDRESS,
            esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDRESS,
        .scl_speed_hz = ES8311_I2C_CLOCK_HZ,
    };

    ret = i2c_master_bus_add_device(
        bus,
        &device_config,
        device_out);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not attach temporary ES8311 I2C handle: %s",
            esp_err_to_name(ret));
    }

    return ret;
}

static void es8311_close(i2c_master_dev_handle_t device)
{
    if (device == NULL) {
        return;
    }

    const esp_err_t ret = i2c_master_bus_rm_device(device);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not remove temporary ES8311 I2C handle: %s",
            esp_err_to_name(ret));
    }
}

static esp_err_t es8311_read_reg(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t *value_out)
{
    if (device == NULL || value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(
        device,
        &reg,
        1,
        value_out,
        1,
        ES8311_TIMEOUT_MS);
}

static esp_err_t es8311_write_reg(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t value)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t payload[2] = {reg, value};

    return i2c_master_transmit(
        device,
        payload,
        sizeof(payload),
        ES8311_TIMEOUT_MS);
}

static esp_err_t es8311_take_snapshot(i2c_master_dev_handle_t device)
{
    if (s_codec_snapshot.valid) {
        return ESP_OK;
    }

    for (size_t i = 0; i < sizeof(s_es8311_snapshot_registers); ++i) {
        const esp_err_t ret = es8311_read_reg(
            device,
            s_es8311_snapshot_registers[i],
            &s_codec_snapshot.values[i]);

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not snapshot ES8311 reg 0x%02X: %s",
                s_es8311_snapshot_registers[i],
                esp_err_to_name(ret));
            return ret;
        }
    }

    s_codec_snapshot.valid = true;

    ESP_LOGI(
        TAG,
        "ES8311 pre-sleep register snapshot captured (%u registers)",
        (unsigned)sizeof(s_es8311_snapshot_registers));

    return ESP_OK;
}

static esp_err_t es8311_apply_suspend(
    i2c_master_dev_handle_t device)
{
    for (size_t i = 0;
         i < sizeof(s_es8311_suspend_sequence) /
                 sizeof(s_es8311_suspend_sequence[0]);
         ++i) {
        const es8311_reg_value_t *item = &s_es8311_suspend_sequence[i];

        const esp_err_t ret = es8311_write_reg(
            device,
            item->reg,
            item->value);

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "ES8311 suspend write failed: reg=0x%02X value=0x%02X: %s",
                item->reg,
                item->value,
                esp_err_to_name(ret));
            return ret;
        }
    }

    /*
     * Give the analog/reference and clock blocks a few milliseconds to settle
     * before read-back. This delay occurs only on the destructive Deep-sleep
     * path, never in Active mode.
     */
    vTaskDelay(pdMS_TO_TICKS(5));
    return ESP_OK;
}

static esp_err_t es8311_verify_suspend(
    i2c_master_dev_handle_t device)
{
    for (size_t i = 0;
         i < sizeof(s_es8311_suspend_verify) /
                 sizeof(s_es8311_suspend_verify[0]);
         ++i) {
        const es8311_verify_value_t *expected = &s_es8311_suspend_verify[i];
        uint8_t actual = 0;

        const esp_err_t ret = es8311_read_reg(
            device,
            expected->reg,
            &actual);

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "ES8311 verify read failed at reg 0x%02X: %s",
                expected->reg,
                esp_err_to_name(ret));
            return ret;
        }

        if ((actual & expected->mask) !=
            (expected->expected & expected->mask)) {
            ESP_LOGE(
                TAG,
                "ES8311 suspend mismatch: reg=0x%02X expected=0x%02X "
                "mask=0x%02X readback=0x%02X",
                expected->reg,
                expected->expected,
                expected->mask,
                actual);
            return ESP_ERR_INVALID_STATE;
        }

        if (expected->mask != 0xFFU) {
            ESP_LOGI(
                TAG,
                "ES8311 masked verify OK: reg=0x%02X readback=0x%02X "
                "mask=0x%02X",
                expected->reg,
                actual,
                expected->mask);
        }
    }

    ESP_LOGI(
        TAG,
        "ES8311 suspend verified: analog/DAC/ADC/clock blocks in "
        "Espressif power-down register state (REG0E reserved bit masked)");

    return ESP_OK;
}

static int snapshot_index_for_reg(uint8_t reg)
{
    for (size_t i = 0; i < sizeof(s_es8311_snapshot_registers); ++i) {
        if (s_es8311_snapshot_registers[i] == reg) {
            return (int)i;
        }
    }

    return -1;
}

static esp_err_t es8311_restore_snapshot(
    i2c_master_dev_handle_t device)
{
    if (!s_codec_snapshot.valid) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Leave the codec state machine in reset while restoring the other
     * registers. REG00 is restored last so the exact pre-sleep state takes
     * effect only after its clock/power/gain registers are back in place.
     */
    esp_err_t ret = es8311_write_reg(device, ES8311_RESET_REG00, 0x1F);
    if (ret != ESP_OK) {
        return ret;
    }

    for (size_t i = 0; i < sizeof(s_es8311_snapshot_registers); ++i) {
        const uint8_t reg = s_es8311_snapshot_registers[i];

        if (reg == ES8311_RESET_REG00) {
            continue;
        }

        ret = es8311_write_reg(
            device,
            reg,
            s_codec_snapshot.values[i]);

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "ES8311 restore failed: reg=0x%02X: %s",
                reg,
                esp_err_to_name(ret));
            return ret;
        }
    }

    const int reset_index = snapshot_index_for_reg(ES8311_RESET_REG00);
    if (reset_index < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = es8311_write_reg(
        device,
        ES8311_RESET_REG00,
        s_codec_snapshot.values[reset_index]);

    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    s_codec_powered_down = false;
    s_codec_snapshot.valid = false;

    ESP_LOGI(TAG, "ES8311 pre-sleep register state restored");
    return ESP_OK;
}

esp_err_t component_audio_disable_for_light_sleep(void)
{
    /*
     * Preserve the existing proven Light-sleep policy: only gate the external
     * speaker amplifier. Full ES8311 suspend is reserved for Deep-sleep.
     */
    return audio_amp_disable();
}

esp_err_t component_audio_restore_after_light_sleep(void)
{
    return audio_amp_restore();
}

esp_err_t component_audio_disable_for_deep_sleep(void)
{
    sleep_power_profile_before("NS4150B amplifier OFF");
    esp_err_t first_error = audio_amp_disable();
    sleep_power_profile_after("NS4150B amplifier OFF",
                              esp_err_to_name(first_error));

    sleep_power_profile_before("ES8311 codec suspend");
    if (s_codec_powered_down) {
        sleep_power_profile_after("ES8311 codec suspend", "ALREADY_SUSPENDED");
        return first_error;
    }

    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = es8311_open(&device);

    if (ret != ESP_OK) {
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        sleep_power_profile_after("ES8311 codec suspend", esp_err_to_name(ret));
        return first_error;
    }

    ret = es8311_take_snapshot(device);
    if (ret == ESP_OK) {
        ret = es8311_apply_suspend(device);
    }
    if (ret == ESP_OK) {
        ret = es8311_verify_suspend(device);
    }

    if (ret == ESP_OK) {
        s_codec_powered_down = true;
        ESP_LOGI(
            TAG,
            "Audio Deep-sleep state complete: NS4150B=OFF ES8311=SUSPEND");
    } else {
        ESP_LOGE(
            TAG,
            "ES8311 Deep-sleep power-down failed: %s",
            esp_err_to_name(ret));

        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    es8311_close(device);
    sleep_power_profile_after("ES8311 codec suspend", esp_err_to_name(ret));
    return first_error;
}

esp_err_t component_audio_verify_power_down(void)
{
    /*
     * If the first verification failed only because of a transient I2C read,
     * do not trust the software flag alone. A captured snapshot proves that
     * the suspend transaction was attempted, so re-read the actual hardware.
     */
    if (!s_codec_snapshot.valid) {
        ESP_LOGE(
            TAG,
            "ES8311 final audit unavailable: no pre-suspend snapshot exists");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t device = NULL;
    esp_err_t ret = es8311_open(&device);

    if (ret == ESP_OK) {
        ret = es8311_verify_suspend(device);
    }

    if (ret == ESP_OK) {
        s_codec_powered_down = true;
    }

    es8311_close(device);
    return ret;
}

esp_err_t component_audio_restore_after_failed_sleep(void)
{
    esp_err_t first_error = ESP_OK;

    if (s_codec_powered_down || s_codec_snapshot.valid) {
        i2c_master_dev_handle_t device = NULL;
        esp_err_t ret = es8311_open(&device);

        if (ret == ESP_OK && s_codec_snapshot.valid) {
            ret = es8311_restore_snapshot(device);
        }

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not restore ES8311 after failed Deep-sleep: %s",
                esp_err_to_name(ret));
            first_error = ret;
        }

        es8311_close(device);
    }

    const esp_err_t amp_ret = audio_amp_restore();
    if (amp_ret != ESP_OK && first_error == ESP_OK) {
        first_error = amp_ret;
    }

    return first_error;
}
