#pragma once

#include "driver/gpio.h"

/* Camera and display buffers */
#define APP_CAMERA_BUFFER_COUNT             2
#define APP_DISPLAY_BUFFER_COUNT            2

/* Face-processing configuration */
#define APP_FACE_DETECT_INTERVAL_FRAMES             2U
#define APP_FACE_DETECT_IDLE_180_INTERVAL_FRAMES    8U
#define APP_FACE_DETECT_IDLE_90_INTERVAL_FRAMES     16U
#define APP_FACE_RECOG_INTERVAL_FRAMES              2U
#define APP_MAX_FACE_BOXES                          5
#define APP_FACE_BOX_HOLD_MISSES                    5
#define APP_FACE_RECOG_MIN_SCORE                    0.70f
#define APP_FACE_BOX_THICKNESS                      3
#define APP_FACE_LABEL_FONT_SCALE                   3

/*
 * Asynchronous AI worker. The live preview stays on CPU0; CPU1 receives only
 * one latest-frame snapshot and never builds a backlog of stale frames.
 */
#define APP_AI_SNAPSHOT_MAX_EDGE                    320U
#define APP_AI_WORKER_CORE                          1
#define APP_AI_WORKER_STACK_SIZE                    (12 * 1024)
#define APP_AI_WORKER_PRIORITY                      5
#define APP_AI_WORKER_DRAIN_TIMEOUT_MS              18000U

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

/*
 * esp_light_sleep_start() may return a few hundred microseconds before the
 * requested timer deadline because of timer/clock quantization and software
 * overhead. Treat only a materially early return as abnormal. This prevents
 * normal ~249.4 ms returns for a 250 ms slice from being reported as a
 * Light-sleep failure.
 */
#define APP_LIGHT_SLEEP_EARLY_RETURN_TOLERANCE_US   5000LL

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
 * Waveshare's ESP32-P4-NANO BSP defines the LCD backlight GPIO, LCD reset,
 * GT911 reset and GT911 interrupt pins as GPIO_NUM_NC. The backlight is
 * controlled by the display-side I2C controller (address 0x45), not by a
 * dedicated ESP32-P4 GPIO. Therefore -1 below is intentional for this board.
 *
 * The GT911 can be commanded into sleep over I2C, but a sleeping GT911 needs
 * a hardware INT pulse or RESET toggle to wake reliably. Because the stock BSP
 * exposes neither pin, GT911 sleep must remain disabled unless the hardware is
 * modified or a specific display revision exposes a verified wake pin.
 */
#define APP_PWR_GT911_SLEEP_ENABLED                 0
#define APP_PWR_GT911_PRIMARY_ADDRESS               0x5D
#define APP_PWR_GT911_SECONDARY_ADDRESS             0x14

/* GT911 timings used only if a real wake pin is added/configured. */
#define APP_PWR_GT911_RESET_ASSERT_MS               20
#define APP_PWR_GT911_INT_PULSE_MS                  5
#define APP_PWR_GT911_BOOT_MS                       60

/* No dedicated backlight GPIO on the stock ESP32-P4-NANO BSP. */
#define APP_PWR_DISPLAY_BACKLIGHT_GPIO              (-1)
#define APP_PWR_DISPLAY_BACKLIGHT_OFF_LEVEL         0

/* Stock BSP: GT911 INT is GPIO_NUM_NC. Keep -1 unless hardware is verified. */
#define APP_PWR_TOUCH_INT_GPIO                      (-1)

/* Stock BSP: GT911 RESET is GPIO_NUM_NC. Keep -1 unless hardware is verified. */
#define APP_PWR_TOUCH_RESET_GPIO                    (-1)
#define APP_PWR_TOUCH_RESET_ACTIVE_LEVEL            0

/*
 * ---------------------------------------------------------------------
 * Optional extra Deep-sleep measures
 * ---------------------------------------------------------------------
 */

/*
 * Do not request VDD_SPI power-down in this build. The boot log confirms that
 * .text/.rodata execute in place from PSRAM, and PSRAM shares the VDD_SPI
 * domain with flash. Requesting ESP_PD_DOMAIN_VDDSDIO=OFF therefore produces
 * ESP-IDF's noisy "Domain is already in ESP_PD_OPTION_OFF" / invalid-state
 * diagnostic without providing a usable saving.
 *
 * Re-enable only if XIP-from-PSRAM is disabled and the rail can be switched
 * independently in a future build.
 */
#define APP_PWR_DEEP_SLEEP_POWER_DOWN_FLASH         0

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

/*
 * Refuse to build a firmware that puts the GT911 to sleep with no way to wake
 * it again. Without an INT or RESET pin the controller stays asleep across the
 * next reset and bsp_touch_new() aborts the application at boot.
 */
#if APP_PWR_GT911_SLEEP_ENABLED && \
    (APP_PWR_TOUCH_INT_GPIO < 0) && (APP_PWR_TOUCH_RESET_GPIO < 0)
#error "APP_PWR_GT911_SLEEP_ENABLED needs APP_PWR_TOUCH_INT_GPIO or APP_PWR_TOUCH_RESET_GPIO: the touch controller could not be woken and the next boot would abort in bsp_touch_new()"
#endif

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
