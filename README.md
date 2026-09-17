# Power Efficient Operation of an Edge AI Device (ESP32-P4)

Bachelor thesis project for a power-efficient, on-device face-recognition and door-access system built around the **Waveshare ESP32-P4-NANO**.

The system combines local Edge AI inference, camera-to-display processing, multi-user face recognition, PIN-based second-factor authentication, a touchscreen GUI, dynamic CPU power management, Light-sleep, Deep-sleep, peripheral power control, and wake-up performance instrumentation.

**Thesis:** *Power Efficient Operation of an Edge AI Device (Espressif ESP32-P4)*  
**Platform:** Waveshare ESP32-P4-NANO  
**Framework:** ESP-IDF 5.5.4 / FreeRTOS  
**Primary application:** `src_p4/`

---

## Purpose of This README

This README is the technical entry point for the GitHub repository.

Its purpose is to:

- explain what the Bachelor thesis project implements and why it exists;
- identify the main hardware and software components;
- provide a reproducible ESP32-P4 build and flashing procedure;
- explain the software architecture and the responsibilities of the main modules;
- summarize the power-management, AI, GUI, authentication, and wake-up behavior;
- document the main measured achievements of the thesis;
- help another developer understand where to start without first reading the complete source tree or thesis.

The README is intentionally a repository-level overview. Detailed implementation decisions, measurements, experiments, background theory, and references remain part of the Bachelor thesis and the source-code documentation.

---

## Description

The project investigates how a continuously operating **Edge AI face-recognition system** can reduce total board power consumption while still preserving useful response time and application functionality.

The application is designed as the embedded part of a door-access system. Camera frames are captured locally on the ESP32-P4, processed by the image pipeline, displayed on the attached MIPI-DSI screen, and analyzed by an on-device face-detection and face-recognition pipeline. No cloud inference is required for the authentication pipeline.

A recognized user proceeds to a **PIN-entry stage** as a second authentication factor. Multiple users can be enrolled using reference images stored on the microSD card.

Power optimization is applied to the complete system rather than only to the ESP32-P4 CPU. The implementation coordinates CPU frequency, AI scheduling, display brightness, camera operation, the ESP32-C6 wireless co-processor, Ethernet PHY, audio subsystem, microSD, touchscreen, and the display when entering lower-power states.

### Main Functions

- On-device face detection and face recognition
- Multiple enrolled users
- Four-digit PIN second-factor authentication
- Camera preview on a MIPI-DSI display
- LVGL launcher, settings, and PIN-entry interfaces
- Persistent configuration
- Active-mode Dynamic Frequency Scaling and workload-aware behavior
- Light-sleep with application-state preservation
- Deep-sleep with GPIO wake-up
- Peripheral-specific low-power control
- Wake-up latency measurement
- Persistent wake-time log files on microSD
- Modular, layered software architecture

### Main Hardware

The project is centered around the **ESP32-P4-NANO** and uses the board-level peripherals required by the application, including:

- ESP32-P4 dual high-performance RISC-V cores
- 32 MB PSRAM
- ESP32-C6-MINI-1 wireless co-processor
- MIPI-CSI camera interface
- OV5647 camera
- MIPI-DSI display path
- 10.1-inch touch display
- Goodix GT9271/GT911-compatible touch interface
- IP101GRI Ethernet PHY
- ES8311 audio codec
- NS4150B audio amplifier
- microSD storage

---

## Repository Overview

```text
Bachelor_Thesis_ESP/
├── src_p4/                 # Main ESP32-P4 thesis application
│   ├── main/               # Application, services, platform adapters and UI
│   ├── managed_components/ # ESP-IDF / BSP managed dependencies
│   └── ...
├── src_c6/                 # ESP32-C6 support / companion projects
├── results/                # Measurement and evaluation results
├── tools/                  # Development / project tools
├── utilities/              # Flashing, logging and helper scripts
├── legacy/                 # Retained legacy material
├── power_patch_backups/    # Historical power-related development material
└── ...
```

