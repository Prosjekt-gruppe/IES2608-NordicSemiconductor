#pragma once

#include <stdbool.h>

int lte_service_init(void);
int lte_service_connect_async(void);
int lte_service_disconnect(void);
bool lte_service_is_connected(void);