/*
 * Merged prototype: "Intelligent Modem" + nRF Cloud CoAP A-GNSS / Cellular Location
 *
 * Features:
 *  - LTE-M with signal-quality monitoring (RSRP / SNR trend detection)
 *  - Periodic GNSS tracking
 *  - nRF Cloud CoAP: A-GNSS assistance + cloud-based cellular location
 *  - UDP socket echo (button-triggered GPS data send)
 *  - PSM / eDRX power saving
 */

#include <stdio.h>
#include <ncs_version.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <dk_buttons_and_leds.h>
#include <date_time.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/location.h>
#include <nrf_modem_gnss.h>
#include <nrf_modem_at.h>
#include <limits.h>
#include <string.h>
#include <net/nrf_cloud_coap.h>
#include <app_version.h>

LOG_MODULE_REGISTER(ArneTracking, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* UDP echo server settings                                           */
/* ------------------------------------------------------------------ */
#define SERVER_HOSTNAME "udp-echo.nordicsemi.academy"
#define SERVER_PORT "2444"

#define MESSAGE_SIZE 256
#define MESSAGE_TO_SEND "Hello"

/* ------------------------------------------------------------------ */
/* GNSS state                                                         */
/* ------------------------------------------------------------------ */
static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static int64_t gnss_start_time;
static bool first_fix = false;
static uint8_t gps_data[MESSAGE_SIZE];

/* ------------------------------------------------------------------ */
/* UDP socket state                                                   */
/* ------------------------------------------------------------------ */
static int sock;
static struct sockaddr_storage server;
static uint8_t recv_buf[MESSAGE_SIZE];

/* ------------------------------------------------------------------ */
/* Signal-quality monitoring                                          */
/* ------------------------------------------------------------------ */
static struct k_work_delayable sig_work;
static atomic_t rrc_connected;
static atomic_t modem_info_ready;

static int last_rsrp_dbm = INT32_MIN;
static int last_snr_db   = INT32_MIN;

#define SIG_HIST_LEN 6  /* 6 samples = 60 s if you poll every 10 s */
static int rsrp_hist[SIG_HIST_LEN];
static int snr_hist[SIG_HIST_LEN];
static uint8_t hist_idx;
static uint8_t hist_count;

/* ------------------------------------------------------------------ */
/* Semaphores                                                         */
/* ------------------------------------------------------------------ */
static K_SEM_DEFINE(lte_connected, 0, 1);
static K_SEM_DEFINE(time_update_finished, 0, 1);
static K_SEM_DEFINE(location_event, 0, 1);

/* ------------------------------------------------------------------ */
/* nRF Cloud cellular-location configuration                          */
/* ------------------------------------------------------------------ */
static struct nrf_cloud_location_config cloud_loc_config = {
	.hi_conf  = IS_ENABLED(CONFIG_COAP_CELL_DEFAULT_HICONF_VAL),
	.fallback = IS_ENABLED(CONFIG_COAP_CELL_DEFAULT_FALLBACK_VAL),
	.do_reply = IS_ENABLED(CONFIG_COAP_CELL_DEFAULT_DOREPLY_VAL),
};

static int server_resolve(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM
	};

	err = getaddrinfo(SERVER_HOSTNAME, SERVER_PORT, &hints, &result);
	if (err != 0) {
		LOG_INF("ERROR: getaddrinfo failed %d", err);
		return -EIO;
	}

	if (result == NULL) {
		LOG_INF("ERROR: Address not found");
		return -ENOENT;
	}

	struct sockaddr_in *server4 = ((struct sockaddr_in *)&server);

	server4->sin_addr.s_addr =
		((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	server4->sin_family = AF_INET;
	server4->sin_port = ((struct sockaddr_in *)result->ai_addr)->sin_port;

	char ipv4_addr[NET_IPV4_ADDR_LEN];
	inet_ntop(AF_INET, &server4->sin_addr.s_addr, ipv4_addr,
		  sizeof(ipv4_addr));
	LOG_INF("IPv4 Address found %s", ipv4_addr);

	freeaddrinfo(result);

	return 0;
}

static int server_connect(void)
{
	int err;
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Failed to create socket: %d.", errno);
		return -errno;
	}

	err = connect(sock, (struct sockaddr *)&server,
		      sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Connect failed : %d", errno);
		return -errno;
	}
	LOG_INF("Successfully connected to server");

	return 0;
}

/* ------------------------------------------------------------------ */
/* Date-time handler (needed for JWT auth with nRF Cloud)             */
/* ------------------------------------------------------------------ */
static void date_time_evt_handler(const struct date_time_evt *evt)
{
	k_sem_give(&time_update_finished);
}

/* ------------------------------------------------------------------ */
/* Cloud cellular-location helpers (from AGNSS)                       */
/* ------------------------------------------------------------------ */
static void print_cloud_location(double latitude, double longitude, uint32_t accuracy)
{
	LOG_INF("Cloud Lat: %f, Lon: %f, Uncertainty: %u m", latitude, longitude, accuracy);
	LOG_INF("Google maps URL: https://maps.google.com/?q=%.06f,%.06f", latitude, longitude);
}

static void handle_cloud_location_request(const struct lte_lc_cells_info *cell_info)
{
	int err = 0;
	struct nrf_cloud_location_result cell_pos_result = {0};
	const struct nrf_cloud_rest_location_request cell_pos_req = {
		.config    = &cloud_loc_config,
		.cell_info = (struct lte_lc_cells_info *)cell_info,
	};

	err = nrf_cloud_coap_location_get(&cell_pos_req, &cell_pos_result);
	if (err) {
		LOG_ERR("Cloud location request failed, error: %d", err);
		if (cell_pos_result.err != NRF_CLOUD_ERROR_NONE) {
			LOG_ERR("nRF Cloud error code: %d", cell_pos_result.err);
		}
		return;
	}

	LOG_INF("Cellular location fulfilled with %s",
		cell_pos_result.type == LOCATION_TYPE_SINGLE_CELL  ? "single-cell"
		: cell_pos_result.type == LOCATION_TYPE_MULTI_CELL ? "multi-cell"
								   : "unknown");

	if (cloud_loc_config.do_reply) {
		print_cloud_location(cell_pos_result.lat, cell_pos_result.lon,
				     cell_pos_result.unc);
	} else {
		LOG_INF("Result of location request only stored in nRF Cloud.");
	}
}

/* ------------------------------------------------------------------ */
/* Location library event handler                                     */
/* ------------------------------------------------------------------ */
static void location_event_handler(const struct location_event_data *event_data)
{
	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		LOG_INF("Got location from location library");
		print_cloud_location((double)event_data->location.latitude,
				     (double)event_data->location.longitude,
				     (uint32_t)event_data->location.accuracy);
		break;
	case LOCATION_EVT_TIMEOUT:
		LOG_WRN("Getting location timed out");
		break;
	case LOCATION_EVT_ERROR:
		LOG_ERR("Getting location failed");
		break;
	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
		LOG_INF("A-GNSS assistance data requested");
		break;
	case LOCATION_EVT_GNSS_PREDICTION_REQUEST:
		LOG_INF("P-GPS prediction data requested");
		break;
	case LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST:
		LOG_INF("Cloud location request received from location library");
		handle_cloud_location_request(event_data->cloud_location_request.cell_data);
		break;
	default:
		LOG_ERR("Unknown location event: %d", event_data->id);
		break;
	}

	k_sem_give(&location_event);
}

