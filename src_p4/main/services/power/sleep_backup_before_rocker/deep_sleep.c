#include "services/power/sleep/deep_sleep.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/app_config.h"
#include "diagnostics/sleep_power_profile.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_rom_uart.h"
#include "platform/power/component_audio.h"
#include "platform/power/component_display.h"
#include "platform/power/component_ethernet.h"
#include "platform/power/component_wifi.h"
#include "soc/soc_caps.h"

#if APP_PWR_DEEP_SLEEP_PMU_HP_PAD_HOLD && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "DS-RETENTION-1 private PMU workaround reviewed for ESP-IDF 5.5.4 only; review SDK or disable APP_PWR_DEEP_SLEEP_PMU_HP_PAD_HOLD"
#endif
#include "hal/pmu_ll.h"
#include "soc/pmu_struct.h"
#endif

static const char *TAG = "deep_sleep";

/* Program only the PMU SLEEP-state policy. Flash/UART remain operational
 * in ACTIVE; esp_deep_sleep_start() performs its normal memory shutdown.
 * Reference: IDF v5.5.4 gpio_ll.h, pmu_param.c and pmu_sleep_digital_init().
 * The latter updates LP hold but does not overwrite HP pad hold.
 */
static esp_err_t arm_hp_pad_retention_for_deep_sleep(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "DS-RETENTION-1: IDF=%s chip_revision=%u.%u",
             esp_get_idf_version(), (unsigned)(chip.revision / 100),
             (unsigned)(chip.revision % 100));
#if APP_PWR_DEEP_SLEEP_PMU_HP_PAD_HOLD && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    if (chip.revision < 300) {
        const unsigned before =
            PMU.hp_sys[PMU_MODE_HP_SLEEP].syscntl.hp_pad_hold_all;
        pmu_ll_hp_set_hold_all_hp_pad(&PMU, PMU_MODE_HP_SLEEP, true);
        const unsigned after =
            PMU.hp_sys[PMU_MODE_HP_SLEEP].syscntl.hp_pad_hold_all;
        ESP_LOGI(TAG, "DS-RETENTION-1: PMU HP_SLEEP pad_hold_all %u -> %u; "
                 "policy armed, verify GPIO45/51/53/54 during actual sleep",
                 before, after);
        return after == 1U ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
#endif
    ESP_LOGI(TAG, "DS-RETENTION-1: PMU workaround skipped (disabled or rev >= 3)");
    return ESP_OK;
}

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
 * Per-pin HP hold alone is insufficient when TOP loses power on old P4
 * silicon. The separate PMU HP_SLEEP pad hold is armed at the final boundary.
 * No immediate gpio_force_hold_all() is used. LP wake GPIO3 remains under
 * ESP-IDF wake-source management. Successful register readback proves only
 * the pre-entry configuration, never the physical post-entry voltage.
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
        .gpio_num = APP_PWR_WIFI_C6_MODE_GPIO,
        .description = "C6 mode GPIO2",
        .expected_level = APP_PWR_WIFI_C6_MODE_DEEP_LEVEL,
        .consequence_if_wrong = "C6 can select retained 80 MHz mode instead of self-Deep-sleep",
    },
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
        .consequence_if_wrong = "PHY RESET differs from selected sleep policy",
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

    /* This may fall back to RESET LOW; obtain the expected level afterwards. */
    esp_err_t ethernet_ret = component_ethernet_verify_power_down();
    if (ethernet_ret == ESP_OK) {
        ESP_LOGI(AUDIT_TAG, "IP101GRI selected state=%s OK",
                 component_ethernet_deep_sleep_state());
    } else {
        mismatches++;
        ESP_LOGE(
            AUDIT_TAG,
            "IP101GRI selected-state verification FAILED (%s)",
            esp_err_to_name(ethernet_ret));
    }

    /*
    * GPIO53 proves only that the external NS4150B amplifier is disabled.
    * The ES8311 is a separate I2C codec, so verify the final Espressif
    * suspend-register state before the shared I2C pads are isolated.
    */
    esp_err_t codec_ret = component_audio_verify_power_down();
    if (codec_ret == ESP_OK) {
        ESP_LOGI(AUDIT_TAG, "ES8311 codec suspend registers          OK");
    } else {
        mismatches++;
        ESP_LOGE(
            AUDIT_TAG,
            "ES8311 codec suspend registers          FAILED (%s)",
            esp_err_to_name(codec_ret));
    }

