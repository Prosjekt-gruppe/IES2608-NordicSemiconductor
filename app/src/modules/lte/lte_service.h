#pragma once

#include <stdbool.h>

int lte_service_init(void); 
int lte_service_connect_async(void); 
int lte_serivce_disconnect(void);
int lte_serivce_get_rsrp(int *rsrp_dbm); 
bool lte_service_is_connected(void); 