static void location_event_wait(void)
{
	k_sem_take(&location_event, K_FOREVER);
}

/**
 * @brief Request a location fix using default configuration.
 */
static void location_default_get(void)
{
	int err;

	LOG_INF("Requesting cloud-assisted location...");
	err = location_request(NULL);
	if (err) {
		LOG_ERR("Requesting location failed, error: %d", err);
		return;
	}

	location_event_wait();
}

/* ------------------------------------------------------------------ */
/* LTE event handler (merged: Prt1 signal monitoring + cloud events)  */
/* ------------------------------------------------------------------ */
static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
			(evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}
		LOG_INF("Network registration status: %s",
				evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
				"Connected - home network" : "Connected - roaming");
		k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
			atomic_set(&rrc_connected, 1);
			LOG_INF("RRC connection status: Connected");

			k_work_schedule(&sig_work, K_SECONDS(10));
			}
		else {
			atomic_set(&rrc_connected, 0);
			LOG_INF("RRC connection status: Idle");

			k_work_cancel_delayable(&sig_work);
		}
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_INF("PSM parameter update: Periodic TAU: %d s, Active time: %d s",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		if (evt->psm_cfg.active_time == -1){
			LOG_ERR("Network rejected PSM parameters. Failed to enable PSM");
		}
		break;
	case LTE_LC_EVT_EDRX_UPDATE:
		LOG_INF("eDRX parameter update: eDRX: %f, PTW: %f",
			(double)evt->edrx_cfg.edrx, (double)evt->edrx_cfg.ptw);
		break;
	default:
		break;
	}
}

