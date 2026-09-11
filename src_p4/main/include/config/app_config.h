
#pragma once

#include "driver/gpio.h"
#include "config/sleep_mode_selection.h"

/* Camera and display buffers */
#define APP_CAMERA_BUFFER_COUNT             2
#define APP_DISPLAY_BUFFER_COUNT            2

/*
 * Application task affinity on the ESP32-P4 high-performance CPU pair.
 *
 * CPU0 and CPU1 are equivalent HP RISC-V cores; this split is about workload
 * isolation, not different core capabilities. Keep application-side camera,
 * display, UI, sleep-control and diagnostic tasks on CPU0, and reserve CPU1
 * for the asynchronous detector -> recognizer worker. ESP-IDF/library tasks
 * still keep whatever affinity their own components configure.
 *
 * The CPU-frequency PM policy is shared by the HP subsystem. A 360 MHz PM
 * lock raised by the AI worker therefore changes the HP CPU clock policy; it
 * does not create independent 180 MHz/360 MHz clocks for CPU0 and CPU1.
 */
#define APP_SYSTEM_WORKER_CORE                      0
#define APP_AI_WORKER_CORE                          1

#if APP_SYSTEM_WORKER_CORE == APP_AI_WORKER_CORE
#error "System/application tasks and AI worker must use different HP cores"
#endif

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
 * Asynchronous AI worker. CPU1 receives only one latest-frame snapshot and
 * never builds a backlog of stale frames. All application-created non-AI
 * tasks are kept on APP_SYSTEM_WORKER_CORE.
 */
#define APP_AI_SNAPSHOT_MAX_EDGE                    320U
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
 * the normal detection interval. With both saved Power Modes enabled, the
 * coordinated Light-sleep path starts at 15 seconds and Deep-sleep follows
 * 10 seconds later unless touch/GPIO3 restores Active mode.
 */
#define APP_CPU_MAX_FREQ_MHZ                        360
#define APP_CPU_ACTIVE_FREQ_MHZ                     180
#define APP_CPU_IDLE_180_AFTER_MS                   5000U
#define APP_CPU_IDLE_90_AFTER_MS                    10000U

/*
 * Runtime inactivity sleep policy.
 *
 * Light Sleep and Deep Sleep are selected independently from Settings and
 * persisted in NVS. The generated APP_SLEEP_POLICY value is retained only so
 * the existing flashing helper and build layout stay compatible; app_sleep.c
 * does not use it to select behavior.
 *
 *   Light ON  + Deep ON : full Light-sleep at 15 s, then true Deep 10 s later.
 *   Light ON  + Deep OFF: the SAME full Light-sleep state at 15 s, indefinitely.
 *   Light OFF + Deep ON : Deep only at 7 s.
 *   Light OFF + Deep OFF: automatic sleep disabled.
 *
 * V15 deliberately makes every Light-sleep selection use the same trigger and
 * the same external-peripheral state. APP_SINGLE_SLEEP_TIMEOUT_MS is therefore
 * retained only for Deep-only mode.
 */
#define APP_SINGLE_SLEEP_TIMEOUT_MS                 7000U

#if APP_SLEEP_POLICY != APP_SLEEP_POLICY_HYBRID && \
    APP_SLEEP_POLICY != APP_SLEEP_POLICY_LIGHT_ONLY && \
    APP_SLEEP_POLICY != APP_SLEEP_POLICY_DEEP_ONLY
#error "Invalid APP_SLEEP_POLICY"
#endif

#if APP_SINGLE_SLEEP_TIMEOUT_MS == 0
#error "APP_SINGLE_SLEEP_TIMEOUT_MS must be greater than zero"
#endif

/* Cache synchronization */
#define APP_SYNC_CACHE_AROUND_OVERLAY               1

/* Light-sleep configuration */
#define APP_LIGHT_SLEEP_WAKE_GPIO                   GPIO_NUM_3
#define APP_LIGHT_SLEEP_TIMEOUT_MS                  15000U
#define APP_POWER_MODES_LIGHT_TO_DEEP_GAP_MS        10000U
/*
 * Light-sleep touch responsiveness:
 * Poll GT911 every 250 ms instead of every 1000 ms. This intentionally trades
 * a small amount of Light-sleep energy for up to 4x faster touch detection,
 * while preserving the C6-at-80-MHz and Deep-sleep policies unchanged.
 */
