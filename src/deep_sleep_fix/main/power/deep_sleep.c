#include "deep_sleep.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/app_config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "soc/soc_caps.h"

static const char *TAG = "deep_sleep";

#define WAKE_BUTTON_GPIO APP_DEEP_SLEEP_BUTTON_GPIO

/*
 * ESP32-P4-NANO shared I2C bus from the Waveshare schematic:
 *
 *     GPIO7 = ESP_I2C_SDA
 *     GPIO8 = ESP_I2C_SCL
 *
 * R50 (SDA) and R48 (SCL) are external 2.2K pull-ups to ESP_3V3.
 * Software cannot disconnect those physical resistors. The lowest-current
 * software state is therefore to disable every internal pull resistor and
 * isolate the ESP32-P4 pads so that the external pull-ups can hold both lines
 * HIGH without a DC path through the SoC.
 *
 * Do this only at the final, irreversible Deep-sleep boundary. Light-sleep
 * must retain this bus because the application polls the GT911 touchscreen.
 */
#define SHARED_I2C_SDA_GPIO APP_PWR_SHARED_I2C_SDA_GPIO
#define SHARED_I2C_SCL_GPIO APP_PWR_SHARED_I2C_SCL_GPIO

/*
 * NOTE ON GPIO HOLDS AND ESP32-P4
 *
 * ESP32-P4 defines SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP = 1, meaning a
 * per-pin gpio_hold_en() keeps a digital pad frozen while the digital domain
 * is powered down. Because of that capability, ESP-IDF does NOT declare
 * gpio_deep_sleep_hold_en() for this target - the header guards it with
 * "#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP". Calling it here would not
 * compile, and it is not needed: the per-pin holds taken by the
 * power_save/component_*.c modules are already the correct mechanism.
 *
 * gpio_force_hold_all() does exist on ESP32-P4, but it also freezes GPIO3.
 * That is the Deep-sleep wake pin, so it is deliberately NOT used here.
 */

static void record_first_error(
    esp_err_t operation_result,
    esp_err_t *first_error)
{
    if (operation_result != ESP_OK && *first_error == ESP_OK) {
        *first_error = operation_result;
    }
}

#if APP_PWR_DEEP_SLEEP_AUDIT_ENABLED

static const char *AUDIT_TAG = "PWR_AUDIT";

typedef struct {
    int gpio_num;
    const char *description;
    int expected_level;
    const char *consequence_if_wrong;
} deep_sleep_rail_t;

static const deep_sleep_rail_t s_audited_rails[] = {
    {
        .gpio_num = APP_PWR_WIFI_C6_CHIP_PU_GPIO,
        .description = "ESP32-C6 CHIP_PU",
        .expected_level = APP_PWR_WIFI_C6_DISABLED_LEVEL,
        .consequence_if_wrong = "Wi-Fi coprocessor is running (tens of mA)",
    },
    {
        .gpio_num = APP_PWR_ETHERNET_PHY_RESET_GPIO,
        .description = "IP101GRI PHY RESET",
        .expected_level = APP_PWR_ETHERNET_RESET_ACTIVE_LEVEL,
        .consequence_if_wrong = "Ethernet PHY is auto-negotiating (20-40 mA)",
    },
    {
        .gpio_num = APP_PWR_AUDIO_AMP_GPIO,
        .description = "audio amplifier EN",
        .expected_level = APP_PWR_AUDIO_AMP_DISABLED_LEVEL,
        .consequence_if_wrong = "audio amplifier is enabled",
    },
    {
        .gpio_num = APP_PWR_SDCARD_POWER_GPIO,
        .description = "microSD Q1 gate",
        .expected_level = APP_PWR_SDCARD_POWER_OFF_LEVEL,
        .consequence_if_wrong = "SD1_VDD is still connected to the card",
    },
#if APP_PWR_DISPLAY_BACKLIGHT_GPIO >= 0
    {
        .gpio_num = APP_PWR_DISPLAY_BACKLIGHT_GPIO,
        .description = "display backlight EN",
        .expected_level = APP_PWR_DISPLAY_BACKLIGHT_OFF_LEVEL,
        .consequence_if_wrong = "backlight driver is partly on",
    },
#endif
#if APP_PWR_TOUCH_INT_GPIO >= 0
    {
        .gpio_num = APP_PWR_TOUCH_INT_GPIO,
        .description = "GT911 INT",
        .expected_level = 0,
        .consequence_if_wrong = "GT911 can wake itself out of sleep",
    },
#endif
    {
        .gpio_num = SHARED_I2C_SDA_GPIO,
        .description = "shared I2C SDA",
        .expected_level = 1,
        .consequence_if_wrong = "~1.5 mA through the 2.2K pull-up",
    },
    {
        .gpio_num = SHARED_I2C_SCL_GPIO,
        .description = "shared I2C SCL",
        .expected_level = 1,
        .consequence_if_wrong = "~1.5 mA through the 2.2K pull-up",
    },
};

