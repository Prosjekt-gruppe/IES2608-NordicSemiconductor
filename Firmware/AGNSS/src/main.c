/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/location.h>
#include <modem/nrf_modem_lib.h>
#include <date_time.h>
#include <zephyr/logging/log.h>
#include <net/nrf_cloud_coap.h>
#include <app_version.h>
#include <zephyr/smf.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);
static void print_location(double latitude, double longitude, uint32_t accuracy);
static void handle_cloud_location_request(const struct lte_lc_cells_info *cell_info);
static void print_location(double latitude, double longitude, uint32_t accuracy)
{
	LOG_INF("Lat: %f, Lon: %f, Uncertainty: %u m", latitude, longitude, accuracy);
	LOG_INF("Google maps URL: https://maps.google.com/?q=%.06f,%.06f",
		latitude, longitude);
}
// 1. Define States 
enum app_state {
	STATE_BOOT, 
	STATE_WAIT_LTE,
	STATE_WAIT_TIME,
	STATE_LOCATION_INIT,
	STATE_COAP_INIT,
	STATE_COAP_CONNECT,
	STATE_LOCATION_REQUEST,
	STATE_WAIT_LOCATION,
	STATE_DONE,
	STATE_ERROR,
};


// 2. Define event types 
enum app_evt_type {
	EVT_NONE,
	EVT_LTE_CONNECTED,
	EVT_TIME_READY,
	EVT_LOCATION_READY,
	EVT_LOCATION_TIMEOUT,
	EVT_LOCATION_ERROR,
	EVT_CLOUD_LOC_READY,
	EVT_CLOUD_LOC_ERROR,
};

// 3. Define SMF context object
struct app_ctx {
	struct smf_ctx ctx; 

	enum app_evt_type evt; 
	int err; 

	struct nrf_cloud_location_config config;	

	double lat; 
	double lon; 
	uint32_t unc; 
}; 


// 4. Global app object - structure to hold configuration flags. 
static struct app_ctx app = {
	.config = {
		.hi_conf = IS_ENABLED(CONFIG_COAP_CELL_DEFAULT_HICONF_VAL),
		.fallback = IS_ENABLED(CONFIG_COAP_CELL_DEFAULT_FALLBACK_VAL),
		.do_reply = IS_ENABLED(CONFIG_COAP_CELL_DEFAULT_DOREPLY_VAL),
	},
};

// 5. SMF Events

/* Date/time Event - decide when date/time is ready */

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	ARG_UNUSED(evt); 
	app.evt = EVT_TIME_READY; 
}

/* LTE Event - decide when lte is connected*/
static void lte_event_handler(const struct lte_lc_evt *const evt)
{
		if (evt->type == LTE_LC_EVT_NW_REG_STATUS){
			if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
				(evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
				LOG_INF("Connected to network");
				app.evt = EVT_LTE_CONNECTED;
		}
	}
}

/* Location Event - */
static void location_event_handler(const struct location_event_data *event_data)
{
	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		LOG_INF("Got location from location library");
		app.lat = (double)event_data->location.latitude;
		app.lon = (double)event_data->location.longitude; 
		app.unc = (uint32_t)event_data->location.accuracy;
		app.evt = EVT_LOCATION_READY;
		break;

	case LOCATION_EVT_TIMEOUT:
		LOG_DBG("Getting location timed out");
		app.evt = EVT_LOCATION_TIMEOUT;
		break;

	case LOCATION_EVT_ERROR:
		LOG_DBG("Getting location failed");
		app.evt = EVT_LOCATION_ERROR;
		break;

	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
		LOG_DBG("Getting location assistance requested (A-GNSS)");
		break;

	case LOCATION_EVT_GNSS_PREDICTION_REQUEST:
		LOG_DBG("Getting location assistance requested (P-GPS)");
		break;

	case LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST:
		LOG_DBG("Cloud location request received from location library");
		handle_cloud_location_request(event_data->cloud_location_request.cell_data);
		break;

	default:
		LOG_ERR("Getting location: Unknown event");
		app.evt = EVT_LOCATION_ERROR;
		break;
	}
}



// 6. Define state function prototypes 
static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void wait_lte_entry(void *obj);
static enum smf_state_result wait_lte_run(void *obj);

static void wait_time_entry(void *obj);
static enum smf_state_result wait_time_run(void *obj);

static void location_init_entry(void *obj);
static enum smf_state_result location_init_run(void *obj);

static void coap_init_entry(void *obj);
static enum smf_state_result coap_init_run(void *obj);