The current `src_p4/main/` application is split into application logic, domain definitions, services, platform-specific adapters, diagnostics, and user-interface modules.

---

# Flashing Process

## Requirements

Before building the ESP32-P4 firmware, install and configure:

- **ESP-IDF 5.5.4**
- Git
- CMake/Ninja through the ESP-IDF environment
- Python dependencies installed by ESP-IDF
- A USB connection to the ESP32-P4-NANO

The examples below assume that the board appears as:

```text
/dev/ttyACM0
```

If your system assigns another serial device, replace `/dev/ttyACM0` with the correct port.

## 1. Clone the Repository

```bash
git clone <repository-url>
cd Bachelor_Thesis_ESP
```

## 2. Load ESP-IDF 5.5.4

```bash
source ~/esp/esp-idf-v5.5.4/export.sh
```

Verify:

```bash
idf.py --version
```

Expected project toolchain:

```text
ESP-IDF v5.5.4
```

## 3. Enter the ESP32-P4 Project

```bash
cd src_p4
```

## 4. Clean Only the Build Directory

```bash
rm -rf build
```

> **Important:** Do not use `idf.py fullclean` as the normal clean procedure for this project. The repository can contain intentional project-specific changes inside managed components. Removing only `src_p4/build` preserves those component sources while still forcing a clean application rebuild.

## 5. Reconfigure

```bash
idf.py reconfigure
```

## 6. Build

```bash
idf.py build
```

## 7. Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

## 8. Open the Serial Monitor

```bash
idf.py -p /dev/ttyACM0 monitor
```

Exit the ESP-IDF monitor with:

```text
Ctrl + ]
```

## Complete Build and Flash Sequence

```bash
source ~/esp/esp-idf-v5.5.4/export.sh

cd src_p4
rm -rf build

idf.py reconfigure
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM0 monitor
```

---

# Software Architecture

The ESP32-P4 application follows a **layered and interface-oriented architecture**. The design separates high-level application decisions from hardware-specific implementation details.

The main goals are:

- separation of concerns;
- high cohesion inside individual modules;
- low coupling between unrelated subsystems;
- information hiding;
- dependency inversion;
- replaceable hardware and AI backends;
- maintainable power-management behavior.

## High-Level Architecture

```mermaid
flowchart TD
    UI["User Interface<br/>Launcher · Settings · PIN · Camera Overlay"]
    APP["Application Logic<br/>Runtime · Navigation · Controller · State Machine<br/>Authentication · Camera Session"]
    AI["Face Recognition Service<br/>Detection · Recognition · Snapshot Scheduling<br/>Result Processing · AI Worker"]
    CFG["Configuration Service<br/>Persistent Settings"]
    PM["Power Management<br/>DFS · Inactivity Policy · Light Sleep · Deep Sleep<br/>Peripheral Power Policy"]
    ADAPTER["System Adapters<br/>Camera · Display · Storage · CPU/Power"]
    PLATFORM["Platform Layer<br/>ESP32-P4 / BSP-specific implementation"]
    ESPIDF["ESP-IDF + Managed Components<br/>FreeRTOS · LVGL · ESP-DL · Drivers · BSP"]

    UI --> APP
    APP --> AI
    APP --> CFG
    APP --> PM
    APP --> ADAPTER
    AI --> ADAPTER
    PM --> ADAPTER
    CFG --> ADAPTER
    ADAPTER --> PLATFORM
    PLATFORM --> ESPIDF
```

## User Interface

The presentation layer is located mainly under:

```text
src_p4/main/ui/
```

It contains:

- launcher screen;
- settings screen;
- PIN-entry screen;
- live camera information and face overlay.

The launcher, settings, and PIN interfaces use **LVGL**. The live camera view is handled separately through the direct display path.

## Application Logic

Application coordination is located mainly under:

```text
src_p4/main/app/
```

Responsibilities include:

- boot/runtime coordination;
- navigation;
- application state transitions;
- camera-session ownership;
- authentication workflow;
- configuration integration;
- coordination between UI, AI, power management, and platform services.

Important logical states include:

