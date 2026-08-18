#pragma once

#include "driver/gpio.h"

/* Camera and display buffers */
#define APP_CAMERA_BUFFER_COUNT             2
#define APP_DISPLAY_BUFFER_COUNT            2

/* Face-processing configuration */
#define APP_FACE_DETECT_INTERVAL_FRAMES             2U
#define APP_FACE_DETECT_IDLE_180_INTERVAL_FRAMES    8U
#define APP_FACE_DETECT_IDLE_90_INTERVAL_FRAMES     16U
#define APP_FACE_RECOG_INTERVAL_FRAMES              10U
#define APP_MAX_FACE_BOXES                          5
#define APP_FACE_BOX_HOLD_MISSES                    5
#define APP_FACE_RECOG_MIN_SCORE                    0.70f
#define APP_FACE_BOX_THICKNESS                      3
#define APP_FACE_LABEL_FONT_SCALE                   3

/*
 * Adaptive active-power policy.
 *
 * ACTIVE (0..5 seconds without a face):
 *   - camera and MIPI-DSI display are active;
 *   - CPU normally runs at the 180 MHz baseline;
 *   - a positive face detection requests the 360 MHz maximum;
 *   - the 360 MHz request is held through recognition and the PIN transition;
 *   - once the PIN screen is visible, the request is released and the CPU
 *     returns to the 180 MHz baseline;
 *   - detection runs every 2 camera frames.
 *
 * ECO-SCAN-8 (5..10 seconds without a face):
 *   - camera, ISP, LVGL and MIPI-DSI remain initialized;
 *   - the unlocked CPU baseline remains 180 MHz;
 *   - detection runs every 8 camera frames;
 *   - no shared camera/display hardware is deleted while CSI is active.
 *
 * ECO-SCAN-16 (10..15 seconds without a face):
 *   - camera, ISP and display remain initialized;
 *   - the unlocked CPU baseline remains 180 MHz;
 *   - detection runs every 16 camera frames;
 *   - the CPU is not reduced to 90 MHz while the CSI/ISP pipeline is active.
 *
 * A face detected during either ECO-SCAN stage requests 360 MHz and restores
 * the normal detection interval. At 15 seconds the existing coordinated
 * Light-sleep path stops the camera before suspending the display. At 30
 * seconds the system enters Deep-sleep.
 */
#define APP_CPU_MAX_FREQ_MHZ                        360
#define APP_CPU_ACTIVE_FREQ_MHZ                     180
#define APP_CPU_IDLE_180_AFTER_MS                   5000U
#define APP_CPU_IDLE_90_AFTER_MS                    10000U

/* Cache synchronization */
#define APP_SYNC_CACHE_AROUND_OVERLAY               1

/* Light-sleep configuration */
#define APP_LIGHT_SLEEP_WAKE_GPIO                   GPIO_NUM_3
#define APP_LIGHT_SLEEP_TIMEOUT_MS                  15000U
#define APP_LIGHT_SLEEP_TOUCH_POLL_MS               250U
#define APP_LIGHT_SLEEP_BUTTON_POLL_MS              5U
#define APP_LIGHT_SLEEP_BUTTON_DEBOUNCE_MS          25U

/* Deep-sleep configuration */
#define APP_DEEP_SLEEP_TIMEOUT_MS                   30000U
#define APP_DEEP_SLEEP_INACTIVITY_POLL_MS           100U
#define APP_DEEP_SLEEP_BUTTON_GPIO                  GPIO_NUM_3
#define APP_DEEP_SLEEP_BUTTON_POLL_MS               5U
#define APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS           25U
#define APP_DEEP_SLEEP_BUTTON_PRIORITY              8

