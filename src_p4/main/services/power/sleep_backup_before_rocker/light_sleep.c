#include "services/power/sleep/light_sleep.h"

#include <inttypes.h>
#include <stdio.h>

#include "config/app_config.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/*
* ESP-IDF v5.5 ESP32-P4 documented Light-sleep policy used by this project:
*
* - Flash is NOT supply-power-gated because PSRAM is enabled and shares the
*   memory power domain.
* - Flash and PSRAM CS leakage workarounds are required.
* - PM support is required by the application's 180..360 MHz DFS policy.
*
* The installer writes these values into sdkconfig and sdkconfig.defaults.
* Keep compile-time guards here so a future configuration change cannot
* silently undo the low-power/safety assumptions.
*/
#ifndef CONFIG_PM_ENABLE
#error "V8 Light-sleep requires CONFIG_PM_ENABLE=y"
#endif

#ifndef CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND
#error "V8 Light-sleep requires CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y"
#endif

#if defined(CONFIG_SPIRAM) && !defined(CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND)
#error "V8 Light-sleep requires CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y with PSRAM"
#endif

#ifdef CONFIG_ESP_SLEEP_POWER_DOWN_FLASH
#error "Do not power down Flash supply in Light-sleep while this ESP32-P4 build uses PSRAM"
#endif

static const char *TAG = "light_sleep";

static esp_sleep_wakeup_cause_t s_last_wakeup_cause =
    ESP_SLEEP_WAKEUP_UNDEFINED;

static bool s_vddsdio_request_owned;

static esp_err_t configure_documented_light_sleep_domains(void)
{
    /*
    * Keep the shared flash/PSRAM supply domain ON. This follows the ESP-IDF
    * warning for applications using SPIRAM and avoids the unsafe Flash
    * supply-power-down path. The leakage workarounds above reduce CS leakage
    * while the rail remains powered.
    */
    if (s_vddsdio_request_owned) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = esp_sleep_pd_config(
        ESP_PD_DOMAIN_VDDSDIO,
        ESP_PD_OPTION_ON);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not keep VDD_SPI/PSRAM domain ON for Light-sleep: %s",
            esp_err_to_name(ret));
        return ret;
    }

    s_vddsdio_request_owned = true;

    /*
    * RTC_PERIPH is intentionally not touched here. ESP-IDF/component drivers
    * can own that reference-counted power-domain policy. Calling AUTO from
    * application code would reset their ownership state and can make a later
    * OFF request asymmetric. With no application override, ESP-IDF applies
    * the domain policy required by the active wake sources.
    */

    return ESP_OK;
}

static void restore_light_sleep_domain_defaults(void)
{
    /* Release exactly our ON reference. AUTO would erase other owners'
     * references in ESP-IDF 5.5.4's esp_sleep_pd_config(). */
    if (!s_vddsdio_request_owned) {
        return;
    }
    esp_err_t ret = esp_sleep_pd_config(
        ESP_PD_DOMAIN_VDDSDIO,
        ESP_PD_OPTION_OFF);
    if (ret == ESP_OK) {
        s_vddsdio_request_owned = false;
    } else {
        ESP_LOGE(TAG, "Could not release owned VDD_SPI sleep request: %s",
                 esp_err_to_name(ret));
    }
}


static esp_err_t disable_sleep_source_if_enabled(esp_sleep_source_t source)
{
    esp_err_t ret = esp_sleep_disable_wakeup_source(source);

    /* ESP_ERR_INVALID_STATE simply means that source was not enabled. */
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

static void log_wakeup_cause(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_GPIO:
        ESP_LOGI(
            TAG,
            "Light-sleep wake-up: GPIO%d",
            APP_LIGHT_SLEEP_WAKE_GPIO);
        break;

    case ESP_SLEEP_WAKEUP_TIMER:
        ESP_LOGI(TAG, "Light-sleep wake-up: timer");
        break;

    default:
        ESP_LOGI(TAG, "Light-sleep wake-up cause: %d", (int)cause);
        break;
    }
}

