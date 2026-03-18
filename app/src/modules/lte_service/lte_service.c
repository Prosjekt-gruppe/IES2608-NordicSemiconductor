/*
 * LTE_Service : LTE-specific behavior on top of the modem
 */ 


 #include "lte_service.h"
 #include "app_events.h"

 #include <modem/lte_lc.h>
 #include <nrf_modem_at.h>
 #include <zephyr/kernel.h>
 #include <zephyr/logging/log.h>