#if APP_PWR_GT911_SLEEP_ENABLED || APP_PWR_GT911_GREEN_MODE_ENABLED
    esp_err_t gt911_ret = component_display_verify_deep_sleep_low_power();
    if (gt911_ret == ESP_OK) {
#if APP_PWR_GT911_SLEEP_ENABLED
        ESP_LOGI(AUDIT_TAG, "GT911 FULL SLEEP (no I2C ACK)             OK");
#else
        ESP_LOGI(AUDIT_TAG, "GT911 automatic Green/low-power mode      OK");
#endif
    } else {
        mismatches++;
#if APP_PWR_GT911_SLEEP_ENABLED
        ESP_LOGE(
            AUDIT_TAG,
            "GT911 FULL SLEEP                           FAILED (%s)",
            esp_err_to_name(gt911_ret));
#else
        ESP_LOGE(
            AUDIT_TAG,
            "GT911 automatic Green/low-power mode      FAILED (%s)",
            esp_err_to_name(gt911_ret));
#endif
    }
#endif

    for (size_t i = 0; i < sizeof(s_audited_rails) / sizeof(s_audited_rails[0]); i++) {
        const deep_sleep_rail_t *rail = &s_audited_rails[i];
        const int level = gpio_get_level((gpio_num_t)rail->gpio_num);
        const int expected_level = rail->gpio_num == APP_PWR_ETHERNET_PHY_RESET_GPIO
            ? component_ethernet_deep_sleep_reset_level() : rail->expected_level;
        const bool matches = (level == expected_level);

        if (matches) {
            ESP_LOGI(AUDIT_TAG, "GPIO%-2d %-22s level=%d expected=%d  OK",
                    rail->gpio_num,
                    rail->description,
                    level,
                    expected_level);
        } else {
            mismatches++;
            ESP_LOGE(AUDIT_TAG,
                    "GPIO%-2d %-22s level=%d expected=%d  MISMATCH -> %s",
                    rail->gpio_num,
                    rail->description,
                    level,
                    expected_level,
                    rail->consequence_if_wrong);
        }
    }

    if (mismatches == 0) {
        ESP_LOGI(
            AUDIT_TAG,
            "Pre-entry checks passed; post-entry rail levels are NOT measured here: "
            "C6 mode GPIO LOW + CHIP_PU LOW, selected IP101GRI policy, "
            "ES8311 suspend, NS4150B off, GT911 FULL SLEEP, microSD rail off "
            "and shared I2C high. Display shutdown requested "
            "LCD SLEEP_IN before the Light-sleep transport is destroyed.");
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

/*
* Release a digital peripheral signal into a high-impedance/no-pull state at
* the irreversible Deep-sleep boundary. This helper is shared by the SDMMC,
* audio, C6, Ethernet, CSI-sideband and final UART0 cleanup paths.
*
* rtc_gpio_isolate() is still used separately for the shared I2C RTC-capable
* pins. Do not use persistent HP-GPIO holds here.
*/
static esp_err_t float_digital_pin_for_deep_sleep(gpio_num_t gpio_num)
{
    /*
    * Release any stale application hold, then stop driving the pin and remove
    * internal pulls. Disable the input buffer too: floating input buffers
    * can draw current. Global PMU pad retention is armed separately after
    * all peripheral signals and control levels have their final settings.
    */
    esp_err_t ret = gpio_hold_dis(gpio_num);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        return ret;
    }

    /*
    * Keep this explicit high-impedance configuration at the sleep boundary.
    * The P4 Deep-sleep hardware then powers down/isolates the HP GPIO domain.
    */
    return gpio_sleep_sel_dis(gpio_num);
}