#define APP_LIGHT_SLEEP_TOUCH_POLL_MS               250U

/*
 * Light-sleep visual-standby experiment:
 * Keep only the physical LCD backlight powered while the JD9365 panel,
 * LVGL/MIPI-DSI transport and camera remain suspended. This intentionally
 * trades some Light-sleep energy for a visible standby indication. The
 * backlight is forced OFF again before true Deep-sleep.
 */
#define APP_LIGHT_SLEEP_KEEP_BACKLIGHT_ON            1
#define APP_LIGHT_SLEEP_BUTTON_POLL_MS              5U
#define APP_LIGHT_SLEEP_BUTTON_DEBOUNCE_MS          25U

/*
 * Documentation-aligned external-peripheral Light-sleep policy (V8).
 *
 * ESP-IDF automatically clock-gates/reduces the ESP32-P4 internal domains that
 * are not required by the configured wake sources. External board devices are
 * outside the SoC power-domain controller, so the application must quiesce them
 * explicitly if they are not needed while sleeping.
 *
 * GT911 is intentionally NOT powered down here because this board does not
 * expose a verified GT911 INT/RESET wake pin; touchscreen wake is implemented
 * by the existing RTC/I2C polling path.
 *
 * microSD is also intentionally kept powered and mounted in Light-sleep.
 * Light-sleep is a state-preserving mode: unmounting/remounting FATFS allocates
 * fresh VFS/SDMMC resources and previously caused ESP_ERR_NO_MEM on wake,
 * followed by an application reset. Only Deep-sleep may unmount and power-gate
 * the card because a Deep-sleep wake is a full system restart by design.
 *
 * The ESP32-C6 follows the persistent Wi-Fi setting during Active mode. The
 * ordered Deep-sleep path still forces CHIP_PU LOW at the sleep boundary.
 */
#define APP_LIGHT_SLEEP_DISABLE_AUDIO_AMP            1
/*
 * microSD has no Light-sleep power-down toggle by design. The mounted card is
 * part of the retained application state. Deep-sleep owns SD unmount/power-off.
 */

/*
 * IP101GRI Light-sleep reduced-bandwidth experiment:
 *
 * Do NOT assert the PHY RESET pin in Light-sleep. Keep the PHY powered and
 * temporarily force its standard MII Control Register to 10 Mbps. This gives a
 * deterministic, immediate reduced-Ethernet state without waiting for the
 * IP101G WOL+ sleep-ready timer.
 *
 * The original BMCR value is restored when Light-sleep returns to Active mode.
 * Deep-sleep remains unchanged and still asserts GPIO51 RESET LOW.
 */
#define APP_LIGHT_SLEEP_ETHERNET_REDUCED_10M         1

/*
 * GT911 Light-sleep false-wake filter (V8, preserving the proven V6 driver fix).
 *
 * Root cause:
 *   esp_lcd_touch_gt911_read_data() can return with the controller's DATA_READY
 *   bit clear without invalidating the driver's cached tp->data.points value.
 *   A previous one-frame touch can therefore be returned again by
 *   esp_lcd_touch_get_coordinates() even though no new GT911 touch packet exists.
 *
 * The V6 installer patches that managed GT911 driver in place so DATA_READY=0
 * invalidates cached points. The time-qualified filter below remains as a
 * secondary guard against a genuine one-frame electrical/transient touch packet:
 *
 *   1. Ignore every touch sample during the startup quarantine.
 *   2. Require a continuous RELEASED window before touch wake is armed.
 *   3. After arming, require a continuous PRESSED window before restoring
 *      camera/display.
 *
 * With the 250 ms poll interval below, the defaults mean:
 *   startup ignored : 1000 ms
 *   stable release  : 1000 ms
 *   stable press    :  500 ms
 *
 * GPIO3 remains available as an immediate Light-sleep wake source.
 */