/*
 * Report every controlled rail immediately before Deep-sleep is entered.
 *
 * A pad that was configured with GPIO_MODE_INPUT_OUTPUT reads back its real
 * electrical level even while it is held, so a hold that silently failed - or
 * an external device pulling a line - shows up in the serial log instead of
 * only on a multimeter.
 */
static void audit_rails_before_deep_sleep(void)
{
    unsigned mismatches = 0;

    ESP_LOGI(AUDIT_TAG, "---- Pre-Deep-sleep rail audit ----------------------");

    for (size_t i = 0; i < sizeof(s_audited_rails) / sizeof(s_audited_rails[0]); i++) {
        const deep_sleep_rail_t *rail = &s_audited_rails[i];
        const int level = gpio_get_level((gpio_num_t)rail->gpio_num);
        const bool matches = (level == rail->expected_level);

        if (matches) {
            ESP_LOGI(AUDIT_TAG, "GPIO%-2d %-22s level=%d expected=%d  OK",
                     rail->gpio_num,
                     rail->description,
                     level,
                     rail->expected_level);
        } else {
            mismatches++;
            ESP_LOGE(AUDIT_TAG,
                     "GPIO%-2d %-22s level=%d expected=%d  MISMATCH -> %s",
                     rail->gpio_num,
                     rail->description,
                     level,
                     rail->expected_level,
                     rail->consequence_if_wrong);
        }
    }

    if (mismatches == 0) {
        ESP_LOGI(AUDIT_TAG,
                 "All controlled rails are in their Deep-sleep state. "
                 "Remaining current belongs to always-on hardware "
                 "(camera module, panel, PHY, regulators, LED, USB bridge).");
    } else {
        ESP_LOGE(AUDIT_TAG,
                 "%u rail(s) are NOT in their Deep-sleep state; expect "
                 "elevated sleep current",
                 mismatches);
    }

    ESP_LOGI(AUDIT_TAG, "-----------------------------------------------------");
}

#else /* APP_PWR_DEEP_SLEEP_AUDIT_ENABLED */

static void audit_rails_before_deep_sleep(void)
{
}

#endif /* APP_PWR_DEEP_SLEEP_AUDIT_ENABLED */

static esp_err_t isolate_i2c_pin_for_deep_sleep(
    gpio_num_t gpio_num,
    const char *signal_name,
    int level_before_isolation)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t ret;

    if (level_before_isolation == 0) {
        /*
         * One externally pulled-up 2.2K line held LOW draws approximately
         * 3.3 V / 2200 ohm = 1.5 mA. Isolation removes an SoC-side LOW path,
         * but it cannot release a line that an external peripheral holds LOW.
         */
        ESP_LOGW(
            TAG,
            "%s GPIO%d is LOW before isolation; check whether an external "
            "I2C device is holding the bus LOW",
            signal_name,
            gpio_num);
    }

    ret = gpio_pullup_dis(gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not disable the internal pull-up on %s GPIO%d: %s",
            signal_name,
            gpio_num,
            esp_err_to_name(ret));
        record_first_error(ret, &first_error);
    }

    ret = gpio_pulldown_dis(gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not disable the internal pull-down on %s GPIO%d: %s",
            signal_name,
            gpio_num,
            esp_err_to_name(ret));
        record_first_error(ret, &first_error);
    }

    /*
     * GPIO7 and GPIO8 are ESP32-P4 LP/RTC-capable GPIOs. rtc_gpio_isolate()
     * disconnects the pad's digital input/output paths and internal pulls for
     * Deep-sleep. A Deep-sleep wake resets the application, so no runtime
     * restoration path is required here.
     */
    ret = rtc_gpio_isolate(gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not isolate %s GPIO%d for Deep-sleep: %s",
            signal_name,
            gpio_num,
            esp_err_to_name(ret));
        record_first_error(ret, &first_error);
    }

    return first_error;
}

