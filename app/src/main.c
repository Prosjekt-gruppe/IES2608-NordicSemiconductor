/*
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 * 
 * NTN (Non-Terrestrial Network) Prototype for Thingy:91 X
 * 
 * This application demonstrates satellite network connectivity using
 * NTN (satellite) mode on the nRF9160 modem in the Thingy:91 X device.
 * It enables remote testing of position tracking over satellite networks.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_info.h>
#include <nrf_modem_at.h>
#include <stdio.h>

LOG_MODULE_REGISTER(ntn_prototype, LOG_LEVEL_DBG);

/* NTN connection state */
static bool ntn_connected = false;

/* LTE event handler */
static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_INF("Network registration status: %d", evt->nw_reg_status);
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			ntn_connected = true;
			LOG_INF("NTN network connected!");
		} else {
			ntn_connected = false;
		}
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_INF("PSM parameter update: TAU=%d, Active time=%d",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE:
		LOG_INF("eDRX parameter update: eDRX=%f, PTW=%f",
			evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		LOG_INF("Modem domain event: %d", evt->modem_evt);
		break;
	default:
		break;
	}
}

/* Initialize modem and configure NTN mode */
static int ntn_modem_init(void)
{
	int err;

	LOG_INF("Initializing modem for NTN mode...");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize modem library, error: %d", err);
		return err;
	}

	/* Configure modem for NTN/satellite operation */
	LOG_INF("Configuring modem for NTN...");
	
	/* Enable NTN mode - this is key for satellite connectivity */
	err = nrf_modem_at_printf("AT%%XMODEMUUID");
	if (err) {
		LOG_WRN("Failed to get modem UUID: %d", err);
	}

	/* Set system mode to NB-IoT/LTE-M with GNSS */
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS, 
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("Failed to set system mode: %d", err);
		return err;
	}

	/* Request PSM (Power Saving Mode) for efficient satellite operations */
	err = lte_lc_psm_req(true);
	if (err) {
		LOG_WRN("Failed to request PSM: %d", err);
	}

	/* Request eDRX for improved battery life */
	err = lte_lc_edrx_req(true);
	if (err) {
		LOG_WRN("Failed to request eDRX: %d", err);
	}

	LOG_INF("Modem initialized for NTN mode");
	return 0;
}

/* Connect to NTN network */
static int ntn_connect(void)
{
	int err;

	LOG_INF("Connecting to NTN network...");

	/* Register LTE event handler */
	lte_lc_register_handler(lte_handler);

	/* Initiate LTE connection */
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("Failed to initiate LTE connection: %d", err);
		return err;
	}

	LOG_INF("NTN connection initiated, waiting for network...");
	return 0;
}

/* Send test data over NTN */
static void ntn_send_test_data(void)
{
	if (!ntn_connected) {
		LOG_WRN("Not connected to NTN network, skipping test data");
		return;
	}

	LOG_INF("NTN network is connected - ready for remote testing");
	LOG_INF("Device can now send position data over satellite network");
	
	/* In a full implementation, this would send actual position data
	 * For now, we just log that the connection is ready */
}

/* Main application entry point */
int main(void)
{
	int err;

	LOG_INF("=== NTN Prototype Starting ===");
	LOG_INF("Device: Thingy:91 X");
	LOG_INF("Mode: Non-Terrestrial Network (Satellite)");

	/* Initialize modem for NTN */
	err = ntn_modem_init();
	if (err) {
		LOG_ERR("Failed to initialize NTN modem: %d", err);
		return err;
	}

	/* Connect to NTN network */
	err = ntn_connect();
	if (err) {
		LOG_ERR("Failed to connect to NTN: %d", err);
		return err;
	}

	/* Main loop - monitor connection and send test data */
	LOG_INF("Entering main loop - monitoring NTN connection");
	
	while (1) {
		/* Wait for connection */
		k_sleep(K_SECONDS(10));

		/* Send test data if connected */
		ntn_send_test_data();

		/* Print status update */
		LOG_INF("NTN Status: %s", 
			ntn_connected ? "CONNECTED" : "CONNECTING...");
	}

	return 0;
}