#define APP_LIGHT_SLEEP_TOUCH_PRE_RELEASE_SAMPLES    3U
#define APP_LIGHT_SLEEP_TOUCH_PRECHECK_MAX_SAMPLES   12U
#define APP_LIGHT_SLEEP_TOUCH_SAMPLE_DELAY_MS        15U
#define APP_LIGHT_SLEEP_TOUCH_STARTUP_IGNORE_MS      1000U
#define APP_LIGHT_SLEEP_TOUCH_RELEASE_STABLE_MS      1000U
#define APP_LIGHT_SLEEP_TOUCH_PRESS_STABLE_MS        500U

/*
 * esp_light_sleep_start() may return a few hundred microseconds before the
 * requested timer deadline because of timer/clock quantization and software
 * overhead. Treat only a materially early return as abnormal. This prevents
 * normal ~249.4 ms returns for a 250 ms slice from being reported as a
 * Light-sleep failure.
 */
#define APP_LIGHT_SLEEP_EARLY_RETURN_TOLERANCE_US   5000LL

/* Deep-sleep configuration. The 30000 ms value is legacy compatibility only. */
#define APP_DEEP_SLEEP_TIMEOUT_MS                   30000U
#define APP_DEEP_SLEEP_INACTIVITY_POLL_MS           100U
#define APP_DEEP_SLEEP_BUTTON_GPIO                  GPIO_NUM_3
#define APP_DEEP_SLEEP_BUTTON_POLL_MS               5U
#define APP_DEEP_SLEEP_BUTTON_DEBOUNCE_MS           25U
#define APP_DEEP_SLEEP_BUTTON_PRIORITY              8

#if APP_POWER_MODES_LIGHT_TO_DEEP_GAP_MS == 0
#error "APP_POWER_MODES_LIGHT_TO_DEEP_GAP_MS must be greater than zero"
#endif

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
 *   GPIO31 -> IP101GRI MDC           Clause-22 management clock
 *   GPIO52 -> IP101GRI MDIO          Clause-22 management data
 *   GPIO51 -> IP101GRI PHY RESET     LOW = reset, HIGH = released
 *   GPIO53 -> NS4150B amplifier EN   LOW = amplifier disabled
 *   GPIO45 -> Q1 (AO3401) gate       HIGH = SD1_VDD disconnected
 *   GPIO7  -> shared I2C SDA         ES8311/GT911/display control
 *   GPIO8  -> shared I2C SCL         ES8311/GT911/display control
 */
#define APP_PWR_WIFI_C6_CHIP_PU_GPIO                GPIO_NUM_54
#define APP_PWR_WIFI_C6_DISABLED_LEVEL              0
#define APP_PWR_WIFI_C6_ENABLED_LEVEL               1

/*
 * P4 <-> C6 low-power mode sideband.
 *
 * ESP32-P4-NANO schematic:
 *   P4 GPIO6 -- R52 (0R) --> C6 GPIO2
 *
 * HIGH before C6 reset/release = boot the custom C6 firmware into the
 * retained 80 MHz Light-sleep companion state.
 * LOW = normal/deep policy. During real P4 Deep-sleep the C6 boots with this
 * line LOW and immediately enters its existing self-Deep-sleep path.
 */
#define APP_PWR_WIFI_C6_MODE_GPIO                   GPIO_NUM_6
#define APP_PWR_WIFI_C6_MODE_DEEP_LEVEL             0
#define APP_PWR_WIFI_C6_MODE_LIGHT_LEVEL            1
#define APP_PWR_WIFI_C6_RESET_PULSE_MS              20U
#define APP_PWR_WIFI_C6_LIGHT_BOOT_SETTLE_MS        150U
#define APP_PWR_WIFI_C6_ACTIVE_SETTLE_MS            50U

/*
 * IP101GRI low-current policy.
 *
 * Waveshare documents MDC=GPIO31, MDIO=GPIO52 and RESET=GPIO51. Earlier
 * Deep-sleep code released RESET and programmed BMCR bit11. Bench measurement
 * of the Hybrid Light->Deep path showed the lower plateau while RESET stayed
 * asserted in Light-sleep, so Deep-sleep now preserves that electrical state:
 * GPIO51 stays LOW and held until the P4 actually enters Deep-sleep.
 *
 * MDC/MDIO definitions are retained for board documentation and future
 * diagnostics, but normal low-power entry no longer wakes the PHY for MDIO.
 */
