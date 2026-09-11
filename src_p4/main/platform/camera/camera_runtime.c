#include "platform/camera/camera_runtime.h"

#include "bsp/esp-bsp.h"
#include "platform/camera/video_capture.h"


esp_err_t camera_runtime_initialize(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    return app_video_main(bus);
}
