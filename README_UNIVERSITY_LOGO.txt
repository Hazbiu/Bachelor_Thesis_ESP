ESP32-P4 UNIVERSITY LOGO GUI UPDATE
==================================

Purpose
-------
Replaces the blue "AI" tile on the Face Recognition launcher with your
university logo. The source JPG is converted once on the PC to native RGB565
and then compiled directly into the ESP32-P4 firmware.

1. Copy this update over the repository root
---------------------------------------------
The ZIP already contains repository-relative paths.

2. Put your university logo here
--------------------------------
/home/admin/Programming/Bachelor_Thesis_ESP/src_p4/main/ui/assets/university_logo.jpg

The filename must be exactly:
    university_logo.jpg

A tightly cropped logo works best. The converter preserves aspect ratio and
limits the image to 220 x 88 pixels.

3. Convert the JPG
------------------
cd /home/admin/Programming/Bachelor_Thesis_ESP
bash utilities/prepare_university_logo.sh

If Pillow is missing:
python3 -m pip install --user Pillow
bash utilities/prepare_university_logo.sh

4. Build and flash using your existing official flow
----------------------------------------------------
cd /home/admin/Programming/Bachelor_Thesis_ESP
bash utilities/esp_AI_flashing.sh dl /dev/ttyACM0

What changed
------------
src_p4/main/CMakeLists.txt
    Compiles ui/assets/university_logo.c.

src_p4/main/ui/views/app_ui.c
    Displays the university logo above "Face Recognition". A neutral white
    logo card is used so a normal JPG works in light and dark mode.

src_p4/main/ui/assets/university_logo.h
src_p4/main/ui/assets/university_logo.c
    LVGL image asset interface. The included C file is only a safe fallback.
    prepare_university_logo.sh overwrites it with your real RGB565 image.

tools/convert_gui_logo.py
    JPG/PNG -> LVGL v9 RGB565 C converter.

utilities/prepare_university_logo.sh
    One-command logo preparation.

Important
---------
The JPG itself is NOT read from SD card or decoded at runtime. After running
prepare_university_logo.sh, the generated RGB565 data becomes part of the
normal application firmware and is flashed together with the rest of src_p4.