#if APP_PWR_ISOLATE_SDMMC_PINS
static esp_err_t float_sdmmc_pins_for_deep_sleep(void)
{
    static const int sdmmc_pins[] = APP_PWR_SDMMC_PIN_LIST;
    unsigned failed = 0;

    for (size_t i = 0; i < sizeof(sdmmc_pins) / sizeof(sdmmc_pins[0]); i++) {
        const gpio_num_t gpio_num = (gpio_num_t)sdmmc_pins[i];
        const esp_err_t ret = float_digital_pin_for_deep_sleep(gpio_num);

        if (ret != ESP_OK) {
            failed++;
            ESP_LOGW(
                TAG,
                "Could not float SDMMC GPIO%d: %s",
                (int)gpio_num,
                esp_err_to_name(ret));
        }
    }

    if (failed == 0) {
        ESP_LOGI(
            TAG,
            "SDMMC GPIO39..44 buffers DISABLED/no-pull after SD1_VDD off; "
            "card signal back-power path minimized");
    } else {
        ESP_LOGW(
            TAG,
            "%u SDMMC pin(s) could not be floated; continuing to Deep-sleep",
            failed);
    }
    return failed == 0 ? ESP_OK : ESP_FAIL;
}
#endif /* APP_PWR_ISOLATE_SDMMC_PINS */

#if APP_PWR_QUIESCE_PERIPHERAL_SIGNAL_PINS
/* Keep the configured pin membership; separate changes by physical subsystem
 * so one combined pin-cleanup operation cannot hide the source of a rise. */
static unsigned peripheral_signal_group(gpio_num_t pin)
{
    switch (pin) {
    case 9: case 10: case 11: case 12: case 13:
        return 0; /* ES8311 I2S */
    case 14: case 15: case 16: case 17: case 18: case 19:
    case 24: case 25:
        return 1; /* C6 SDIO / sideband; GPIO6 is a retained mode-control pin */
    case 20: case 21:
        return 2; /* C6 programming UART */
    case 28: case 29: case 30: case 34: case 35: case 49: case 50: case 52:
        return 3; /* Ethernet RMII / released MDIO */
    case 46: case 47:
        return 4; /* Camera CSI sideband */
    default:
        return 5; /* Any additional configured pins */
    }
}

static void quiesce_peripheral_signal_pins_for_deep_sleep(void)
{
    static const int signal_pins[] = APP_PWR_PERIPHERAL_SIGNAL_PIN_LIST;
    static const char *const group_names[] = {
        "Audio I2S GPIOs DISABLED/no-pull",
        "C6 SDIO / sideband GPIOs DISABLED/no-pull",
        "C6 programming UART GPIOs DISABLED/no-pull",
        "Ethernet RMII GPIOs DISABLED/no-pull",
        "Camera CSI sideband GPIOs DISABLED/no-pull",
        "Additional configured signal GPIOs DISABLED/no-pull",
    };
    unsigned total_failed = 0U;

    for (unsigned group = 0; group < sizeof(group_names) / sizeof(group_names[0]); ++group) {
        unsigned count = 0U;
        unsigned failed = 0U;
        for (size_t i = 0; i < sizeof(signal_pins) / sizeof(signal_pins[0]); ++i) {
            const gpio_num_t gpio_num = (gpio_num_t)signal_pins[i];
            if (peripheral_signal_group(gpio_num) != group) {
                continue;
            }
            if (count++ == 0U) {
                sleep_power_profile_before(group_names[group]);
            }

            /* Never release a wake input or a held board-control signal. */
            if (gpio_num == WAKE_BUTTON_GPIO ||
                gpio_num == APP_PWR_SDCARD_POWER_GPIO ||
                gpio_num == APP_PWR_AUDIO_AMP_GPIO ||
                gpio_num == APP_PWR_WIFI_C6_MODE_GPIO ||
                gpio_num == APP_PWR_WIFI_C6_CHIP_PU_GPIO ||
                gpio_num == APP_PWR_ETHERNET_PHY_RESET_GPIO ||
                gpio_num == APP_PWR_ETHERNET_MDC_GPIO) {
                failed++;
                ESP_LOGE(TAG, "Refusing to float protected control/wake GPIO%d",
                         (int)gpio_num);
                continue;
            }

            const esp_err_t ret = float_digital_pin_for_deep_sleep(gpio_num);
            if (ret != ESP_OK) {
                failed++;
                ESP_LOGW(TAG, "Could not release peripheral signal GPIO%d: %s",
                         (int)gpio_num, esp_err_to_name(ret));
            }
        }
        if (count != 0U) {
            sleep_power_profile_after(group_names[group],
                                      failed == 0U ? "ESP_OK" : "GPIO_ERRORS_SEE_LOG");
        }
        total_failed += failed;
    }

    ESP_LOGI(TAG, "Peripheral signal cleanup complete: failed_gpio_count=%u",
             total_failed);
}
#endif /* APP_PWR_QUIESCE_PERIPHERAL_SIGNAL_PINS */

