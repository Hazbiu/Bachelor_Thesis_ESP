ESP32-P4 GUI + PIN POWER POLICY UPDATE (V2)
===========================================

This update is intended to be extracted over:
  /home/admin/Programming/Bachelor_Thesis_ESP

WHAT CHANGED
------------
Launcher
- University logo area is much larger and centered across the launcher.
- Logo remains native RGB565 in flash (no runtime JPEG decoding).
- Heading changed to: AI-Face Recognition Application
- Heading is larger/bold; subtitle is smaller below it.
- Removed SYSTEM READY, Touch ready and the launcher GPIO3 hint.
- Removed large shadows/gradients and uses flat opaque surfaces.

Settings
- Removed card shadows and disabled the scroll bar drawing.
- Light Sleep and Deep Sleep keep only their title + toggle; explanatory text removed.

PIN screen
- Follows the saved system-wide Light/Dark theme.
- Removed gradient and large shadows; uses flat opaque surfaces.
- Camera remains stopped while the PIN screen is active.
- Automatic Light/Deep inactivity transitions are paused for the entire PIN screen.
- GPIO3/manual Deep-sleep requests are also blocked while PIN owns the screen.
- The inactivity timer restarts from 0 only after the camera stream has successfully
  restarted and the application has returned to CAMERA_ACTIVE.

Light-sleep display power
- Physical LCD backlight is no longer intentionally kept ON during Light-sleep.
- MIPI-DSI/LVGL/camera suspend behavior remains in the existing power flow.

INSTALL
-------
1. Extract this ZIP into the repository root:

   cd /home/admin/Programming/Bachelor_Thesis_ESP
   unzip -o ~/Downloads/ESP32P4_GUI_Power_v2.zip

2. IMPORTANT: regenerate the university logo once. The converter now allows a
   substantially larger logo (up to 320 x 160 while preserving aspect ratio):

   bash utilities/prepare_university_logo.sh

   Your existing JPG is expected at:
     src_p4/main/ui/assets/university_logo.jpg

   This ZIP intentionally does NOT contain or overwrite university_logo.jpg.

3. Build and flash with the same existing workflow:

   bash utilities/esp_AI_flashing.sh dl /dev/ttyACM0

4. Optional serial monitor after flashing:

   cd /home/admin/Programming/Bachelor_Thesis_ESP/src_p4
   source /home/admin/esp/esp-idf-v5.5.4/export.sh >/dev/null
   idf.py -p /dev/ttyACM0 monitor

NOTES
-----
- Pillow is only used on the PC to convert the JPG; no image decoder runs on P4.
- If Pillow is missing:
    python3 -m pip install Pillow
- If you replace university_logo.jpg later, rerun prepare_university_logo.sh before flashing.
