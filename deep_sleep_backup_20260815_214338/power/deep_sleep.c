#include "deep_sleep.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "deep_sleep";

#define WAKE_BUTTON_GPIO GPIO_NUM_3

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
#define SHARED_I2C_SDA_GPIO GPIO_NUM_7
#define SHARED_I2C_SCL_GPIO GPIO_NUM_8

static void record_first_error(
    esp_err_t operation_result,
    esp_err_t *first_error)
{
    if (operation_result != ESP_OK && *first_error == ESP_OK) {
        *first_error = operation_result;
    }
}

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
