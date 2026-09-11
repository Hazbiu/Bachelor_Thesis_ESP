#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize the complete detector -> recognizer inference chain.
 *
 * This service owns only the mutex. It does not perform detection,
 * recognition, CPU-frequency changes, authentication, result publication,
 * worker scheduling or sleep coordination.
 */
esp_err_t vision_ai_inference_guard_init(void);

/*
 * Wait indefinitely for exclusive ownership, matching the previous
 * xSemaphoreTake(..., portMAX_DELAY) behavior.
 *
 * Returns false when the guard has not been initialized or the take fails.
 */
bool vision_ai_inference_guard_lock(void);

/* Release the inference guard after the current AI chain is complete. */
void vision_ai_inference_guard_unlock(void);

#ifdef __cplusplus
}
#endif
