ESP32-P4 RESTORE TO GUI/POWER V2

This package contains the exact V2 GUI/power files plus a rollback helper.

After extracting at the repository root, run:
  bash utilities/restore_v2_exact.sh

Then clean and flash:
  rm -rf src_p4/build
  bash utilities/prepare_university_logo.sh
  bash utilities/esp_AI_flashing.sh dl /dev/ttyACM0

The rollback helper only undoes the later V3 GT911 BSP hotfix if its backup exists.
