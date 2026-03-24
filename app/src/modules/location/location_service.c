#include "location_service.h"
#include "app_events.h"

#include <modem/location.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/syscalls/util.h>

LOG_MODULE_REGISTER(location_service, LOG_LEVEL_INF); 