#if APP_PWR_DEEP_SLEEP_FORCE_DOMAINS_OFF
typedef struct {
    esp_sleep_pd_domain_t domain;
    const char *name;
} deep_sleep_domain_request_t;

static esp_err_t configure_final_p4_power_domains(void)
{
    /*
    * ESP-IDF defaults unused domains to AUTO. Keep RTC_PERIPH out of this
    * application-owned OFF list: ESP-IDF/component drivers may manage that
    * domain with the reference-counted esp_sleep_pd_config() API. Issuing a
    * second OFF from here can underflow that ownership count and produces:
    *
    *   "Domain is already in ESP_PD_OPTION_OFF state"
    *
    * The application therefore leaves RTC_PERIPH with its existing owner.
    * Deep-sleep will still power it down automatically when the configured
    * GPIO3 wake source does not require it.
    */
    static const deep_sleep_domain_request_t off_domains[] = {
        { ESP_PD_DOMAIN_XTAL,       "XTAL" },
#if SOC_PM_SUPPORT_XTAL32K_PD
        { ESP_PD_DOMAIN_XTAL32K,    "XTAL32K" },
#endif
#if SOC_PM_SUPPORT_RC32K_PD
        { ESP_PD_DOMAIN_RC32K,      "RC32K" },
#endif
#if SOC_PM_SUPPORT_RC_FAST_PD
        { ESP_PD_DOMAIN_RC_FAST,    "RC_FAST" },
#endif
        /*
        * ESP-IDF v5.5.4 intentionally removes ESP_PD_DOMAIN_CPU from the
        * public enum when CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y. The user's
        * ESP32-P4 v1.3 build uses that compatibility path, so do not name the
        * unavailable enum there. Deep-sleep still shuts the HP/CPU domain down
        * as part of the SoC sleep transition.
        */
#if SOC_PM_SUPPORT_CPU_PD && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
        { ESP_PD_DOMAIN_CPU,        "CPU" },
#endif
#if SOC_PM_SUPPORT_TOP_PD
        { ESP_PD_DOMAIN_TOP,        "TOP" },
#endif
#if SOC_PM_SUPPORT_CNNT_PD
        { ESP_PD_DOMAIN_CNNT,       "CNNT/USB-HS" },
#endif
    };

    unsigned failed = 0U;
    esp_err_t ret = ESP_OK;

#if SOC_PM_SUPPORT_RTC_PERIPH_PD
    ESP_LOGI(
        TAG,
        "P4 Deep-sleep domain policy: RTC_PERIPH left to ESP-IDF/component ownership; "
        "no application OFF request");
#endif

    /*
    * Do not force the shared flash/PSRAM supply rail OFF here. The application
    * uses XIP from PSRAM; Deep-sleep itself handles flash safely. Returning
    * VDDSDIO to AUTO also clears any stale Light-sleep ON request.
    */
#if SOC_PM_SUPPORT_VDDSDIO_PD
    ret = esp_sleep_pd_config(
        ESP_PD_DOMAIN_VDDSDIO,
        ESP_PD_OPTION_AUTO);

    if (ret != ESP_OK) {
        failed++;
        ESP_LOGW(
            TAG,
            "Could not return VDDSDIO to AUTO: %s",
            esp_err_to_name(ret));
    }
#endif

    for (size_t i = 0; i < sizeof(off_domains) / sizeof(off_domains[0]); ++i) {
        ret = esp_sleep_pd_config(
            off_domains[i].domain,
            ESP_PD_OPTION_OFF);

        if (ret == ESP_OK) {
            ESP_LOGI(
                TAG,
                "P4 Deep-sleep domain request: %-12s -> OFF",
                off_domains[i].name);
        } else {
            failed++;
            ESP_LOGW(
                TAG,
                "P4 Deep-sleep domain request failed: %s -> OFF: %s",
                off_domains[i].name,
                esp_err_to_name(ret));
        }
    }

#if SOC_PM_SUPPORT_CNNT_PD
    ESP_LOGI(
        TAG,
        "USB Type-A software policy: external VBUS switch has no P4 GPIO "
        "control; internal high-speed connectivity domain CNNT requested OFF");
#else
    ESP_LOGI(
        TAG,
        "USB Type-A software policy: external VBUS switch has no P4 GPIO "
        "control; target exposes no switchable CNNT power domain");
#endif

    if (failed == 0U) {
        ESP_LOGI(
            TAG,
            "P4 power-domain audit: all safe software-only OFF requests accepted");
    } else {
        ESP_LOGW(
            TAG,
            "P4 power-domain audit: %u request(s) were not accepted; "
            "ESP-IDF will still apply its Deep-sleep AUTO policy",
            failed);
    }
    return failed == 0U ? ESP_OK : ESP_FAIL;
}
#endif /* APP_PWR_DEEP_SLEEP_FORCE_DOMAINS_OFF */

