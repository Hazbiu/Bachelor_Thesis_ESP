#include "app/activity/app_activity.h"

#include "services/power/power_manager.h"

void app_activity_notify_user(void)
{
    power_manager_notify_activity();
}
