#include "light_sleep.h"

#include <inttypes.h>
#include <stdio.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "light_sleep";

static esp_sleep_wakeup_cause_t s_last_wakeup_cause =
    ESP_SLEEP_WAKEUP_UNDEFINED;

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

    esp_err_t ret = ESP_OK;

    if (enable_gpio_wakeup) {
        gpio_config_t wake_gpio_config = {
            .pin_bit_mask = 1ULL << wake_gpio,
            .mode = GPIO_MODE_INPUT,
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
            return ret;
        }

        ret = esp_sleep_enable_gpio_wakeup();
        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not enable GPIO Light-sleep wake source: %s",
                esp_err_to_name(ret));
            (void)gpio_wakeup_disable(wake_gpio);
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
        if (verbose) {
            log_wakeup_cause(s_last_wakeup_cause);
        }
    } else {
        s_last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
        ESP_LOGE(
            TAG,
            "Light-sleep failed: %s",
            esp_err_to_name(ret));
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