static void sig_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!atomic_get(&rrc_connected) || !atomic_get(&modem_info_ready)) {
        return;
    }

    int rsrp, snr;

    if (modem_info_get_rsrp(&rsrp) != 0 || modem_info_get_snr(&snr) != 0) {
        /* Try again later while connected */
        k_work_schedule(&sig_work, K_SECONDS(10));
        return;
    }

    /* Print current */
    LOG_INF("RSRP: %d dBm, SNR: %d dB", rsrp, snr);

    /* Compare to previous (simple delta) */
    if (last_rsrp_dbm != INT32_MIN) {
        int drsrp = rsrp - last_rsrp_dbm; /* negative means worse */
        int dsnr  = snr  - last_snr_db;

        if (drsrp <= -5) {
            LOG_WRN("RSRP dropped %d dB (from %d to %d)", -drsrp, last_rsrp_dbm, rsrp);
        }
        if (dsnr <= -3) {
            LOG_WRN("SNR dropped %d dB (from %d to %d)", -dsnr, last_snr_db, snr);
        }
    }

    /* Save “last” */
    last_rsrp_dbm = rsrp;
    last_snr_db   = snr;

    /* Save history for trend */
    rsrp_hist[hist_idx] = rsrp;
    snr_hist[hist_idx]  = snr;
    hist_idx = (hist_idx + 1) % SIG_HIST_LEN;
    if (hist_count < SIG_HIST_LEN) {
        hist_count++;
    }

    /* Optional: detect steady degradation over last 3 points */
    if (hist_count >= 3) {
        /* Look at last 3 samples (most recent is at idx-1) */
        int i2 = (hist_idx + SIG_HIST_LEN - 1) % SIG_HIST_LEN;
        int i1 = (hist_idx + SIG_HIST_LEN - 2) % SIG_HIST_LEN;
        int i0 = (hist_idx + SIG_HIST_LEN - 3) % SIG_HIST_LEN;

        bool rsrp_worsening = (rsrp_hist[i2] < rsrp_hist[i1]) && (rsrp_hist[i1] < rsrp_hist[i0]);
        bool snr_worsening  = (snr_hist[i2]  < snr_hist[i1])  && (snr_hist[i1]  < snr_hist[i0]);

        if (rsrp_worsening) {
            LOG_WRN("RSRP trend worsening: %d -> %d -> %d dBm",
                    rsrp_hist[i0], rsrp_hist[i1], rsrp_hist[i2]);
        }
        if (snr_worsening) {
            LOG_WRN("SNR trend worsening: %d -> %d -> %d dB",
                    snr_hist[i0], snr_hist[i1], snr_hist[i2]);
        }
    }

    k_work_schedule(&sig_work, K_SECONDS(10));
}

static int modem_configure(void)
{
	int err;

	k_work_init_delayable(&sig_work, sig_work_fn);
	atomic_clear(&rrc_connected);
	atomic_clear(&modem_info_ready);

	LOG_INF("Initializing modem library");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);
		return err;
	}

	/* Register date-time handler early so we don't miss the first event */
	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		date_time_register_handler(date_time_evt_handler);
	}
	
	err = lte_lc_psm_req(true);
	if (err) {
		LOG_ERR("lte_lc_psm_req, error: %d", err);
	}

	err = lte_lc_edrx_req(false); /* Enable when you want eDRX; does not work with GNSS for now */
	if (err) {
		LOG_ERR("lte_lc_edrx_req, error: %d", err);
	}

	err = lte_lc_system_mode_set(
		LTE_LC_SYSTEM_MODE_LTEM_GPS,
		LTE_LC_SYSTEM_MODE_PREFER_AUTO
	);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set, error: %d", err);
	}
	
	LOG_INF("Connecting to LTE network");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("Error in lte_lc_connect_async, error: %d", err);
		return err;
	}

	k_sem_take(&lte_connected, K_FOREVER);
	LOG_INF("Connected to LTE network");
	dk_set_led_on(DK_LED2);

	err = modem_info_init();
	if (err) {
		LOG_ERR("Failed to initialize modem info library, error: %d", err);
	} else {
		atomic_set(&modem_info_ready, 1);
	}

	/* Wait for valid date/time (required for JWT auth with nRF Cloud) */
	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		LOG_INF("Waiting for current time...");
		k_sem_take(&time_update_finished, K_MINUTES(10));
		if (!date_time_is_valid()) {
			LOG_WRN("Failed to get current time. Continuing anyway.");
		}
	}

	return 0;
}

static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	LOG_INF("Latitude:       %.06f", pvt_data->latitude);
	LOG_INF("Longitude:      %.06f", pvt_data->longitude);
	LOG_INF("Altitude:       %.01f m", (double)pvt_data->altitude);
	LOG_INF("Time (UTC):     %02u:%02u:%02u.%03u",
	       pvt_data->datetime.hour,
	       pvt_data->datetime.minute,
	       pvt_data->datetime.seconds,
	       pvt_data->datetime.ms);

	int err = snprintf(gps_data, MESSAGE_SIZE, "Latitude: %.06f, Longitude: %.06f", pvt_data->latitude, pvt_data->longitude);
	if (err < 0) {
		LOG_ERR("Failed to print to buffer: %d", err);
	}
}