static esp_err_t isolate_shared_i2c_for_deep_sleep(void)
{
    /* Sample the physical bus before disconnecting the ESP32-P4 pads. */
    const int sda_level = gpio_get_level(SHARED_I2C_SDA_GPIO);
    const int scl_level = gpio_get_level(SHARED_I2C_SCL_GPIO);
    esp_err_t first_error = ESP_OK;

    esp_err_t ret = isolate_i2c_pin_for_deep_sleep(
        SHARED_I2C_SDA_GPIO,
        "SDA",
        sda_level);
    record_first_error(ret, &first_error);

    ret = isolate_i2c_pin_for_deep_sleep(
        SHARED_I2C_SCL_GPIO,
        "SCL",
        scl_level);
    record_first_error(ret, &first_error);

    ESP_LOGI(
        TAG,
        "Shared I2C prepared for Deep-sleep: "
        "SDA=GPIO%d level_before=%d, SCL=GPIO%d level_before=%d, "
        "internal_pulls=OFF pads=ISOLATED",
        SHARED_I2C_SDA_GPIO,
        sda_level,
        SHARED_I2C_SCL_GPIO,
        scl_level);

    return first_error;
}

#if APP_PWR_ISOLATE_SDMMC_PINS
/*
 * Float a non-RTC digital pad and freeze it.
 *
 * rtc_gpio_isolate() only works on GPIO0..GPIO15 (the LP-capable range) on
 * ESP32-P4. The SDMMC signals are outside that range, so the equivalent action
 * is: configure as a pull-less input, stop ESP-IDF from switching the pad to
 * its sleep configuration, then hold it. This keeps the ESP32-P4 from
 * back-powering the microSD card through its ESD structures after SD1_VDD has
 * already been switched off.
 */
static esp_err_t float_and_hold_digital_pin(gpio_num_t gpio_num)
{
    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        return ret;
    }

    (void)gpio_sleep_sel_dis(gpio_num);
    return gpio_hold_en(gpio_num);
}

static void float_sdmmc_pins_for_deep_sleep(void)
{
    static const int sdmmc_pins[] = APP_PWR_SDMMC_PIN_LIST;

    for (size_t i = 0; i < sizeof(sdmmc_pins) / sizeof(sdmmc_pins[0]); i++) {
        const gpio_num_t gpio_num = (gpio_num_t)sdmmc_pins[i];
        const esp_err_t ret = float_and_hold_digital_pin(gpio_num);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Could not float SDMMC GPIO%d: %s",
                     (int)gpio_num, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG,
             "SDMMC pads floated so the powered-down microSD card cannot be "
             "back-powered through its signal lines");
}
#endif /* APP_PWR_ISOLATE_SDMMC_PINS */