/*
 * =====================================================================
 * Deep-sleep peripheral power control
 * =====================================================================
 *
 * Single source of truth for every board pin that the Deep-sleep shutdown
 * sequence drives. The individual power_save/component_*.c modules and the
 * pre-sleep rail audit in power/deep_sleep.c both read these definitions, so
 * a pin can never be held in one place and audited in another.
 *
 * Waveshare ESP32-P4-NANO board mapping:
 *
 *   GPIO54 -> ESP32-C6 CHIP_PU      LOW  = coprocessor held in reset
 *   GPIO51 -> IP101GRI PHY RESET    LOW  = PHY held in reset
 *   GPIO53 -> audio amplifier EN    LOW  = amplifier disabled
 *   GPIO45 -> Q1 (AO3401) gate      HIGH = SD1_VDD disconnected
 *   GPIO7  -> ESP_I2C_SDA           external 2.2K pull-up (R50)
 *   GPIO8  -> ESP_I2C_SCL           external 2.2K pull-up (R48)
 */
#define APP_PWR_WIFI_C6_CHIP_PU_GPIO                GPIO_NUM_54
#define APP_PWR_WIFI_C6_DISABLED_LEVEL              0
#define APP_PWR_WIFI_C6_ENABLED_LEVEL               1

#define APP_PWR_ETHERNET_PHY_RESET_GPIO             GPIO_NUM_51
#define APP_PWR_ETHERNET_RESET_ACTIVE_LEVEL         0
#define APP_PWR_ETHERNET_RESET_RELEASED_LEVEL       1

#define APP_PWR_AUDIO_AMP_GPIO                      GPIO_NUM_53
#define APP_PWR_AUDIO_AMP_DISABLED_LEVEL            0
#define APP_PWR_AUDIO_AMP_ENABLED_LEVEL             1

#define APP_PWR_SDCARD_POWER_GPIO                   GPIO_NUM_45
#define APP_PWR_SDCARD_POWER_ON_LEVEL               0
#define APP_PWR_SDCARD_POWER_OFF_LEVEL              1
#define APP_PWR_SDCARD_POWER_SETTLE_MS              20

#define APP_PWR_SHARED_I2C_SDA_GPIO                 GPIO_NUM_7
#define APP_PWR_SHARED_I2C_SCL_GPIO                 GPIO_NUM_8

/*
 * Read-back settle window.
 *
 * A pad does not necessarily reflect its new level on the very next
 * instruction after gpio_hold_en(). Measured on the ESP32-P4-NANO: GPIO54
 * (ESP32-C6 CHIP_PU) still reads HIGH immediately after the hold is applied,
 * and reads LOW a few tens of milliseconds later - the pre-sleep rail audit
 * confirms it. Verifying with zero delay therefore produced a false
 * "coprocessor is still powered" error on a pin that was in fact held
 * correctly.
 *
 * Each component now polls the pad until it matches, up to this timeout,
 * before deciding the hold failed.
 */
#define APP_PWR_HOLD_VERIFY_TIMEOUT_MS              150
#define APP_PWR_HOLD_VERIFY_POLL_MS                 5

/*
 * ---------------------------------------------------------------------
 * Display / touch Deep-sleep configuration
 * ---------------------------------------------------------------------
 *
 * The GT911 touch controller keeps actively scanning (roughly 5..10 mA)
 * unless it is explicitly told to sleep over I2C. The command must be sent
 * AFTER the BSP has released its own touch handle and BEFORE the shared I2C
 * pins are isolated in enter_deep_sleep().
 *
 * The backlight-enable and touch-INT pins are NOT documented in the exported
 * project, so they default to "unused". Fill them in from the Waveshare
 * schematic for your exact display, otherwise these pads float during
 * Deep-sleep and can leave the backlight driver or the GT911 partly active.
 *
 *   -1 disables the corresponding step.
 */
#define APP_PWR_GT911_SLEEP_ENABLED                 1
#define APP_PWR_GT911_PRIMARY_ADDRESS               0x5D
#define APP_PWR_GT911_SECONDARY_ADDRESS             0x14

/* Set to the real backlight-enable pin, e.g. GPIO_NUM_23. -1 = skip. */
#define APP_PWR_DISPLAY_BACKLIGHT_GPIO              (-1)
#define APP_PWR_DISPLAY_BACKLIGHT_OFF_LEVEL         0