static esp_err_t enter_light_sleep_internal(
    uint32_t timeout_ms,
    bool enable_gpio_wakeup,
    bool verbose)
{
    const gpio_num_t wake_gpio = APP_LIGHT_SLEEP_WAKE_GPIO;

    if (timeout_ms == 0 && !enable_gpio_wakeup) {
        ESP_LOGE(TAG, "Light-sleep requires at least one wake-up source");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = configure_documented_light_sleep_domains();
    if (ret != ESP_OK) {
        return ret;
    }

    /*
    * Wake sources remain enabled after wake according to ESP-IDF. The normal
    * cleanup below removes the sources configured by this function; clearing
    * all sources here as well makes every 250 ms polling slice deterministic
    * even after an earlier rejected sleep request.
    */
    ret = disable_sleep_source_if_enabled(ESP_SLEEP_WAKEUP_ALL);
    if (ret != ESP_OK) {
        restore_light_sleep_domain_defaults();
        ESP_LOGE(
            TAG,
            "Could not clear stale sleep wake sources: %s",
            esp_err_to_name(ret));
        return ret;
    }

    if (enable_gpio_wakeup) {
        gpio_config_t wake_gpio_config = {
            .pin_bit_mask = 1ULL << wake_gpio,
            .mode = GPIO_MODE_INPUT,
            /* GPIO3 is held HIGH by the board's external pull-up. */
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        ret = gpio_config(&wake_gpio_config);
        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "GPIO%d configuration failed: %s",
                wake_gpio,
                esp_err_to_name(ret));
            restore_light_sleep_domain_defaults();
            return ret;
        }

        /*
        * GPIO wake-up is level triggered. Entering sleep while the active-low
        * button is already held would cause an immediate wake-up.
        */
        if (gpio_get_level(wake_gpio) == 0) {
            if (verbose) {
                ESP_LOGI(
                    TAG,
                    "GPIO%d is LOW; waiting for button release before Light-sleep",
                    wake_gpio);
            }

            while (gpio_get_level(wake_gpio) == 0) {
                vTaskDelay(pdMS_TO_TICKS(APP_LIGHT_SLEEP_BUTTON_POLL_MS));
            }

            vTaskDelay(pdMS_TO_TICKS(APP_LIGHT_SLEEP_BUTTON_DEBOUNCE_MS));
        }

        ret = gpio_wakeup_enable(wake_gpio, GPIO_INTR_LOW_LEVEL);
        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not enable GPIO%d Light-sleep wake-up: %s",
                wake_gpio,
                esp_err_to_name(ret));
            restore_light_sleep_domain_defaults();
            return ret;
        }

        ret = esp_sleep_enable_gpio_wakeup();
        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not enable GPIO Light-sleep wake source: %s",
                esp_err_to_name(ret));
            (void)gpio_wakeup_disable(wake_gpio);
            restore_light_sleep_domain_defaults();
            return ret;
        }
    }

    if (timeout_ms > 0) {
        const uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;

        ret = esp_sleep_enable_timer_wakeup(timeout_us);
        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not enable Light-sleep timer wake-up: %s",
                esp_err_to_name(ret));
            if (enable_gpio_wakeup) {
                (void)disable_sleep_source_if_enabled(ESP_SLEEP_WAKEUP_GPIO);
                (void)gpio_wakeup_disable(wake_gpio);
            }
            restore_light_sleep_domain_defaults();
            return ret;
        }

        if (verbose) {
            if (enable_gpio_wakeup) {
                ESP_LOGI(
                    TAG,
                    "Entering Light-sleep: GPIO%d LOW or timer after %" PRIu32 " ms",
                    wake_gpio,
                    timeout_ms);
            } else {
                ESP_LOGI(
                    TAG,
                    "Entering Light-sleep: timer after %" PRIu32 " ms",
                    timeout_ms);
            }
        }
    } else if (verbose) {
        ESP_LOGI(
            TAG,
            "Entering Light-sleep: GPIO%d LOW wake-up only",
            wake_gpio);
    }

    fflush(stdout);

    ret = esp_light_sleep_start();
    if (ret == ESP_OK) {
        s_last_wakeup_cause = esp_sleep_get_wakeup_cause();
        const uint32_t wake_causes = esp_sleep_get_wakeup_causes();

        if (verbose) {
            log_wakeup_cause(s_last_wakeup_cause);
            ESP_LOGI(
                TAG,
                "Light-sleep wake bitmap=0x%08" PRIx32,
                wake_causes);
        }
    } else {
        s_last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
        if (!verbose && ret == ESP_ERR_INVALID_ARG) {
            ESP_LOGW(
                TAG,
                "Timer-sliced Light-sleep entry was rejected: %s",
                esp_err_to_name(ret));
        } else {
            ESP_LOGE(
                TAG,
                "Light-sleep failed: %s",
                esp_err_to_name(ret));
        }
    }

    /*
    * Wake sources remain configured after wake-up in ESP-IDF. Remove the
    * Light-sleep sources here so they cannot accidentally carry over into
    * the existing Deep-sleep path.
    */
    esp_err_t cleanup_error = ESP_OK;

    esp_err_t cleanup_ret = ESP_OK;

    if (enable_gpio_wakeup) {
        cleanup_ret = gpio_wakeup_disable(wake_gpio);
        if (cleanup_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Could not disable GPIO%d wake-up: %s",
                wake_gpio,
                esp_err_to_name(cleanup_ret));
            cleanup_error = cleanup_ret;
        }

        cleanup_ret = disable_sleep_source_if_enabled(ESP_SLEEP_WAKEUP_GPIO);
        if (cleanup_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Could not clear GPIO sleep wake source: %s",
                esp_err_to_name(cleanup_ret));
            if (cleanup_error == ESP_OK) {
                cleanup_error = cleanup_ret;
            }
        }
    }

    if (timeout_ms > 0) {
        cleanup_ret = disable_sleep_source_if_enabled(ESP_SLEEP_WAKEUP_TIMER);
        if (cleanup_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Could not clear timer sleep wake source: %s",
                esp_err_to_name(cleanup_ret));
            if (cleanup_error == ESP_OK) {
                cleanup_error = cleanup_ret;
            }
        }
    }

    restore_light_sleep_domain_defaults();

    if (ret != ESP_OK) {
        return ret;
    }

    return cleanup_error;
}

esp_err_t enter_light_sleep(uint32_t timeout_ms, bool enable_gpio_wakeup)
{
    return enter_light_sleep_internal(
        timeout_ms,
        enable_gpio_wakeup,
        true);
}

esp_err_t enter_light_sleep_poll_slice(
    uint32_t timeout_ms,
    bool enable_gpio_wakeup)
{
    return enter_light_sleep_internal(
        timeout_ms,
        enable_gpio_wakeup,
        false);
}

esp_sleep_wakeup_cause_t light_sleep_get_last_wakeup_cause(void)
{
    return s_last_wakeup_cause;
}