#if APP_PWR_DEEP_SLEEP_POWER_DOWN_FLASH
static void power_down_flash_for_deep_sleep(void)
{
    /*
     * GPIO3 is the only Deep-sleep wake source; the RTC timer is never armed.
     * Under that condition ESP-IDF is allowed to remove power from the SPI
     * flash rail for the whole sleep interval.
     *
     * The capability macro guard keeps this portable: on a target without a
     * switchable VDD_SPI domain the enum value does not exist and the request
     * is simply skipped.
     */
#if defined(SOC_PM_SUPPORT_VDDSDIO_PD) && SOC_PM_SUPPORT_VDDSDIO_PD
    const esp_err_t ret = esp_sleep_pd_config(
        ESP_PD_DOMAIN_VDDSDIO,
        ESP_PD_OPTION_OFF);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPI flash rail (VDD_SPI) will be powered down");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        /*
         * Expected on this build. The boot log reports
         *   mmu_psram: .rodata xip on psram
         *   mmu_psram: .text   xip on psram
         * so code and constants execute in place from the PSRAM that shares
         * the VDD_SPI rail with the flash; IDF refuses to switch that rail
         * off. Nothing is wrong, and the saving would have been a few tens of
         * microamps - far below this board's floor.
         */
        ESP_LOGI(TAG,
                 "VDD_SPI power-down declined (XIP from PSRAM shares this "
                 "rail); continuing without it");
    } else {
        ESP_LOGW(TAG, "Could not request flash power-down: %s",
                 esp_err_to_name(ret));
    }
#else
    ESP_LOGI(TAG,
             "This target has no software-switchable VDD_SPI domain; "
             "flash power-down skipped");
#endif
}
#endif

void enter_deep_sleep(void)
{
    /*
     * Button wiring:
     *
     * ESP_3V3 ---- external 47K..100K pull-up ---- GPIO3
     * GPIO3  ---- button ------------------------- GND
     *
     * Released: GPIO3 = HIGH
     * Pressed:  GPIO3 = LOW
     *
     * Keep the internal pull-up enabled as a safe fallback. Disable it only
     * after the physical external GPIO3 pull-up has been installed and
     * verified with a meter.
     */
    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << WAKE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));

    /* Verify that GPIO3 is supported as a Deep-sleep wake pin. */
    if (!esp_sleep_is_valid_wakeup_gpio(WAKE_BUTTON_GPIO)) {
        ESP_LOGE(
            TAG,
            "GPIO%d cannot wake this ESP32-P4 from Deep-sleep",
            WAKE_BUTTON_GPIO);
        return;
    }

    /* Prevent immediate wake-up if the button is currently pressed. */
    if (gpio_get_level(WAKE_BUTTON_GPIO) == 0) {
        ESP_LOGW(
            TAG,
            "Button is currently pressed. Release it before Deep-sleep.");

        while (gpio_get_level(WAKE_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* Wake when GPIO3 becomes LOW. No timer wake-up is enabled. */
    ESP_ERROR_CHECK(
        esp_deep_sleep_enable_gpio_wakeup(
            1ULL << WAKE_BUTTON_GPIO,
            ESP_GPIO_WAKEUP_GPIO_LOW));

    /*
     * Report the state of every controlled rail while the pads can still be
     * read. This is the last point at which a released hold is observable in
     * software.
     */
    audit_rails_before_deep_sleep();

#if APP_PWR_ISOLATE_SDMMC_PINS
    float_sdmmc_pins_for_deep_sleep();
#endif

#if APP_PWR_DEEP_SLEEP_POWER_DOWN_FLASH
    power_down_flash_for_deep_sleep();
#endif

    /*
     * All camera/display teardown has completed before this function is
     * called. Isolate the shared I2C pins at this final boundary so the GT911
     * Light-sleep polling path is not affected.
     */
    esp_err_t i2c_ret = isolate_shared_i2c_for_deep_sleep();
    if (i2c_ret != ESP_OK) {
        /*
         * Continue into Deep-sleep even if one isolation operation failed.
         * Remaining active would consume far more current than sleeping, and
         * the ESP32-P4 powers down its digital I2C peripheral in Deep-sleep.
         */
        ESP_LOGW(
            TAG,
            "I2C pin isolation completed with errors: %s; continuing",
            esp_err_to_name(i2c_ret));
    }

    ESP_LOGI(
        TAG,
        "Entering Deep-sleep. Press the GPIO%d button to wake up.",
        WAKE_BUTTON_GPIO);

    /* Flush buffered output before entering Deep-sleep. */
    fflush(stdout);

    esp_deep_sleep_start();
}
