
#include "services/power/component_runtime_policy.h"

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "platform/power/component_audio.h"
#include "platform/power/component_ethernet.h"
#include "platform/power/component_sdcard.h"
#include "platform/power/component_wifi.h"

static const char *TAG = "runtime_policy";

bool component_runtime_audio_policy_ready(void)
{
    return bsp_i2c_get_handle() != NULL;
}

esp_err_t component_runtime_set_audio_enabled(bool enabled)
{
    if (!component_runtime_audio_policy_ready()) {
        ESP_LOGI(TAG, "Audio policy deferred until the BSP I2C bus is ready");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = enabled
        ? component_audio_restore_after_failed_sleep()
        : component_audio_disable_for_deep_sleep();

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Runtime audio policy applied: ES8311/NS4150B=%s",
            enabled ? "ON" : "OFF");
    }

    return ret;
}

esp_err_t component_runtime_set_sdcard_enabled(bool enabled)
{
    if (!enabled) {
        const esp_err_t ret = component_sdcard_disable_for_deep_sleep();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Runtime storage policy applied: microSD=OFF");
        }
        return ret;
    }

    esp_err_t ret = component_sdcard_restore_after_failed_sleep();
    if (ret != ESP_OK) {
        return ret;
    }

    if (bsp_sdcard == NULL) {
        ret = bsp_sdcard_mount();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Could not mount enabled microSD: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "Runtime storage policy applied: microSD=ON and mounted");
    return ESP_OK;
}


esp_err_t component_runtime_set_wifi_enabled(bool enabled)
{
    return component_wifi_set_enabled(enabled);
}

esp_err_t component_runtime_set_ethernet_enabled(bool enabled)
{
    return component_ethernet_set_enabled(enabled);
}
