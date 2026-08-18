# Deep-sleep fix — file drop (rev 3)

## Rev 3 — read this first

**If the board is currently in a boot loop:** fully unplug it — USB *and* any
other supply — wait ~10 s, plug back in. A reset button or `idf.py flash` will
not clear it.

Rev 2 put the GT911 to sleep successfully, and that turned out to be a one-way
trip. The controller runs from the display module's always-on 3.3 V rail, so
the sleep command survived the P4 reset; its I2C interface is off while asleep,
so the next boot aborted in

    bsp_display_indev_init -> bsp_touch_new() -> ESP_ERR_NOT_FOUND

inside `ESP_ERROR_CHECK`, looping forever. With both touch pins at `-1` nothing
in the firmware could wake it. This would also have broken the Deep-sleep wake
path, not just reflashing.

Rev 3:

- `APP_PWR_GT911_SLEEP_ENABLED` now defaults to **0**, so this cannot recur.
- The build now **refuses** to enable it without `APP_PWR_TOUCH_INT_GPIO` or
  `APP_PWR_TOUCH_RESET_GPIO` — a `#error`, not a runtime surprise.
- `component_display_wake_touch_after_reset()` performs the hardware wake
  (RESET toggle, or INT pulse) and runs automatically before `app_main()`, so
  once a pin is configured the sleep is safe and self-recovering. No edit to
  `app_main.c` is needed.

To get the ~5–10 mA back, find the GT911 INT or RESET pin in the Waveshare
schematic, set it in `app_config.h`, and flip `APP_PWR_GT911_SLEEP_ENABLED`
to 1. Everything else in rev 2 is unchanged and was working.

## Rev 2 — from the first flash-and-monitor run

1. **False "Wi-Fi coprocessor shutdown failed".** GPIO54 read HIGH on the
   instruction after `gpio_hold_en()`, but the rail audit 50 ms later read it
   LOW and `OK`. The hold was fine; the verification was too eager. All four
   component read-backs now poll the pad for up to 150 ms before declaring a
   failure, and report how long it took to settle.
2. **`Could not request flash power-down: ESP_ERR_INVALID_STATE`.** Expected on
   this build — `.text` and `.rodata` run XIP from the PSRAM that shares the
   VDD_SPI rail, so IDF will not switch that rail off. Now logged as
   information instead of a warning.
3. **Light-sleep was consuming its whole ~15 s budget in ~96 ms.** Each
   250 ms slice returned almost immediately with a TIMER wake, but the loop
   still charged the budget a full 250 ms — so the board dropped into
   Deep-sleep about 15 s early and the Light-sleep stage saved nothing. Each
   slice is now padded to a full slice of real time, and the first short slice
   logs `LIGHT_SLEEP_SLICE_TOO_SHORT` with the measured duration.

All paths are relative to `src/` in `Bachelor_Thesis_ESP`.

| File | Change |
|---|---|
| `main/include/config/app_config.h` | **replace** — all peripheral pins centralised, new deep-sleep config block, profiling delay set to 0 |
| `main/include/power_save/component_display.h` | **new file** |
| `main/power/component_display.c` | **new file** — GT911 sleep + backlight/INT/RESET holds |
| `main/power/deep_sleep.c` | **replace** — pre-sleep rail audit, flash power-down, optional SDMMC float |
| `main/power/app_sleep.c` | **replace** — new STEP 3, renumbered stages, restore path |
| `main/power/component_wifi.c` | **replace** — read-back verification |
| `main/power/component_ethernet.c` | **replace** — adds the missing `gpio_sleep_sel_dis()` + read-back |
| `main/power/component_audio.c` | **replace** — adds `gpio_sleep_sel_dis()` + read-back |
| `main/power/component_sdcard.c` | **replace** — adds `gpio_sleep_sel_dis()` + read-back |
| `main/diagnostics/app_logging.c` | **replace** — un-mutes `component_display` and `PWR_AUDIT` |
| `main/CMakeLists.txt` | **replace** — registers `power/component_display.c` |

No other file is touched. Headers you already have
(`component_wifi.h`, `component_ethernet.h`, `component_audio.h`,
`component_sdcard.h`, `app_sleep.h`, `deep_sleep.h`, `light_sleep.h`)
are unchanged — do not overwrite them.

---

## Before you flash: three pins to fill in

Open `main/include/config/app_config.h` and look for the display block.
These three default to `-1` (step skipped) because the exported project does
not reveal them; take them from the Waveshare schematic for your display:

```c
#define APP_PWR_DISPLAY_BACKLIGHT_GPIO   (-1)   /* backlight enable / PWM  */
#define APP_PWR_TOUCH_INT_GPIO           (-1)   /* GT911 INT               */
#define APP_PWR_TOUCH_RESET_GPIO         (-1)   /* GT911 RESET             */
```

The GT911 I2C sleep command works without them. The pins matter because a
floating backlight-enable pad can leave the driver partly on, and a floating
INT can wake the GT911 straight back out of sleep.

---

## What you should see in the log

`PWR_AUDIT` prints one line per controlled rail right before sleep:

```
I PWR_AUDIT ---- Pre-Deep-sleep rail audit ----------------------
I PWR_AUDIT GPIO54 ESP32-C6 CHIP_PU       level=0 expected=0  OK
I PWR_AUDIT GPIO51 IP101GRI PHY RESET     level=0 expected=0  OK
I PWR_AUDIT GPIO53 audio amplifier EN     level=0 expected=0  OK
I PWR_AUDIT GPIO45 microSD Q1 gate        level=1 expected=1  OK
I PWR_AUDIT GPIO7  shared I2C SDA         level=1 expected=1  OK
I PWR_AUDIT GPIO8  shared I2C SCL         level=1 expected=1  OK
```

Any `MISMATCH` line names the consequence directly, e.g.

```
E PWR_AUDIT GPIO51 IP101GRI PHY RESET     level=1 expected=0  MISMATCH
  -> Ethernet PHY is auto-negotiating (20-40 mA)
```

This is the measurement I suggested doing with a multimeter — now the firmware
does it for you on every sleep entry, which also gives you a reproducible
artefact for the thesis.

---

## Measuring afterwards

1. Power the board from a bench supply or a power-only USB cable.
   A data USB connection keeps the UART bridge enumerated and adds current
   that will not exist in the deployed device.
2. Record the plateau after `STEP 8`.
3. For per-component attribution, set
   `APP_SLEEP_POWER_PROFILE_STAGE_DELAY_MS` to `3000` **temporarily** and read
   the current step by step. Set it back to `0` afterwards — the old default of
   3000 was adding ~21 s at full power to every deep-sleep entry.
4. Unplug tests give the rest: sleep with the camera FPC removed, then with the
   display FPC removed. The deltas are your always-on hardware budget.

---

## Optional: SDMMC back-powering

After `SD1_VDD` is switched off, the card can still be fed through its signal
lines. If the audit is clean but sleep current is still higher than expected,
confirm the SDMMC pin numbers in the schematic, then set:

```c
#define APP_PWR_ISOLATE_SDMMC_PINS   1
#define APP_PWR_SDMMC_PIN_LIST       { 39, 40, 41, 42, 43, 44 }   /* verify! */
```

Leave it at `0` until the numbers are confirmed — floating the wrong pad can
disturb an unrelated peripheral.
