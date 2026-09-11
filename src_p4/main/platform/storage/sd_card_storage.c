#include "platform/storage/sd_card_storage.h"

#include "bsp/esp-bsp.h"


esp_err_t sd_card_storage_ensure_mounted(void)
{
    return bsp_sdcard != NULL ? ESP_OK : bsp_sdcard_mount();
}
