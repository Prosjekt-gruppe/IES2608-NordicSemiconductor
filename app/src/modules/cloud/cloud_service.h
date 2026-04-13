#pragma once

#include <stdbool.h>

int cloud_service_init(void);
int cloud_service_connect_async(void); 
int cloud_service_disconnect(void); 

bool cloud_service_is_connected(void); 