#if APP_PWR_FLOAT_UART0_AT_FINAL_BOUNDARY
/*
* After these pins are detached from UART0 no additional logging is allowed.
* The diagnostic build uses one silent three-second measurement interval here
* before esp_deep_sleep_start(); normal firmware (delay=0) enters immediately.
*/
static void float_uart0_at_final_boundary(void)
{
    static const int uart0_pins[] = APP_PWR_UART0_PIN_LIST;

    for (size_t i = 0; i < sizeof(uart0_pins) / sizeof(uart0_pins[0]); ++i) {
        (void)float_digital_pin_for_deep_sleep(
            (gpio_num_t)uart0_pins[i]);
    }
}
#endif /* APP_PWR_FLOAT_UART0_AT_FINAL_BOUNDARY */

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

void enter_deep_sleep_with_profile(deep_sleep_profile_t profile)
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

    sleep_power_profile_before("GPIO3 wake source / stale timers cleared");
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

    /*
    * Clear every wake source that may have been left by an earlier
    * Light-sleep cycle, then arm only GPIO3 for this Deep-sleep entry.
    */
    esp_err_t wake_clear_ret =
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    if (wake_clear_ret != ESP_OK && wake_clear_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(
            TAG,
            "Could not clear all stale wake sources: %s",
            esp_err_to_name(wake_clear_ret));
    } else {
        ESP_LOGI(TAG, "Deep-sleep wake audit: all stale wake sources cleared");
    }

    /* Wake when GPIO3 becomes LOW. No timer/USB/UART wake-up is enabled. */
    ESP_ERROR_CHECK(
        esp_deep_sleep_enable_gpio_wakeup(
            1ULL << WAKE_BUTTON_GPIO,
            ESP_GPIO_WAKEUP_GPIO_LOW));
    sleep_power_profile_after(
        "GPIO3 wake source / stale timers cleared",
        (wake_clear_ret == ESP_OK || wake_clear_ret == ESP_ERR_INVALID_STATE)
            ? "ESP_OK" : "WAKE_CLEAR_ERROR_SEE_LOG");

    ESP_LOGI(
        TAG,
        "Deep-sleep boundary profile=%s",
        profile == DEEP_SLEEP_PROFILE_LIGHT_COMPATIBLE
            ? "LIGHT_COMPATIBLE"
            : "AGGRESSIVE");

