#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool component_runtime_audio_policy_ready(void);
esp_err_t component_runtime_set_audio_enabled(bool enabled);
esp_err_t component_runtime_set_sdcard_enabled(bool enabled);
esp_err_t component_runtime_set_wifi_enabled(bool enabled);
esp_err_t component_runtime_set_ethernet_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