static void coap_connect_entry(void *obj);
static enum smf_state_result coap_connect_run(void *obj);

static void location_request_entry(void *obj);
static enum smf_state_result location_request_run(void *obj);

static enum smf_state_result wait_location_run(void *obj);

static void done_entry(void *obj);
static void error_entry(void *obj);


// 7. State table 
static const struct smf_state app_states[] = {
	[STATE_BOOT]             = SMF_CREATE_STATE(boot_entry, boot_run, NULL, NULL, NULL),
	[STATE_WAIT_LTE]         = SMF_CREATE_STATE(wait_lte_entry, wait_lte_run, NULL, NULL, NULL),
	[STATE_WAIT_TIME]        = SMF_CREATE_STATE(wait_time_entry, wait_time_run, NULL, NULL, NULL),
	[STATE_LOCATION_INIT]    = SMF_CREATE_STATE(location_init_entry, location_init_run, NULL, NULL, NULL),
	[STATE_COAP_INIT]        = SMF_CREATE_STATE(coap_init_entry, coap_init_run, NULL, NULL, NULL),
	[STATE_COAP_CONNECT]     = SMF_CREATE_STATE(coap_connect_entry, coap_connect_run, NULL, NULL, NULL),
	[STATE_LOCATION_REQUEST] = SMF_CREATE_STATE(location_request_entry, location_request_run, NULL, NULL, NULL),
	[STATE_WAIT_LOCATION]    = SMF_CREATE_STATE(NULL, wait_location_run, NULL, NULL, NULL),
	[STATE_DONE]             = SMF_CREATE_STATE(done_entry, NULL, NULL, NULL, NULL),
	[STATE_ERROR]            = SMF_CREATE_STATE(error_entry, NULL, NULL, NULL, NULL),
};


// 8. BOOT
static void boot_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	LOG_INF("nRF Cloud CoAP Cellular Location Sample, version: %s",
		APP_VERSION_STRING);

	ctx->evt = EVT_NONE;
	ctx->err = nrf_modem_lib_init();
	if (ctx->err) {
		LOG_ERR("Modem library initialization failed, error: %d", ctx->err);
		return;
	}

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		date_time_register_handler(date_time_evt_handler);
	}

	lte_lc_register_handler(lte_event_handler);
}

static enum smf_state_result boot_run(void *obj)
{
	struct app_ctx *ctx = obj;

	if (ctx->err) {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
	} else {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_WAIT_LTE]);
	}

	return SMF_EVENT_HANDLED;
}

// 9. WAIT_LTE
static void wait_lte_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	LOG_INF("Connecting to LTE...");
	ctx->evt = EVT_NONE;
	ctx->err = lte_lc_connect();
	if (ctx->err) {
		LOG_ERR("lte_lc_connect failed: %d", ctx->err);
	}
}
static enum smf_state_result wait_lte_run(void *obj)
{
	struct app_ctx *ctx = obj;

	if (ctx->err) {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
		return SMF_EVENT_HANDLED;
	}

	if (ctx->evt == EVT_LTE_CONNECTED) {
		ctx->evt = EVT_NONE;

		if (IS_ENABLED(CONFIG_DATE_TIME)) {
			smf_set_state(SMF_CTX(ctx), &app_states[STATE_WAIT_TIME]);
		} else {
			smf_set_state(SMF_CTX(ctx), &app_states[STATE_LOCATION_INIT]);
		}
	}

	return SMF_EVENT_HANDLED;
}

// 10. WAIT_TIME
static void wait_time_entry(void *obj)
{
	struct app_ctx *ctx = obj;
	ARG_UNUSED(ctx);

	LOG_INF("Waiting for current time");
}

static enum smf_state_result wait_time_run(void *obj)
{
	struct app_ctx *ctx = obj;

	if (ctx->evt == EVT_TIME_READY) {
		ctx->evt = EVT_NONE;

		if (!date_time_is_valid()) {
			LOG_ERR("Failed to get current time. Continuing anyway.");
		}

		smf_set_state(SMF_CTX(ctx), &app_states[STATE_LOCATION_INIT]);
	}

	return SMF_EVENT_HANDLED;
}

// 11. LOCATION INIT
static void location_init_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	ctx->err = location_init(location_event_handler);
	if (ctx->err) {
		LOG_ERR("Initializing the Location library failed, error: %d", ctx->err);
	}
}

static enum smf_state_result location_init_run(void *obj)
{
	struct app_ctx *ctx = obj;