#define APP_PWR_ETHERNET_MDC_GPIO                   GPIO_NUM_31
#define APP_PWR_ETHERNET_MDIO_GPIO                  GPIO_NUM_52
#define APP_PWR_ETHERNET_PHY_RESET_GPIO             GPIO_NUM_51
#define APP_PWR_ETHERNET_PHY_ADDRESS                1
#define APP_PWR_ETHERNET_RESET_ACTIVE_LEVEL         0
#define APP_PWR_ETHERNET_RESET_RELEASED_LEVEL       1

/*
 * Audio Deep-sleep controls.
 *
 * GPIO53 gates only the NS4150B speaker amplifier. The separate ES8311 codec
 * remains powered from the board rail and must be suspended over I2C.
 */
#define APP_PWR_AUDIO_AMP_GPIO                      GPIO_NUM_53
#define APP_PWR_AUDIO_AMP_DISABLED_LEVEL            0
#define APP_PWR_AUDIO_AMP_ENABLED_LEVEL             1
#define APP_PWR_ES8311_I2C_ADDRESS                  0x18
#define APP_PWR_ES8311_I2C_CLOCK_HZ                 400000U
#define APP_PWR_ES8311_I2C_TIMEOUT_MS               100U

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
 * V19 intentionally uses the GT911's real full-Sleep command
 * (0x05 -> register 0x8040) instead of Green/low-speed scanning.
 *
 * IMPORTANT STOCK-BOARD LIMITATION:
 * Waveshare's ESP32-P4-NANO BSP exposes the GT911 INT and RESET pins as
 * GPIO_NUM_NC. The GT911 programming guide requires INT-high or RESET to
 * wake from full Sleep. Therefore the P4 cannot restore touch after a
 * Deep-sleep wake on this stock wiring. This V19 build is a deepest-software
 * power-measurement build: after waking the P4 with GPIO3, a complete board
 * power-cycle is required before touch can work again.
 *
 * The sleep command is verified before the P4 sleeps by confirming that:
 *   1. the GT911 stops ACKing on I2C after >58 ms; and
 *   2. the shared I2C bus itself is still alive through the ES8311 address.
 */
#define APP_PWR_GT911_SLEEP_ENABLED                 1
#define APP_PWR_GT911_ALLOW_SLEEP_WITHOUT_HOST_WAKE 1
#define APP_PWR_GT911_SLEEP_VERIFY_DELAY_MS         70U
#define APP_PWR_GT911_PRIMARY_ADDRESS               0x5D
#define APP_PWR_GT911_SECONDARY_ADDRESS             0x14

/* Timings used only when a future board exposes a real wake pin. */
#define APP_PWR_GT911_RESET_ASSERT_MS               20
#define APP_PWR_GT911_INT_PULSE_MS                  5
#define APP_PWR_GT911_BOOT_MS                       60

/* No dedicated backlight GPIO on the stock ESP32-P4-NANO BSP. */
#define APP_PWR_DISPLAY_BACKLIGHT_GPIO              (-1)
#define APP_PWR_DISPLAY_BACKLIGHT_OFF_LEVEL         0

/* Stock BSP: GT911 INT is GPIO_NUM_NC. */
#define APP_PWR_TOUCH_INT_GPIO                      (-1)

/* Stock BSP: GT911 RESET is GPIO_NUM_NC. */
#define APP_PWR_TOUCH_RESET_GPIO                    (-1)
#define APP_PWR_TOUCH_RESET_ACTIVE_LEVEL            0

/*
 * Green mode is explicitly disabled in V19. The GT911 goes from normal
 * operation directly into full Sleep at the destructive Deep-sleep boundary.
 */
#define APP_PWR_GT911_GREEN_MODE_ENABLED            0
#define APP_PWR_GT911_GREEN_IDLE_SECONDS            0U

/*
 * Final ESP32-P4 software-only Deep-sleep pass.
 *
 * - Explicitly request OFF for P4 power domains that are not needed by the
 *   single GPIO3 wake source. ESP-IDF's default AUTO policy should already
 *   remove these domains, but the explicit requests make the intent auditable
 *   and prevent a stale application setting from keeping them on.
 * - Float only board peripheral signal pins whose owning peripherals have
 *   already been shut down. Control rails (GPIO45/53/54) and wake GPIO3 are
 *   deliberately excluded.
 * - UART0 GPIO37/38 are floated at the very last boundary, after fflush(), so
 *   the CH343P-side pins cannot create a P4 I/O leakage path during sleep.
 */
