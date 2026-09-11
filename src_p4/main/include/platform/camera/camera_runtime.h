#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the existing camera/video layer using the board-owned BSP I2C bus.
 *
 * This is an adapter only: it intentionally adds no retry, timing, validation,
 * power policy or camera behavior. app_video_main() retains its existing
 * initialization logic and error handling.
 */
esp_err_t camera_runtime_initialize(void);

#ifdef __cplusplus
}
#endif