	if (ctx->err) {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
	} else {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_COAP_INIT]);
	}

	return SMF_EVENT_HANDLED;
}


// 12. COAP INIT & COAP CONNECT
static void coap_init_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	ctx->err = nrf_cloud_coap_init();
	if (ctx->err) {
		LOG_ERR("Failed to initialize CoAP client: %d", ctx->err);
	}
}

static enum smf_state_result coap_init_run(void *obj)
{
	struct app_ctx *ctx = obj;

	if (ctx->err) {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
	} else {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_COAP_CONNECT]);
	}

	return SMF_EVENT_HANDLED;
}

static void coap_connect_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	ctx->err = nrf_cloud_coap_connect(NULL);
	if (ctx->err) {
		LOG_ERR("Connecting to nRF Cloud failed, error: %d", ctx->err);
	}
}

static enum smf_state_result coap_connect_run(void *obj)
{
	struct app_ctx *ctx = obj;

	if (ctx->err) {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
	} else {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_LOCATION_REQUEST]);
	}

	return SMF_EVENT_HANDLED;
}


// 13. LOCATION REQUEST

static void location_request_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	LOG_INF("Requesting location with the default configuration...");

	ctx->evt = EVT_NONE;
	ctx->err = location_request(NULL);
	if (ctx->err) {
		LOG_ERR("Requesting location failed, error: %d", ctx->err);
	}
}

static enum smf_state_result location_request_run(void *obj)
{
	struct app_ctx *ctx = obj;

	if (ctx->err) {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
	} else {
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_WAIT_LOCATION]);
	}

	return SMF_EVENT_HANDLED;
}

// 14. WAIT LOCATION
static enum smf_state_result wait_location_run(void *obj)
{
	struct app_ctx *ctx = obj;

	switch (ctx->evt) {
	case EVT_LOCATION_READY:
	case EVT_CLOUD_LOC_READY:
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_DONE]);
		break;

	case EVT_LOCATION_TIMEOUT:
		ctx->err = -ETIMEDOUT;
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
		break;

	case EVT_LOCATION_ERROR:
	case EVT_CLOUD_LOC_ERROR:
		if (ctx->err == 0) {
			ctx->err = -EIO;
		}
		smf_set_state(SMF_CTX(ctx), &app_states[STATE_ERROR]);
		break;

	default:
		break;
	}

	return SMF_EVENT_HANDLED;
}

// 15. DONE & ERROR
static void done_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	print_location(ctx->lat, ctx->lon, ctx->unc);
	smf_set_terminate(SMF_CTX(ctx), 0);
}

static void error_entry(void *obj)
{
	struct app_ctx *ctx = obj;

	LOG_ERR("Application failed, err=%d", ctx->err);
	smf_set_terminate(SMF_CTX(ctx), ctx->err ? ctx->err : -1);
}


// Cloud location request
static void handle_cloud_location_request(const struct lte_lc_cells_info *cell_info)
{
	int err = 0;
	struct nrf_cloud_location_result cell_pos_result = {0};
	const struct nrf_cloud_rest_location_request cell_pos_req = {
		.config = &app.config,
		.cell_info = (struct lte_lc_cells_info *)cell_info,
	};

	err = nrf_cloud_coap_location_get(&cell_pos_req, &cell_pos_result);
	if (err) {
		LOG_ERR("Request failed, error: %d", err);
		if (cell_pos_result.err != NRF_CLOUD_ERROR_NONE) {
			LOG_ERR("nRF Cloud error code: %d", cell_pos_result.err);
		}
		app.err = err;
		app.evt = EVT_CLOUD_LOC_ERROR;
		return;
	}

	LOG_INF("Cellular location request fulfilled with %s",
		cell_pos_result.type == LOCATION_TYPE_SINGLE_CELL ? "single-cell" :
		cell_pos_result.type == LOCATION_TYPE_MULTI_CELL ? "multi-cell" :
		"unknown");

	if (app.config.do_reply) {
		app.lat = cell_pos_result.lat;
		app.lon = cell_pos_result.lon;
		app.unc = cell_pos_result.unc;
	} else {
		LOG_INF("Result of location request only stored in nRF Cloud.");
	}

	app.evt = EVT_CLOUD_LOC_READY;
}

int main(void)
{
	int ret;

	smf_set_initial(SMF_CTX(&app), &app_states[STATE_BOOT]);

	while (1) {
		ret = smf_run_state(SMF_CTX(&app));
		if (ret) {
			return ret;
		}

		k_sleep(K_MSEC(100));
	}
}