```text
BOOTING
LAUNCHER
SETTINGS
CAMERA_ACTIVE
AUTHENTICATING
PIN_ENTRY
LIGHT_SLEEP
DEEP_SLEEP
```

Simplified flow:

```text
BOOTING
   |
   v
LAUNCHER
   |
   v
CAMERA_ACTIVE
   |
   +---- face recognized ----> AUTHENTICATING
                                |
                                v
                             PIN_ENTRY
                                |
                                v
                           CAMERA_ACTIVE
```

## Face Recognition Service

AI functionality is separated under:

```text
src_p4/main/services/vision/
```

It contains:

- face detection;
- face recognition;
- AI snapshot management;
- AI worker runtime;
- inference synchronization;
- recognition policy;
- result processing;
- backend abstraction.

The default thesis path uses **ESP-DL**.

The source also provides comparison backends for:

```text
ESP-DL
TensorFlow Lite Micro FP32
TensorFlow Lite Micro INT8 + ESP-NN
```

The detector and recognizer expose common interfaces so the surrounding camera, scheduling, authentication, CPU, and sleep policies do not have to be rewritten when the inference backend changes.

## Configuration Service

Persistent application settings are separated from the hardware operations that apply those settings.

Managed values include:

- light/dark GUI theme;
- Wi-Fi state;
- Ethernet state;
- camera state;
- audio state;
- microSD state;
- Active-mode optimization;
- Light-sleep enable state and delay;
- Deep-sleep enable state and delay.

## Power Management

Power-management code is located primarily under:

```text
src_p4/main/services/power/
src_p4/main/platform/power/
```

It coordinates:

- inactivity tracking;
- Dynamic Frequency Scaling;
- AI-related CPU boost behavior;
- staged Active-mode optimization;
- display/backlight policy;
- Light-sleep entry and restoration;
- Deep-sleep entry;
- camera shutdown/restoration;
- touchscreen policy;
- ESP32-C6 state;
- Ethernet PHY state;
- audio codec/amplifier state;
- microSD state.

## System Adapters and Platform Layer

Important locations include:

```text
src_p4/main/platform/
src_p4/main/platform/system/
src_p4/main/include/domain/ports/
```

The adapter boundary hides ESP-IDF and board-specific details behind application-oriented interfaces for:

- camera;
- display;
- storage;
- CPU power;
- peripheral power;
- BSP/driver functionality.

## ESP Managed Components

ESP-IDF, board-support packages, display/touch drivers, AI libraries, and other managed components provide the low-level software foundation.

The project contains project-specific component changes required for the target hardware. The normal repository workflow therefore preserves `managed_components` during clean builds.

---

# Power-State Behavior

## Active Mode

Active mode runs the camera/display path and AI application while applying workload-dependent optimization.

The implementation can adjust:

- CPU operating policy;
- face-detection scheduling;
- AI work rate;
- preview work rate;
- backlight level.

## Light-Sleep

After the configured inactivity period, the application can suspend high-consumption hardware while preserving application state.

A valid touchscreen wake restores the suspended hardware and returns directly to camera operation without a complete reboot.

## Deep-Sleep

Deep-sleep is used for longer inactivity periods.

Before entering Deep-sleep, the software performs an ordered peripheral shutdown.

The physical GPIO3 rocker is used as the Deep-sleep wake source.

After a GPIO3 Deep-sleep wake, the current firmware uses this fast path:

```text
Deep-sleep GPIO3 wake
        |
        v
ESP32-P4 reset / boot
        |
        v
Application runtime
        |
        v
Prepare display/touch infrastructure
        |
        v
Bypass launcher UI
        |
        v
Initialize camera + AI
        |
        v
First live camera frame
```

A normal cold boot still creates the launcher.

---

# Wake-Up Timing and Logging

The current firmware measures recovery time until the **first successfully presented camera frame**.

## Light-Sleep Timing

Scope:

```text
WAKE_CONFIRMED_TO_FIRST_FRAME
```

The timer begins when touchscreen activity is confirmed as a valid Light-sleep wake event.

## Deep-Sleep Timing