/* Set to the real GT911 INT pin. -1 = skip. Driving it LOW keeps the */
/* controller from waking itself out of the sleep state.              */
#define APP_PWR_TOUCH_INT_GPIO                      (-1)

/* Set to the real GT911 RESET pin. -1 = skip. Holding reset asserted */
/* is even lower than the sleep command when the pin is available.    */
#define APP_PWR_TOUCH_RESET_GPIO                    (-1)
#define APP_PWR_TOUCH_RESET_ACTIVE_LEVEL            0

/*
 * ---------------------------------------------------------------------
 * Optional extra Deep-sleep measures
 * ---------------------------------------------------------------------
 */

/*
 * Ask ESP-IDF to power the SPI flash rail (VDD_SPI) down during Deep-sleep.
 *
 * On this board the request is REJECTED with ESP_ERR_INVALID_STATE, and that
 * is expected rather than a fault: the boot log shows
 *
 *     mmu_psram: .rodata xip on psram
 *     mmu_psram: .text   xip on psram
 *
 * so application code and constants are executed in place from the 32 MB PSRAM
 * that shares the VDD_SPI rail with the flash. IDF will not let that rail be
 * switched off. The rejection is handled and logged as information, not as a
 * warning, and Deep-sleep continues normally.
 *
 * Leave this enabled: it costs nothing, it is self-documenting in the log, and
 * it starts working automatically if XIP-from-PSRAM is ever disabled. The
 * saving would be a few tens of microamps - far below this board's floor.
 */
#define APP_PWR_DEEP_SLEEP_POWER_DOWN_FLASH         1

/*
 * Print the level of every controlled rail immediately before entering
 * Deep-sleep. Each line reports measured vs expected, so a released hold is
 * visible in the serial log instead of only on a multimeter.
 */
#define APP_PWR_DEEP_SLEEP_AUDIT_ENABLED            1

/*
 * After SD1_VDD is switched off, the SDMMC signals can still back-power the
 * card through its ESD structures if the ESP32-P4 keeps driving them or if
 * their pull-ups sit on the always-on 3V3 rail. Enable this and list the real
 * SDMMC pins for your board to float those pads at the Deep-sleep boundary.
 *
 * Leave disabled until the pin numbers have been confirmed in the schematic:
 * isolating a wrong pin can disturb an unrelated peripheral.
 */
#define APP_PWR_ISOLATE_SDMMC_PINS                  0
#define APP_PWR_SDMMC_PIN_LIST                      { 39, 40, 41, 42, 43, 44 }

/*
 * Per-stage delay used only while recording per-subsystem current plateaus
 * with a bench supply. Normal firmware must use 0; any non-zero value keeps
 * the board fully awake for seven extra stages on every Deep-sleep entry.
 */
#define APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS      0

#if APP_CPU_IDLE_180_AFTER_MS >= APP_CPU_IDLE_90_AFTER_MS
#error "The 180 MHz stage must begin before the 90 MHz stage"
#endif
#if APP_CPU_IDLE_90_AFTER_MS >= APP_LIGHT_SLEEP_TIMEOUT_MS
#error "The 90 MHz stage must begin before Light-sleep"
#endif
#if APP_CPU_ACTIVE_FREQ_MHZ > APP_CPU_MAX_FREQ_MHZ
#error "The active CPU baseline cannot exceed the maximum CPU frequency"
#endif
#if APP_LIGHT_SLEEP_TIMEOUT_MS >= APP_DEEP_SLEEP_TIMEOUT_MS
#error "Light-sleep must begin before Deep-sleep"
#endif

#if APP_FACE_DETECT_INTERVAL_FRAMES == 0 || \
    APP_FACE_DETECT_IDLE_180_INTERVAL_FRAMES == 0 || \
    APP_FACE_DETECT_IDLE_90_INTERVAL_FRAMES == 0
#error "Every face-detection interval must be greater than zero"
#endif