static void gnss_event_handler(int event)
{
	int err, num_satellites;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);
		if (err) {
			LOG_ERR("nrf_modem_gnss_read failed, err %d", err);
			return;
		}
		num_satellites = 0;
		for (int i = 0; i < 12 ; i++) {
			if (pvt_data.sv[i].signal != 0) {
				num_satellites++;
			}
		}
		LOG_INF("Searching. Current satellites: %d", num_satellites);
		if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
			dk_set_led_on(DK_LED1);
			print_fix_data(&pvt_data);
			if (!first_fix) {
				LOG_INF("Time to first fix: %lld s", (k_uptime_get() - gnss_start_time)/1000);
				first_fix = true;
			}
			return;
		}
		if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) {
			LOG_INF("GNSS blocked by LTE activity");
		} else if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
			LOG_INF("Insufficient GNSS time window");
		}
		break;

	case NRF_MODEM_GNSS_EVT_PERIODIC_WAKEUP:
		LOG_INF("GNSS has woken up");
		break;
	case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_FIX:
		LOG_INF("GNSS enter sleep after fix");
		break;
	default:
		break;
	}
}

static int gnss_init_and_start(void)
{

	if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL) != 0) {
		LOG_ERR("Failed to activate GNSS functional mode");
		return -1;
	}

	if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
		LOG_ERR("Failed to set GNSS event handler");
		return -1;
	}

	if (nrf_modem_gnss_fix_interval_set(CONFIG_GNSS_PERIODIC_INTERVAL) != 0) {
		LOG_ERR("Failed to set GNSS fix interval");
		return -1;
	}

	if (nrf_modem_gnss_fix_retry_set(CONFIG_GNSS_PERIODIC_TIMEOUT) != 0) {
		LOG_ERR("Failed to set GNSS fix retry");
		return -1;
	}

	LOG_INF("Starting GNSS");
	if (nrf_modem_gnss_start() != 0) {
		LOG_ERR("Failed to start GNSS");
		return -1;
	}

	gnss_start_time = k_uptime_get();

	return 0;
}

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	switch (has_changed) {
	case DK_BTN1_MSK:
		if (button_state & DK_BTN1_MSK){
			int err = send(sock, gps_data, strlen((char *)gps_data), 0);
			if (err < 0) {
				LOG_INF("Failed to send message, %d", errno);
				return;
			}
		}
		break;
	}
}

int main(void)
{
	int err;
	int received;

	LOG_INF("ArneTracking Prototype, version: %s", APP_VERSION_STRING);

	if (dk_leds_init() != 0) {
		LOG_ERR("Failed to initialize the LED library");
	}

	err = modem_configure();
	if (err) {
		LOG_ERR("Failed to configure the modem");
		return 0;
	}

	if (dk_buttons_init(button_handler) != 0) {
		LOG_ERR("Failed to initialize the buttons library");
	}

	/* ---- nRF Cloud CoAP setup ---- */
	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("Failed to initialize nRF Cloud CoAP client: %d", err);
		return 0;
	}
	err = nrf_cloud_coap_connect(NULL);
	if (err) {
		LOG_ERR("Failed to connect to nRF Cloud: %d", err);
		return 0;
	}
	LOG_INF("Connected to nRF Cloud via CoAP");

	/* ---- Location library setup ---- */
	err = location_init(location_event_handler);
	if (err) {
		LOG_ERR("Failed to initialize the Location library, error: %d", err);
		return 0;
	}

	/* Get an initial cloud-assisted cellular location fix */
	location_default_get();

	/* ---- UDP echo server ---- */
	if (server_resolve() != 0) {
		LOG_INF("Failed to resolve server name");
		return 0;
	}

	if (server_connect() != 0) {
		LOG_INF("Failed to initialize client");
		return 0;
	}

	/* ---- GNSS periodic tracking ---- */
	if (gnss_init_and_start() != 0) {
		LOG_ERR("Failed to initialize and start GNSS");
		return 0;
	}

	/* ---- Main loop: receive UDP echo data ---- */
	while (1) {
		received = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);

		if (received < 0) {
			LOG_ERR("Socket error: %d, exit", errno);
			break;
		} else if (received == 0) {
			break;
		}

		recv_buf[received] = 0;
		LOG_INF("Data received from the server: (%s)", recv_buf);
	}

	(void)close(sock);

	return 0;
}