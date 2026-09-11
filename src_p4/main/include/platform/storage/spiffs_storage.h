#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mount the application's SPIFFS partition.
 *
 * Preserves the previous app_boot configuration exactly:
 * - base path: /spiffs
 * - partition label: storage
 * - max files: 4
 * - format if mount fails: enabled
 *
 * Information-query failure remains non-fatal after a successful mount.
 */
esp_err_t spiffs_storage_mount(void);

#ifdef __cplusplus
}
#endif
