#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ensure the BSP-owned microSD card is mounted.
 *
 * Preserves the existing boot behavior:
 * - if the card is already mounted, return ESP_OK;
 * - otherwise call the Waveshare BSP mount function once;
 * - return the BSP result unchanged.
 */
esp_err_t sd_card_storage_ensure_mounted(void);

#ifdef __cplusplus
}
#endif