Scope:

```text
APP_RUNTIME_TO_FIRST_FRAME
```

Because real Deep-sleep resets the processor and `esp_timer_get_time()`, this software metric does **not** include ROM/bootloader latency.

## Persistent Wake Logs

Every completed Light-sleep or Deep-sleep wake measurement is written to a separate file on the microSD card:

```text
/sdcard/logs/
```

FAT-compatible filenames are used:

```text
L000001.LOG
L000002.LOG
D000003.LOG
L000004.LOG
```

`L` identifies Light-sleep and `D` identifies Deep-sleep.

Example:

```text
wake_entry=3
mode=DEEP_SLEEP
start_event=APP_RUNTIME_ENTRY
end_event=FIRST_CAMERA_FRAME
start_us=63524
end_us=5087454
elapsed_us=5023930
elapsed_ms=5023.930
elapsed_sec=5.023930
scope=APP_RUNTIME_TO_FIRST_FRAME excludes_ROM_BOOTLOADER
```

The end timestamp is captured **before** the SD-card file is written, so filesystem I/O is not included in the measured wake latency.

---

# Achievements

The developed system fulfills the main functional and power-management requirements defined for the Bachelor thesis.

## Functional Achievements

- Local face detection on the ESP32-P4
- Local face recognition on the ESP32-P4
- Multi-user enrollment and recognition
- microSD-based enrollment data
- PIN-based second-factor authentication
- Touchscreen launcher
- Runtime settings interface
- PIN-entry GUI
- Direct live camera preview
- Face-information overlay
- Persistent configuration
- Modular state-machine-based application control
- Pluggable AI backend interfaces
- Light-sleep recovery without a complete application reboot
- Deep-sleep with physical GPIO wake-up
- Direct camera startup after Deep-sleep wake without recreating the launcher
- Persistent per-wake timing logs on microSD

## Measured Power Achievements

| Operating mode | Average current | Reduction vs. full Active |
|---|---:|---:|
| Full Active, no optimization | **0.7849 A** | Reference |
| Optimized Active | **0.3762 A** | **52.1%** |
| Light-sleep | **0.0336 A** | **95.7%** |
| Deep-sleep | **0.0189 A** | **97.6%** |

The optimized Active implementation reduced average current from **784.9 mA to 376.2 mA** while remaining operational.

Light-sleep reduced average current to **33.6 mA**.

Deep-sleep produced the lowest measured average current, **18.9 mA**, corresponding to an approximately **97.6% reduction** compared with the full-power Active reference.

## Software-Engineering Achievements

- presentation separated from application decisions;
- explicit application state machine;
- AI implementation hidden behind detector/recognizer interfaces;
- persistent configuration separated from hardware control;
- power policy separated from device-specific operations;
- system adapters isolate higher-level code from ESP-IDF/BSP details;
- explicit camera/display ownership coordination;
- ordered sleep preparation and restoration;
- CPU, AI, power, and wake diagnostics;
- reproducible wake measurements across repeated Light-sleep and Deep-sleep cycles.

---

# Development Notes

## Build Cleaning

Use:

```bash
rm -rf src_p4/build
```

or, from `src_p4`:

```bash
rm -rf build
```

## Serial Diagnostics

Important runtime markers include:

```text
[APP-STATE]
[CORE-PROOF]
[WAKE-TIME]
[WAKE-LOG]
PWR_STATE
PWR_CPU
POWER_PROFILE
```

These markers are useful when associating firmware behavior with Joulescope measurements.

---

# Thesis Scope

The repository accompanies the Bachelor thesis:

> **Power Efficient Operation of an Edge AI Device (Espressif ESP32-P4)**

The central objective is system-level energy efficiency, including the processor, AI workload, camera, display, touch controller, wireless co-processor, Ethernet, audio, storage, and the software coordinating them.

The resulting application demonstrates that an embedded AI system can retain authentication and user-interface functionality while substantially reducing average current during inactive periods.

---

# License

No license is stated here automatically. Use the repository's license file, if one is added, as the authoritative licensing information.
