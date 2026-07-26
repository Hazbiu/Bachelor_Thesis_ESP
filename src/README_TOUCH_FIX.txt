LVGL TOUCH + WATCHDOG FIX
==========================

The serial log proves GT911 is detected and registered. The launcher freezes
before touch processing because its startup overlaps LVGL's first refresh.

This fix:
  - does not force a dummy-draw false transition at boot
  - waits for the LVGL worker to settle
  - retries the BSP LVGL mutex with esp_err_t semantics
  - gets the touch device from bsp_display_get_input_dev()
  - starts on LV_EVENT_PRESSED
  - destroys the launcher before enabling camera dummy draw

Replace:
  main/app/app_main.c
  main/features/gui_screens/app_ui.c
  main/features/gui_screens/app_ui.h
  main/CMakeLists.txt

Build:
  cd ~/Programming/Bachelor_Thesis_ESP/src
  rm -rf build
  idf.py reconfigure
  idf.py build
  idf.py -p /dev/ttyACM0 flash monitor

Expected startup:
  BSP touchscreen ready: type=1

Expected touch:
  Launcher touch accepted; requesting camera application start
  Launcher start callback received

There should be no task-watchdog trace in lv_inv_area.