#if APP_PWR_DEEP_SLEEP_FORCE_DOMAINS_OFF
    if (profile == DEEP_SLEEP_PROFILE_AGGRESSIVE) {
        sleep_power_profile_before("P4 power-domain POLICY (applies at entry)");
        const esp_err_t domain_ret = configure_final_p4_power_domains();
        sleep_power_profile_after("P4 power-domain POLICY (applies at entry)",
                                  esp_err_to_name(domain_ret));
    } else {
        ESP_LOGI(
            TAG,
            "Light-compatible Deep-sleep: manual P4 power-domain OFF "
            "overrides skipped; ESP-IDF Deep-sleep policy retained");
    }
#endif

    if (profile == DEEP_SLEEP_PROFILE_AGGRESSIVE) {
        /*
         * Existing full/destructive Deep-only boundary remains available.
         */
        sleep_power_profile_before("Pre-isolation rail / codec / touch audit");
        audit_rails_before_deep_sleep();
        sleep_power_profile_after("Pre-isolation rail / codec / touch audit",
                                  "SEE_PWR_AUDIT_LOG");

#if APP_PWR_ISOLATE_SDMMC_PINS
        sleep_power_profile_before("SDMMC GPIO39..44 DISABLED/no-pull");
        const esp_err_t sd_pins_ret = float_sdmmc_pins_for_deep_sleep();
        sleep_power_profile_after("SDMMC GPIO39..44 DISABLED/no-pull",
                                  esp_err_to_name(sd_pins_ret));
#endif

#if APP_PWR_DEEP_SLEEP_POWER_DOWN_FLASH
        sleep_power_profile_before("Optional flash power-down POLICY");
        power_down_flash_for_deep_sleep();
        sleep_power_profile_after("Optional flash power-down POLICY", "SEE_LOG");
#endif

        sleep_power_profile_before("Shared I2C SDA/SCL GPIO7/8 isolation");
        esp_err_t i2c_ret =
            isolate_shared_i2c_for_deep_sleep();
        sleep_power_profile_after("Shared I2C SDA/SCL GPIO7/8 isolation",
                                  esp_err_to_name(i2c_ret));

        if (i2c_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "I2C pin isolation completed with errors: %s; continuing",
                esp_err_to_name(i2c_ret));
        }

#if APP_PWR_QUIESCE_PERIPHERAL_SIGNAL_PINS
        quiesce_peripheral_signal_pins_for_deep_sleep();