#define APP_PWR_DEEP_SLEEP_FORCE_DOMAINS_OFF        1
#define APP_PWR_QUIESCE_PERIPHERAL_SIGNAL_PINS      1
#define APP_PWR_FLOAT_UART0_AT_FINAL_BOUNDARY       1

/*
 * Schematic-confirmed board signal pins that can be released only after all
 * corresponding subsystems are quiesced:
 *
 *   GPIO6                    C6 power-mode sideband (NOT floated; retained LOW)
 *   GPIO9..13                ES8311 I2S
 *   GPIO14..19,24,25         P4<->C6 SDIO/sideband
 *   GPIO20,21                exposed C6-programming UART bridge pins
 *   GPIO28,29,30,34,35,49,50 IP101GRI RMII
 *   GPIO46,47                CSI side-channel pins
 *
 * Ethernet MDC/MDIO (31/52) are not driven by the low-current path. RESET
 * GPIO51 is a control rail and must stay LOW/held, so it is deliberately not
 * part of the floating list. SDMMC 39..44 are handled separately. GPIO7/8 are
 * RTC-isolated separately. GPIO45/51/53/54 must retain their control policy.
 */
#define APP_PWR_PERIPHERAL_SIGNAL_PIN_LIST          \
    { 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, \
      20, 21, 24, 25, 28, 29, 30, 34, 35, 46, 47, 49, 50 }

#define APP_PWR_UART0_PIN_LIST                      { 37, 38 }

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
 * After SD1_VDD is switched off, stop the P4 from driving the powered-down
 * card through the SDMMC signals. Waveshare's ESP32-P4-NANO pin map confirms:
 *
 *   D0=GPIO39, D1=GPIO40, D2=GPIO41, D3=GPIO42,
 *   CLK=GPIO43, CMD=GPIO44.
 *
 * At the final Deep-sleep boundary these pins are changed to input/no-pull.
 * No persistent HP-GPIO hold is used: the tested P4 rev-v1.3 cannot be assumed
 * to retain arbitrary HP pad holds once the HP domain powers down.
 */
#define APP_PWR_ISOLATE_SDMMC_PINS                  1
#define APP_PWR_SDMMC_PIN_LIST                      { 39, 40, 41, 42, 43, 44 }

/*
 * Normal-measurement build: do not pause between Deep-sleep shutdown stages.
 *
 * The previous 3000 ms diagnostic windows intentionally kept the P4 awake
 * after every component transition. Those windows produced the repeating
 * ~3-second current staircase visible in Joulescope and are not part of the
 * real Light-sleep behavior. Keep the profiler compiled in, but make its
 * measurement delay zero so shutdown proceeds continuously.
 */
#ifndef APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS
#define APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS      0U
#endif

/*
 * Even with diagnostic delays disabled, Hybrid Light->Deep must still enter
 * the real aggressive ESP32-P4 Deep-sleep path. Do not fall back to the older
 * continuous-Light-sleep "retentive deep" compatibility mode.
 */
#ifndef APP_SLEEP_POWER_PROFILE_TRUE_DEEP_FROM_LIGHT
#define APP_SLEEP_POWER_PROFILE_TRUE_DEEP_FROM_LIGHT 1
#endif

/*
 * Normally full GT911 Sleep is refused when no host wake pin exists.
 * V19 explicitly opts into this only for deepest-software current measurement.
 */
#if APP_PWR_GT911_SLEEP_ENABLED && \
    (APP_PWR_TOUCH_INT_GPIO < 0) && (APP_PWR_TOUCH_RESET_GPIO < 0) && \
    !APP_PWR_GT911_ALLOW_SLEEP_WITHOUT_HOST_WAKE
#error "GT911 full Sleep needs INT/RESET for normal wake; set APP_PWR_GT911_ALLOW_SLEEP_WITHOUT_HOST_WAKE only for measurement builds"
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
