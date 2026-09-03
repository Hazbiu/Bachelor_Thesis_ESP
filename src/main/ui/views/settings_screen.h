#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*settings_screen_back_callback_t)(void *user_data);

/**
 * Create Settings and perform a fresh /sdcard/enroll scan for authorized users.
 */
void settings_screen_create(
    settings_screen_back_callback_t back_callback,
    void *user_data);

/** Remove all Settings objects before returning to the launcher. */
void settings_screen_destroy(void);

#ifdef __cplusplus
}
#endif