#endif
    } else {
        /*
         * Hybrid Light->Deep:
         * preserve the already-low-current external Light-sleep state.
         */
        ESP_LOGI(
            TAG,
            "Light-compatible Deep-sleep boundary: "
            "external Light-sleep peripheral state preserved");

        ESP_LOGI(
            TAG,
            "Skipping Deep-only teardown: microSD rail cut, GT911 full sleep, "
            "ES8311 suspend, shared-I2C isolation, SDMMC/peripheral GPIO "
            "floating and UART detach");

        ESP_LOGI(
            "PWR_AUDIT",
            "LIGHT-COMPAT: GPIO54(C6)=%d GPIO51(ETH_RESET)=%d "
            "GPIO53(AMP_EN)=%d SDA=%d SCL=%d "
            "microSD=PRESERVED",
            gpio_get_level(APP_PWR_WIFI_C6_CHIP_PU_GPIO),
            gpio_get_level(APP_PWR_ETHERNET_PHY_RESET_GPIO),
            gpio_get_level(APP_PWR_AUDIO_AMP_GPIO),
            gpio_get_level(SHARED_I2C_SDA_GPIO),
            gpio_get_level(SHARED_I2C_SCL_GPIO));
    }

    /*
    * FINAL C6 CLAMP
    *
    * STEP 6 has already disabled the ESP32-C6. Re-apply GPIO54 LOW again now,
    * after every other application-side teardown operation, so there is no
    * stale-state shortcut. Diagnostic builds hold this state for measurement
    * and also pause silently after UART detach. The final pad readback below
    * occurs after the C6 measurement window. Delay=0 has no added pauses.
    */
    sleep_power_profile_before("Final ESP32-C6 GPIO54 LOW clamp");
    esp_err_t c6_final_ret = component_wifi_force_off_at_sleep_boundary();
    sleep_power_profile_after("Final ESP32-C6 GPIO54 LOW clamp",
                              esp_err_to_name(c6_final_ret));
    if (c6_final_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Final ESP32-C6 CHIP_PU clamp failed: %s; entering Deep-sleep "
            "anyway because remaining ACTIVE would consume more power",
            esp_err_to_name(c6_final_ret));
    }

    /*
    * Dump the real GPIO54 configuration at the last observable point.
    * ESP-IDF v5.5 reports Pullup/Pulldown, InputEn, OutputEn, DriveCap,
    * FuncSel and SleepSelEn here.
    */
    esp_err_t dump_ret = gpio_dump_io_configuration(
        stdout,
        1ULL << APP_PWR_WIFI_C6_CHIP_PU_GPIO);
    if (dump_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not dump final GPIO54 configuration: %s",
            esp_err_to_name(dump_ret));
    }

    const int c6_final_level =
        gpio_get_level(APP_PWR_WIFI_C6_CHIP_PU_GPIO);

    ESP_LOGW(
        TAG,
        "C6-FINAL-AUDIT: GPIO%d CHIP_PU level=%d expected=%d "
        "mode_gpio%d=%d expected_mode=%d "
        "scope=PRE_ENTRY_ONLY",
        APP_PWR_WIFI_C6_CHIP_PU_GPIO,
        c6_final_level,
        APP_PWR_WIFI_C6_DISABLED_LEVEL,
        APP_PWR_WIFI_C6_MODE_GPIO,
        gpio_get_level(APP_PWR_WIFI_C6_MODE_GPIO),
        APP_PWR_WIFI_C6_MODE_DEEP_LEVEL);

    const esp_err_t retention_ret = arm_hp_pad_retention_for_deep_sleep();
    if (retention_ret != ESP_OK) {
        ESP_LOGE(TAG, "DS-RETENTION-1: PMU hold could not be armed: %s",
                 esp_err_to_name(retention_ret));
    }

    ESP_LOGI(TAG, "PWR-OPT-3: final PHY policy=%s RESET_expected=%d "
             "signal_buffers=DISABLED retention=DS-RETENTION-1",
             component_ethernet_deep_sleep_state(),
             component_ethernet_deep_sleep_reset_level());

    ESP_LOGI(
        TAG,
        "Entering Deep-sleep. Press the GPIO%d button to wake up.",
        WAKE_BUTTON_GPIO);

    const bool detach_uart = APP_PWR_FLOAT_UART0_AT_FINAL_BOUNDARY &&
        profile == DEEP_SLEEP_PROFILE_AGGRESSIVE;
    sleep_power_profile_final_boundary(detach_uart);

    /* Flush the final announcement before UART detach. The only diagnostic
     * work afterwards is a silent FreeRTOS delay; no application log is issued.
     * The P4 then really enters Deep-sleep, so no post-entry pause is possible. */
    fflush(stdout);
    fflush(stderr);
    /* fflush() drains the C stream, not the hardware shift register.
     * IDF also flushes UARTs inside esp_deep_sleep_start(), but that is too
     * late once the TX pin has already been disconnected below. */
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART
    if (uart_ll_is_enabled(CONFIG_ESP_CONSOLE_UART_NUM)) {
        esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);
    }
#endif

#if APP_PWR_FLOAT_UART0_AT_FINAL_BOUNDARY
    if (profile == DEEP_SLEEP_PROFILE_AGGRESSIVE) {
        float_uart0_at_final_boundary();
    }
#endif

    sleep_power_profile_quiet_wait();
    esp_deep_sleep_start();
}


void enter_deep_sleep(void)
{
    enter_deep_sleep_with_profile(
        DEEP_SLEEP_PROFILE_AGGRESSIVE);
}
