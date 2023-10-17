 /*
 * Copyright (c) 2020 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT quectel_bg95_ppp

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_mgsm, CONFIG_MODEM_LOG_LEVEL);

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/ppp.h>
// #include <zephyr/drivers/modem/mgsm_ppp.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/console/uart_mux.h>

#include <orbitz_sdk/drivers/modem_mgsm.h>

#include "modem_context.h"
#include "modem_iface_uart.h"
#include "modem_cmd_handler.h"
#include "gsm_mux.h"

#include <stdio.h>

#define MGSM_UART_NODE                   DT_INST_BUS(0)
#define MGSM_CMD_READ_BUF                128
#define MGSM_CMD_AT_TIMEOUT              K_SECONDS(8)
#define MGSM_CMD_SETUP_TIMEOUT           K_SECONDS(6)
/* MGSM_CMD_LOCK_TIMEOUT should be longer than MGSM_CMD_AT_TIMEOUT & MGSM_CMD_SETUP_TIMEOUT,
 * otherwise the MGSM_ppp_stop might fail to lock tx.
 */
#define MGSM_CMD_LOCK_TIMEOUT            K_SECONDS(10)
#define MGSM_RECV_MAX_BUF                30
#define MGSM_RECV_BUF_SIZE               128
#define MGSM_ATTACH_RETRY_DELAY_MSEC     1000
#define MGSM_REGISTER_DELAY_MSEC         1000
#define MGSM_RETRY_DELAY                 K_SECONDS(1)

#define MGSM_RSSI_RETRY_DELAY_MSEC       2000
#define MGSM_RSSI_RETRIES                10
#define MGSM_RSSI_INVALID                -1000

#if defined(CONFIG_MODEM_MGSM_ENABLE_CESQ_RSSI)
	#define MGSM_RSSI_MAXVAL          0
#else
	#define MGSM_RSSI_MAXVAL         -51
#endif

#define ZEPHYR_USER DT_PATH(zephyr_user)
static const struct gpio_dt_spec slna = GPIO_DT_SPEC_GET(ZEPHYR_USER, lna_gpios);
static const struct gpio_dt_spec slte = GPIO_DT_SPEC_GET(ZEPHYR_USER, lte_gpios);
static const struct gpio_dt_spec sbg95_pwr = GPIO_DT_SPEC_GET(ZEPHYR_USER, bg95_pwr_gpios);

// static const struct gpio_dt_spec sled_red = GPIO_DT_SPEC_GET(ZEPHYR_USER, led_red);

// only nodes with aliases:
#define LDO_NODE DT_ALIAS(ldo)
#if DT_NODE_HAS_STATUS(LDO_NODE, okay)
static struct gpio_dt_spec sldo = GPIO_DT_SPEC_GET_OR(LDO_NODE, gpios, {0});
#endif

#define LDO_POWER  // if enabled/defined, LDO will be powered and LEDs are functional
#define BG95_POWER // if enabled/defined, BG95 will be powered


/* Modem network registration state */
enum network_state {
	MGSM_NET_INIT = -1,
	MGSM_NET_NOT_REGISTERED,
	MGSM_NET_HOME_NETWORK,
	MGSM_NET_SEARCHING,
	MGSM_NET_REGISTRATION_DENIED,
	MGSM_NET_UNKNOWN,
	MGSM_NET_ROAMING,
};

static struct mgsm_modem {
	struct k_mutex lock;
	const struct device *dev;
	struct modem_context context;

	struct modem_cmd_handler_data cmd_handler_data;
	uint8_t cmd_match_buf[MGSM_CMD_READ_BUF];
	struct k_sem sem_response;
	struct k_sem sem_if_down;

	struct modem_iface_uart_data mgsm_data;
	struct k_work_delayable mgsm_configure_work;
	char mgsm_rx_rb_buf[PPP_MRU * 3];

	uint8_t *ppp_recv_buf;
	size_t ppp_recv_buf_len;

	enum mgsm_ppp_state {
		MGSM_PPP_START,
		MGSM_PPP_WAIT_AT,
		MGSM_PPP_AT_RDY,
		MGSM_PPP_STATE_INIT,
		MGSM_PPP_STATE_CONTROL_CHANNEL = MGSM_PPP_STATE_INIT,
		MGSM_PPP_STATE_PPP_CHANNEL,
		MGSM_PPP_STATE_AT_CHANNEL,
		MGSM_PPP_STATE_DONE,
		MGSM_PPP_SETUP = MGSM_PPP_STATE_DONE,
		MGSM_PPP_REGISTERING,
		MGSM_PPP_ATTACHING,
		MGSM_PPP_ATTACHED,
		MGSM_PPP_SETUP_DONE,
		MGSM_PPP_STOP,
		MGSM_PPP_STATE_ERROR,
	} state;

	const struct device *ppp_dev;
	const struct device *at_dev;
	const struct device *control_dev;

	struct net_if *iface;

	struct k_thread rx_thread;
	struct k_work_q workq;
	struct k_work_delayable rssi_work_handle;
	struct mgsm_ppp_modem_info minfo;

	enum network_state net_state;

	int retries;
	bool modem_info_queried : 1;

	void *user_data;

	mgsm_modem_power_cb modem_on_cb;
	mgsm_modem_power_cb modem_off_cb;
	struct net_mgmt_event_callback mgsm_mgmt_cb;
} mgsm;

NET_BUF_POOL_DEFINE(mgsm_recv_pool, MGSM_RECV_MAX_BUF, MGSM_RECV_BUF_SIZE, 0, NULL);
K_KERNEL_STACK_DEFINE(mgsm_rx_stack, CONFIG_MODEM_MGSM_RX_STACK_SIZE);
K_KERNEL_STACK_DEFINE(mgsm_workq_stack, CONFIG_MODEM_MGSM_WORKQ_STACK_SIZE);

static inline void mgsm_ppp_lock(struct mgsm_modem *mgsm)
{
	(void)k_mutex_lock(&mgsm->lock, K_FOREVER);
}

static inline void mgsm_ppp_unlock(struct mgsm_modem *mgsm)
{
	(void)k_mutex_unlock(&mgsm->lock);
}

static inline int mgsm_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	return k_work_reschedule_for_queue(&mgsm.workq, dwork, delay);
}

#if defined(CONFIG_MODEM_MGSM_ENABLE_CESQ_RSSI)
	/* helper macro to keep readability */
#define ATOI(s_, value_, desc_) modem_atoi(s_, value_, desc_, __func__)

/**
 * @brief  Convert string to long integer, but handle errors
 *
 * @param  s: string with representation of integer number
 * @param  err_value: on error return this value instead
 * @param  desc: name the string being converted
 * @param  func: function where this is called (typically __func__)
 *
 * @retval return integer conversion on success, or err_value on error
 */
static int modem_atoi(const char *s, const int err_value,
				const char *desc, const char *func)
{
	int ret;
	char *endptr;

	ret = (int)strtol(s, &endptr, 10);
	if ((endptr == NULL) || (*endptr != '\0')) {
		LOG_ERR("bad %s '%s' in %s", s,
			 desc, func);
		return err_value;
	}

	return ret;
}
#endif

static void mgsm_rx(struct mgsm_modem *mgsm)
{
	LOG_DBG("starting");

	while (true) {
		modem_iface_uart_rx_wait(&mgsm->context.iface, K_FOREVER);

		/* The handler will listen AT channel */
		modem_cmd_handler_process(&mgsm->context.cmd_handler, &mgsm->context.iface);
	}
}

MODEM_CMD_DEFINE(mgsm_cmd_ok)
{
	(void)modem_cmd_handler_set_error(data, 0);
	LOG_DBG("ok");
	k_sem_give(&mgsm.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(mgsm_cmd_error)
{
	(void)modem_cmd_handler_set_error(data, -EINVAL);
	LOG_DBG("error");
	k_sem_give(&mgsm.sem_response);
	return 0;
}

/* Handler: +CME Error: <err>[0] */
MODEM_CMD_DEFINE(mgsm_cmd_exterror)
{
	/* TODO: map extended error codes to values */
	(void)modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mgsm.sem_response);
	return 0;
}

static const struct modem_cmd response_cmds[] = {
	MODEM_CMD("OK", mgsm_cmd_ok, 0U, ""),
	MODEM_CMD("ERROR", mgsm_cmd_error, 0U, ""),
	MODEM_CMD("+CME ERROR: ", mgsm_cmd_exterror, 1U, ""),
	MODEM_CMD("CONNECT", mgsm_cmd_ok, 0U, ""),
};

static int unquoted_atoi(const char *s, int base)
{
	if (*s == '"') {
		s++;
	}

	return strtol(s, NULL, base);
}

/*
 * Handler: +COPS: <mode>[0],<format>[1],<oper>[2]
 */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_cops)
{
	if (argc >= 1) {
		LOG_INF("!!! inside argc of cops");
#if defined(CONFIG_MODEM_CELL_INFO)
		if (argc >= 3) {
			mgsm.context.data_operator = unquoted_atoi(argv[2], 10);
			LOG_INF("operator: %u",
				mgsm.context.data_operator);
		}
#endif
		if (unquoted_atoi(argv[0], 10) == 0) {
			mgsm.context.is_automatic_oper = true;
			LOG_INF("!!! cops true");
		} else {
			mgsm.context.is_automatic_oper = false;
			LOG_INF("!!! cops false");
		}
	}

	return 0;
}

/*
 * Provide modem info if modem shell is enabled. This can be shown with
 * "modem list" shell command.
 */

/* Handler: <manufacturer> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_manufacturer)
{
	size_t out_len;

	out_len = net_buf_linearize(mgsm.minfo.mdm_manufacturer,
				    sizeof(mgsm.minfo.mdm_manufacturer) - 1,
				    data->rx_buf, 0, len);
	mgsm.minfo.mdm_manufacturer[out_len] = '\0';
	LOG_INF("Manufacturer: %s", mgsm.minfo.mdm_manufacturer);

	return 0;
}

/* Handler: <model> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_model)
{
	size_t out_len;

	out_len = net_buf_linearize(mgsm.minfo.mdm_model,
				    sizeof(mgsm.minfo.mdm_model) - 1,
				    data->rx_buf, 0, len);
	mgsm.minfo.mdm_model[out_len] = '\0';
	LOG_INF("Model: %s", mgsm.minfo.mdm_model);

	return 0;
}

/* Handler: <rev> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_revision)
{
	size_t out_len;

	out_len = net_buf_linearize(mgsm.minfo.mdm_revision,
				    sizeof(mgsm.minfo.mdm_revision) - 1,
				    data->rx_buf, 0, len);
	mgsm.minfo.mdm_revision[out_len] = '\0';
	LOG_INF("Revision: %s", mgsm.minfo.mdm_revision);

	return 0;
}

/* Handler: <IMEI> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_imei)
{
	size_t out_len;

	out_len = net_buf_linearize(mgsm.minfo.mdm_imei, sizeof(mgsm.minfo.mdm_imei) - 1,
				    data->rx_buf, 0, len);
	mgsm.minfo.mdm_imei[out_len] = '\0';
	LOG_INF("IMEI: %s", mgsm.minfo.mdm_imei);

	return 0;
}

#if defined(CONFIG_MODEM_SIM_NUMBERS)
/* Handler: <IMSI> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_imsi)
{
	size_t out_len;

	out_len = net_buf_linearize(mgsm.minfo.mdm_imsi, sizeof(mgsm.minfo.mdm_imsi) - 1,
				    data->rx_buf, 0, len);
	mgsm.minfo.mdm_imsi[out_len] = '\0';
	LOG_INF("IMSI: %s", mgsm.minfo.mdm_imsi);

	return 0;
}

/* Handler: <ICCID> */
// MODEM_CMD_DEFINE(on_cmd_atcmdinfo_iccid)
// {
// 	size_t out_len;

// 	out_len = net_buf_linearize(mgsm.minfo.mdm_iccid, sizeof(mgsm.minfo.mdm_iccid) - 1,
// 				    data->rx_buf, 0, len);
// 	mgsm.minfo.mdm_iccid[out_len] = '\0';
// 	if (mgsm.minfo.mdm_iccid[0] == '+') {
// 		/* Seen on U-blox SARA: "+CCID: nnnnnnnnnnnnnnnnnnnn".
// 		 * Skip over the +CCID bit, which other modems omit.
// 		 */
// 		char *p = strchr(mgsm.minfo.mdm_iccid, ' ');

// 		if (p) {
// 			size_t len = strlen(p+1);

// 			(void)memmove(mgsm.minfo.mdm_iccid, p+1, len+1);
// 		}
// 	}
// 	LOG_INF("ICCID: %s", mgsm.minfo.mdm_iccid);

// 	return 0;
// }
#endif /* CONFIG_MODEM_SIM_NUMBERS */

/* Handler: <CGPADDR> */
MODEM_CMD_DEFINE(on_cmd_ipinfo)
{
	int ip = atoi(argv[1]);
	LOG_INF("!!!!!IP is %s", argv[1]);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_net_reg_sts)
{
	mgsm.net_state = (enum network_state)atoi(argv[1]);
	LOG_INF("!!!! netowrk state is %d", mgsm.net_state);

	switch (mgsm.net_state) {
	case MGSM_NET_NOT_REGISTERED:
		LOG_DBG("Network %s.", "not registered");
		break;
	case MGSM_NET_HOME_NETWORK:
		LOG_DBG("Network %s.", "registered, home network");
		break;
	case MGSM_NET_SEARCHING:
		LOG_DBG("Searching for network...");
		break;
	case MGSM_NET_REGISTRATION_DENIED:
		LOG_DBG("Network %s.", "registration denied");
		break;
	case MGSM_NET_UNKNOWN:
		LOG_DBG("Network %s.", "unknown");
		break;
	case MGSM_NET_ROAMING:
		LOG_DBG("Network %s.", "registered, roaming");
		break;
	default:
		break;
	}

	return 0;
}

#if defined(CONFIG_MODEM_CELL_INFO)

/*
 * Handler: +CEREG: <n>[0],<stat>[1],<tac>[2],<ci>[3],<AcT>[4]
 */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_cereg)
{
	LOG_INF("!!! inside cops cereg %d", argc);
	if (argc >= 4) {
		mgsm.context.data_lac = unquoted_atoi(argv[2], 16);
		mgsm.context.data_cellid = unquoted_atoi(argv[3], 16);
		LOG_INF("lac: %u, cellid: %u",
			mgsm.context.data_lac,
			mgsm.context.data_cellid);
	}

	if (argc >= 5) {
		mgsm.context.data_act = unquoted_atoi(argv[4], 10);
		LOG_INF("act: %u", mgsm.context.data_act);
	}
	LOG_INF("argv[0] %s, argv[1] %s", argv[0], argv[1]);

	return 0;
}

static const struct setup_cmd query_cellinfo_cmds[] = {
	SETUP_CMD_NOHANDLE("AT+CEREG=2"),
	SETUP_CMD("AT+CEREG?", "", on_cmd_atcmdinfo_cereg, 5U, ","),
	// SETUP_CMD_NOHANDLE("AT+COPS=3,2"),
	SETUP_CMD("AT+COPS?", "", on_cmd_atcmdinfo_cops, 3U, ","),
};

static int mgsm_query_cellinfo(struct mgsm_modem *mgsm)
{
	int ret;

	ret = modem_cmd_handler_setup_cmds_nolock(&mgsm->context.iface,
						  &mgsm->context.cmd_handler,
						  query_cellinfo_cmds,
						  ARRAY_SIZE(query_cellinfo_cmds),
						  &mgsm->sem_response,
						  MGSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_WRN("modem query for cell info returned %d", ret);
	}

	return ret;
}
#endif /* CONFIG_MODEM_CELL_INFO */

#if defined(CONFIG_MODEM_MGSM_ENABLE_CESQ_RSSI)
/*
 * Handler: +CESQ: <rxlev>[0],<ber>[1],<rscp>[2],<ecn0>[3],<rsrq>[4],<rsrp>[5]
 */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_rssi_cesq)
{
	int rsrp, rscp, rxlev;

	rsrp = ATOI(argv[5], 0, "rsrp");
	rscp = ATOI(argv[2], 0, "rscp");
	rxlev = ATOI(argv[0], 0, "rxlev");

	if ((rsrp >= 0) && (rsrp <= 97)) {
		mgsm.minfo.mdm_rssi = -140 + (rsrp - 1);
		LOG_DBG("RSRP: %d", mgsm.minfo.mdm_rssi);
	} else if ((rscp >= 0) && (rscp <= 96)) {
		mgsm.minfo.mdm_rssi = -120 + (rscp - 1);
		LOG_DBG("RSCP: %d", mgsm.minfo.mdm_rssi);
	} else if ((rxlev >= 0) && (rxlev <= 63)) {
		mgsm.minfo.mdm_rssi = -110 + (rxlev - 1);
		LOG_DBG("RSSI: %d", mgsm.minfo.mdm_rssi);
	} else {
		mgsm.minfo.mdm_rssi = MGSM_RSSI_INVALID;
		LOG_DBG("RSRP/RSCP/RSSI not known");
	}

	return 0;
}
#else
/* Handler: +CSQ: <signal_power>[0],<qual>[1] */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_rssi_csq)
{
	/* Expected response is "+CSQ: <signal_power>,<qual>" */
	if (argc > 0) {
		int rssi = atoi(argv[0]);

		if ((rssi >= 0) && (rssi <= 31)) {
			LOG_DBG("read rssi: %d", rssi);
			rssi = -113 + (rssi * 2);
		} else {
			LOG_DBG("Invalid rrsi");
			rssi = MGSM_RSSI_INVALID;
		}

		mgsm.minfo.mdm_rssi = rssi;
		LOG_DBG("RSSI: %d", rssi);
	}

	return 0;
}
#endif

#if defined(CONFIG_MODEM_MGSM_ENABLE_CESQ_RSSI)
static const struct modem_cmd read_rssi_cmd =
	MODEM_CMD("+CESQ:", on_cmd_atcmdinfo_rssi_cesq, 6U, ",");
#else
static const struct modem_cmd read_rssi_cmd =
	MODEM_CMD("+CSQ:", on_cmd_atcmdinfo_rssi_csq, 2U, ",");
#endif

static const struct setup_cmd setup_modem_info_cmds[] = {
	/* query modem info */
	SETUP_CMD("AT+CGMI", "", on_cmd_atcmdinfo_manufacturer, 0U, ""),
	SETUP_CMD("AT+CGMM", "", on_cmd_atcmdinfo_model, 0U, ""),
	SETUP_CMD("AT+CGMR", "", on_cmd_atcmdinfo_revision, 0U, ""),
	// SETUP_CMD("AT+CGSN", "", on_cmd_atcmdinfo_imei, 0U, ""),
#if defined(CONFIG_MODEM_SIM_NUMBERS)
	SETUP_CMD("AT+CIMI", "", on_cmd_atcmdinfo_imsi, 0U, ""),
	// SETUP_CMD("AT+CCID", "", on_cmd_atcmdinfo_iccid, 0U, ""),
#endif
};

static const struct setup_cmd setup_cmds[] = {
	/* no echo */
	// SETUP_CMD_NOHANDLE("ATE0"),
	/* hang up */
	// SETUP_CMD_NOHANDLE("ATH"),
	/*Specify the band to be searched*/
	// SETUP_CMD_NOHANDLE("AT+NBAND=5"),
	// SETUP_CMD_NOHANDLE("AT+NCONFIG=AUTOCONNECT,FALSE"),
	/*Set the UE into full functionality mode*/
	SETUP_CMD_NOHANDLE("AT+CFUN=1"),
	/* extended errors in numeric form */
	SETUP_CMD_NOHANDLE("AT+CMEE=1"),
	/* disable unsolicited network registration codes */
	SETUP_CMD_NOHANDLE("AT+CEREG=0"),
	/* Query the IMSI number */
	SETUP_CMD("AT+CIMI", "", on_cmd_atcmdinfo_imsi, 0U, ""),
	/* Trigger network attachment */
	SETUP_CMD_NOHANDLE("AT+CGDCONT=1,\"IP\",\"" CONFIG_MODEM_MGSM_APN "\""),
	SETUP_CMD_NOHANDLE("AT+CGATT=1"),
	/* create PDP context */
	// SETUP_CMD_NOHANDLE("AT+CGDCONT=1,\"IP\",\"" CONFIG_MODEM_MGSM_APN "\""),
	// SETUP_CMD_NOHANDLE("AT+CGATT=1"),
// #if IS_ENABLED(DT_PROP(MGSM_UART_NODE, hw_flow_control))
// 	/* enable hardware flow control */
// 	SETUP_CMD_NOHANDLE("AT+IFC=2,2"),
// #endif
};

// static const struct setup_cmd setup_cmds[] = {
// 	/* no echo */
// 	SETUP_CMD_NOHANDLE("ATE0"),
// 	/* hang up */
// 	SETUP_CMD_NOHANDLE("ATH"),
// 	/*me added*/
// 	// SETUP_CMD_NOHANDLE("AT+CFUN=1"),
// 	/* extended errors in numeric form */
// 	SETUP_CMD_NOHANDLE("AT+CMEE=1"),
// 	/* disable unsolicited network registration codes */
// 	SETUP_CMD_NOHANDLE("AT+CEREG=0"),
// 	/* create PDP context */
// 	SETUP_CMD_NOHANDLE("AT+CGDCONT=1,\"IP\",\"" CONFIG_MODEM_MGSM_APN "\""),
// #if IS_ENABLED(DT_PROP(MGSM_UART_NODE, hw_flow_control))
// 	/* enable hardware flow control */
// 	SETUP_CMD_NOHANDLE("AT+IFC=2,2"),
// #endif
// 	// SETUP_CMD_NOHANDLE("AT+QGPSCFG=\"priority\",1"),
// 	/* Trigger network attachment */
// 	// SETUP_CMD_NOHANDLE("AT+CGATT=1"),
// };

MODEM_CMD_DEFINE(on_cmd_atcmdinfo_attached)
{
	/* Expected response is "+CGATT: 0|1" so simply look for '1' */
	if ((argc > 0) && (atoi(argv[0]) == 1)) {
		LOG_INF("Attached to packet service!");
	}

	return 0;
}


static const struct modem_cmd read_cops_cmd =
	MODEM_CMD_ARGS_MAX("+COPS:", on_cmd_atcmdinfo_cops, 1U, 4U, ",");

static const struct modem_cmd check_net_reg_cmd =
	MODEM_CMD("+CEREG: ", on_cmd_net_reg_sts, 2U, ",");

static const struct modem_cmd check_attached_cmd =
	MODEM_CMD("+CGATT:", on_cmd_atcmdinfo_attached, 1U, ",");

static const struct modem_cmd check_ip_cmd = 
	MODEM_CMD("+CGPADDR:", on_cmd_ipinfo, 2U, "," );

static const struct setup_cmd connect_cmds[] = {
	/* connect to network */
	SETUP_CMD_NOHANDLE("AT+CGPADDR"),
	SETUP_CMD_NOHANDLE("ATD*99#"),
};

static int mgsm_query_modem_info(struct mgsm_modem *mgsm)
{
	LOG_INF("!!! mgsm QUERY INFO");
	int ret;

	if (mgsm->modem_info_queried) {
		return 0;
	}

	ret = modem_cmd_handler_setup_cmds_nolock(&mgsm->context.iface,
						  &mgsm->context.cmd_handler,
						  setup_modem_info_cmds,
						  ARRAY_SIZE(setup_modem_info_cmds),
						  &mgsm->sem_response,
						  MGSM_CMD_SETUP_TIMEOUT);
	LOG_DBG("RETURN VALUE OF CMD_SETUP_NOLOCK: %d", ret);

	if (ret < 0) {
		return ret;
	}

	mgsm->modem_info_queried = true;

	return 0;
}

static int mgsm_setup_mccmno(struct mgsm_modem *mgsm)
{
	int ret = 0;
	LOG_INF("inside mgsm_setup_mcmno");
	ret = modem_cmd_send_nolock(&mgsm->context.iface,
						    &mgsm->context.cmd_handler,
						    NULL, 0, "AT+CFUN=1",
						    &mgsm->sem_response,
						    MGSM_CMD_AT_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("AT+CFUN ret:%d", ret);
	}

	// ret = modem_cmd_send_nolock(&mgsm->context.iface,
	// 					    &mgsm->context.cmd_handler,
	// 					    NULL, 0, "AT+CFUN=1",
	// 					    &mgsm->sem_response,
	// 					    MGSM_CMD_AT_TIMEOUT);
	// if (ret < 0) {
	// 	LOG_ERR("AT+CFUN ret:%d", ret);
	// }

	if (CONFIG_MODEM_MGSM_MANUAL_MCCMNO[0] != '\0') {
		LOG_INF("sends AT+COPS=1,2,2404");
		/* use manual MCC/MNO entry */
		ret = modem_cmd_send_nolock(&mgsm->context.iface,
					    &mgsm->context.cmd_handler,
					    NULL, 0,
					    "AT+COPS=1,2,\""
					    CONFIG_MODEM_MGSM_MANUAL_MCCMNO
					    "\"",
					    &mgsm->sem_response,
					    MGSM_CMD_AT_TIMEOUT);
	} else {

/* First AT+COPS? is sent to check if automatic selection for operator
 * is already enabled, if yes we do not send the command AT+COPS= 0,0.
 */
		LOG_INF("sends AT+COPS?");
		ret = modem_cmd_send_nolock(&mgsm->context.iface,
					    &mgsm->context.cmd_handler,
					    &read_cops_cmd,
					    1, "AT+COPS?",
					    &mgsm->sem_response,
					    MGSM_CMD_SETUP_TIMEOUT);

		if (ret < 0) {
			return ret;
		}

		if (!mgsm->context.is_automatic_oper) {
			/* register operator automatically */
			ret = modem_cmd_send_nolock(&mgsm->context.iface,
						    &mgsm->context.cmd_handler,
						    NULL, 0, "AT+COPS=0,0",
						    &mgsm->sem_response,
						    MGSM_CMD_AT_TIMEOUT);
		}
	}

	if (ret < 0) {
		LOG_ERR("AT+COPS ret:%d", ret);
	}

	return ret;
}

static struct net_if *ppp_net_if(void)
{
	return net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
}

static void set_ppp_carrier_on(struct mgsm_modem *mgsm)
{
	static const struct ppp_api *api;
	const struct device *ppp_dev = device_get_binding(CONFIG_NET_PPP_DRV_NAME);
	struct net_if *iface = mgsm->iface;
	int ret;

	if (ppp_dev == NULL) {
		LOG_ERR("Cannot find PPP %s!", CONFIG_NET_PPP_DRV_NAME);
		return;
	}

	if (api == NULL) {
		LOG_INF("api is null");
		api = (const struct ppp_api *)ppp_dev->api;

		/* For the first call, we want to call ppp_start()... */
		ret = api->start(ppp_dev);
		if (ret < 0) {
			LOG_ERR("ppp start returned %d", ret);
		}
	} else {
		LOG_INF("enable net_if_l2");
		/* ...but subsequent calls should be to ppp_enable() */
		ret = net_if_l2(iface)->enable(iface, true);
		if (ret < 0) {
			LOG_ERR("ppp l2 enable returned %d", ret);
		}
	}
}

static void query_rssi(struct mgsm_modem *mgsm, bool lock)
{
	int ret;

#if defined(CONFIG_MODEM_MGSM_ENABLE_CESQ_RSSI)
	ret = modem_cmd_send_ext(&mgsm->context.iface, &mgsm->context.cmd_handler, &read_rssi_cmd, 1,
				 "AT+CESQ", &mgsm->sem_response, MGSM_CMD_SETUP_TIMEOUT,
				 lock ? 0 : MODEM_NO_TX_LOCK);
#else
	LOG_INF("send CSQ");
	ret = modem_cmd_send_ext(&mgsm->context.iface, &mgsm->context.cmd_handler, &read_rssi_cmd, 1,
				 "AT+CSQ", &mgsm->sem_response, MGSM_CMD_SETUP_TIMEOUT,
				 lock ? 0 : MODEM_NO_TX_LOCK);
#endif

	if (ret < 0) {
		LOG_DBG("No answer to RSSI readout, %s", "ignoring...");
	}
}

static inline void query_rssi_lock(struct mgsm_modem *mgsm)
{
	query_rssi(mgsm, true);
}

static inline void query_rssi_nolock(struct mgsm_modem *mgsm)
{
	query_rssi(mgsm, false);
}

static void rssi_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mgsm_modem *mgsm = CONTAINER_OF(dwork, struct mgsm_modem, rssi_work_handle);

	mgsm_ppp_lock(mgsm);
	query_rssi_lock(mgsm);

#if defined(CONFIG_MODEM_CELL_INFO)
	(void)mgsm_query_cellinfo(mgsm);
#endif
	(void)mgsm_work_reschedule(&mgsm->rssi_work_handle,
				  K_SECONDS(CONFIG_MODEM_MGSM_RSSI_POLLING_PERIOD));
	mgsm_ppp_unlock(mgsm);
}

static void mgsm_finalize_connection(struct k_work *work)
{
	LOG_INF("INSIDE MGSM_FINALIZE CONNECTION");
	int ret = 0;
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mgsm_modem *mgsm = CONTAINER_OF(dwork, struct mgsm_modem, mgsm_configure_work);

	mgsm_ppp_lock(mgsm);

	/* If already attached, jump right to RSSI readout */
	if (mgsm->state == MGSM_PPP_ATTACHED) {
		goto attached;
	}

	/* If attach check failed, we should not redo every setup step */
	if (mgsm->state == MGSM_PPP_ATTACHING) {
		goto attaching;
	}

	/* If modem is searching for network, we should skip the setup step */
	if (mgsm->state == MGSM_PPP_REGISTERING) {
		goto registering;
	}

	if (IS_ENABLED(CONFIG_GSM_MUX)) {
		ret = modem_cmd_send_nolock(&mgsm->context.iface,
					    &mgsm->context.cmd_handler,
					    &response_cmds[0],
					    ARRAY_SIZE(response_cmds),
					    "AT", &mgsm->sem_response,
					    MGSM_CMD_AT_TIMEOUT);
		if (ret < 0) {
			LOG_ERR("%s returned %d, %s", "AT", ret, "retrying...");
			(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, MGSM_RETRY_DELAY);
			goto unlock;
		}
	}
	LOG_INF("go to MGSM_PPP_SETUP");
	mgsm->state = MGSM_PPP_SETUP;

	if (IS_ENABLED(CONFIG_MODEM_MGSM_FACTORY_RESET_AT_BOOT)) {
		LOG_INF("RESETING...");
		(void)modem_cmd_send_nolock(&mgsm->context.iface,
					    &mgsm->context.cmd_handler,
					    &response_cmds[0],
					    ARRAY_SIZE(response_cmds),
					    "AT&F", &mgsm->sem_response,
					    MGSM_CMD_AT_TIMEOUT);
		(void)k_sleep(K_SECONDS(1));
	}

	ret = mgsm_setup_mccmno(mgsm);
	if (ret < 0) {
		LOG_ERR("%s returned %d, %s", "mgsm_setup_mccmno", ret, "retrying...");

		(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, MGSM_RETRY_DELAY);
		goto unlock;
	}
	LOG_INF("sends setup commands");
	ret = modem_cmd_handler_setup_cmds_nolock(&mgsm->context.iface,
						  &mgsm->context.cmd_handler,
						  setup_cmds,
						  ARRAY_SIZE(setup_cmds),
						  &mgsm->sem_response,
						  MGSM_CMD_SETUP_TIMEOUT);
	LOG_INF("!!!!tRIGGER NETWORK ATTACHMENT");
	if (ret < 0) {
		LOG_DBG("%s returned %d, %s", "setup_cmds", ret, "retrying...");
		(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, MGSM_RETRY_DELAY);
		goto unlock;
	}
	LOG_INF("sends query info");
	ret = mgsm_query_modem_info(mgsm);
	if (ret < 0) {
		LOG_DBG("Unable to query modem information %d", ret);
		(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, MGSM_RETRY_DELAY);
		goto unlock;
	}
	LOG_INF("go to REgistering");
	mgsm->state = MGSM_PPP_REGISTERING;
registering:
		LOG_INF("!!!REGISTERING!!!");
	/* Wait for cell tower registration */
	ret = modem_cmd_send_nolock(&mgsm->context.iface,
				    &mgsm->context.cmd_handler,
				    &check_net_reg_cmd, 1,
				    "AT+CEREG?",
				    &mgsm->sem_response,
				    MGSM_CMD_SETUP_TIMEOUT);
	// if ((ret < 0) || ((mgsm->net_state != MGSM_NET_ROAMING) &&
	// 		 (mgsm->net_state != MGSM_NET_HOME_NETWORK))) {
	// 	if (mgsm->retries == 0) {
	// 		mgsm->retries = CONFIG_MODEM_MGSM_REGISTER_TIMEOUT *
	// 			(MSEC_PER_SEC / MGSM_REGISTER_DELAY_MSEC);
	// 	} else {
	// 		mgsm->retries--;
	// 	}

	// 	(void)MGSM_work_reschedule(&mgsm->MGSM_configure_work,
	// 				  K_MSEC(MGSM_REGISTER_DELAY_MSEC));
	// 	goto unlock;
	// }

	mgsm->retries = 0;
	mgsm->state = MGSM_PPP_ATTACHING;
attaching:
	/* Don't initialize PPP until we're attached to packet service */
	ret = modem_cmd_send_nolock(&mgsm->context.iface,
				    &mgsm->context.cmd_handler,
				    &check_attached_cmd, 1,
				    "AT+CGATT?",
				    &mgsm->sem_response,
				    MGSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		/*
		 * retries not set        -> trigger N attach retries
		 * retries set            -> decrement and retry
		 * retries set, becomes 0 -> trigger full retry
		 */
		if (mgsm->retries == 0) {
			mgsm->retries = CONFIG_MODEM_MGSM_ATTACH_TIMEOUT *
				(MSEC_PER_SEC / MGSM_ATTACH_RETRY_DELAY_MSEC);
		} else {
			mgsm->retries--;
		}

		LOG_DBG("Not attached, %s", "retrying...");

		(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work,
					K_MSEC(MGSM_ATTACH_RETRY_DELAY_MSEC));
		goto unlock;
	}

	/* Attached, clear retry counter */
	LOG_DBG("modem attach returned %d, %s", ret, "read RSSI");
	mgsm->state = MGSM_PPP_ATTACHED;
	mgsm->retries = MGSM_RSSI_RETRIES;

 attached:
	LOG_INF("!!!ATTACHED");
	ret = modem_cmd_send_nolock(&mgsm->context.iface,
				    &mgsm->context.cmd_handler,
				    &check_ip_cmd, 1,
				    "AT+CGPADDR",
				    &mgsm->sem_response,
				    MGSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		/*
		 * retries not set        -> trigger N attach retries
		 * retries set            -> decrement and retry
		 * retries set, becomes 0 -> trigger full retry
		 */
		LOG_DBG("Not aBle to retireve ip address");
				goto unlock;
	}
	if (!IS_ENABLED(CONFIG_GSM_MUX)) {
		/* Read connection quality (RSSI) before PPP carrier is ON */
		query_rssi_nolock(mgsm);

		if (!((mgsm->minfo.mdm_rssi) && (mgsm->minfo.mdm_rssi != MGSM_RSSI_INVALID) &&
			(mgsm->minfo.mdm_rssi < MGSM_RSSI_MAXVAL))) {

			LOG_DBG("Not valid RSSI, %s", "retrying...");
			if (mgsm->retries-- > 0) {
				(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work,
							K_MSEC(MGSM_RSSI_RETRY_DELAY_MSEC));
				goto unlock;
			}
		}
#if defined(CONFIG_MODEM_CELL_INFO)
		(void)mgsm_query_cellinfo(mgsm);
#endif
	}

	LOG_DBG("modem RSSI: %d, %s", mgsm->minfo.mdm_rssi, "enable PPP");

	ret = modem_cmd_handler_setup_cmds_nolock(&mgsm->context.iface,
						  &mgsm->context.cmd_handler,
						  connect_cmds,
						  ARRAY_SIZE(connect_cmds),
						  &mgsm->sem_response,
						  MGSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_DBG("%s returned %d, %s", "connect_cmds", ret, "retrying...");
		(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, MGSM_RETRY_DELAY);
		goto unlock;
	}

	mgsm->state = MGSM_PPP_SETUP_DONE;
	set_ppp_carrier_on(mgsm);
	LOG_INF("set_ppp_carrie4r one");
	if (IS_ENABLED(CONFIG_GSM_MUX)) {
		/* Re-use the original iface for AT channel */
		ret = modem_iface_uart_init_dev(&mgsm->context.iface,
						mgsm->at_dev);
		if (ret < 0) {
			LOG_DBG("iface %suart error %d", "AT ", ret);
			mgsm->state = MGSM_PPP_STATE_ERROR;
		} else {
			/* Do a test and try to send AT command to modem */
			ret = modem_cmd_send_nolock(
				&mgsm->context.iface,
				&mgsm->context.cmd_handler,
				&response_cmds[0],
				ARRAY_SIZE(response_cmds),
				"AT", &mgsm->sem_response,
				MGSM_CMD_AT_TIMEOUT);
			if (ret < 0) {
				LOG_WRN("%s returned %d, %s", "AT", ret, "iface failed");
				mgsm->state = MGSM_PPP_STATE_ERROR;
			} else {
				LOG_INF("AT channel %d connected to %s",
					DLCI_AT, mgsm->at_dev->name);
			}
		}

		modem_cmd_handler_tx_unlock(&mgsm->context.cmd_handler);
		if (mgsm->state != MGSM_PPP_STATE_ERROR) {
			(void)mgsm_work_reschedule(&mgsm->rssi_work_handle,
						K_SECONDS(CONFIG_MODEM_MGSM_RSSI_POLLING_PERIOD));
		}
	}

unlock:
	LOG_INF("UNLOCK mgsm");
	mgsm_ppp_unlock(mgsm);
}

static int mux_enable(struct mgsm_modem *mgsm)
{
	int ret;

	/* Turn on muxing */
	if (IS_ENABLED(CONFIG_MODEM_MGSM_SIMCOM)) {
		ret = modem_cmd_send_nolock(
			&mgsm->context.iface,
			&mgsm->context.cmd_handler,
			&response_cmds[0],
			ARRAY_SIZE(response_cmds),
#if defined(SIMCOM_LTE)
			/* FIXME */
			/* Some SIMCOM modems can set the channels */
			/* Control channel always at DLCI 0 */
			"AT+CMUXSRVPORT=0,0;"
			/* PPP should be at DLCI 1 */
			"+CMUXSRVPORT=" STRINGIFY(DLCI_PPP) ",1;"
			/* AT should be at DLCI 2 */
			"+CMUXSRVPORT=" STRINGIFY(DLCI_AT) ",1;"
#else
			"AT"
#endif
			"+CMUX=0,0,5,"
			STRINGIFY(CONFIG_GSM_MUX_MRU_DEFAULT_LEN),
			&mgsm->sem_response,
			MGSM_CMD_AT_TIMEOUT);
	} else if (IS_ENABLED(CONFIG_MODEM_MGSM_QUECTEL)) {
		ret = modem_cmd_send_nolock(&mgsm->context.iface,
				    &mgsm->context.cmd_handler,
				    &response_cmds[0],
				    ARRAY_SIZE(response_cmds),
				    "AT+CMUX=0,0,5,"
				    STRINGIFY(CONFIG_GSM_MUX_MRU_DEFAULT_LEN),
				    &mgsm->sem_response,
				    MGSM_CMD_AT_TIMEOUT);

		/* Arbitrary delay for Quectel modems to initialize the CMUX,
		 * without this the AT cmd will fail.
		 */
		(void)k_sleep(K_SECONDS(1));
	} else {
		/* Generic mgsm modem */
		ret = modem_cmd_send_nolock(&mgsm->context.iface,
				     &mgsm->context.cmd_handler,
				     &response_cmds[0],
				     ARRAY_SIZE(response_cmds),
				     "AT+CMUX=0", &mgsm->sem_response,
				     MGSM_CMD_AT_TIMEOUT);
	}

	if (ret < 0) {
		LOG_ERR("AT+CMUX ret:%d", ret);
	}

	return ret;
}

static void mux_setup_next(struct mgsm_modem *mgsm)
{
	(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, K_MSEC(1));
}

static void mux_attach_cb(const struct device *mux, int dlci_address,
			  bool connected, void *user_data)
{
	LOG_DBG("DLCI %d to %s %s", dlci_address, mux->name,
		connected ? "connected" : "disconnected");

	if (connected) {
		uart_irq_rx_enable(mux);
		uart_irq_tx_enable(mux);
	}

	mux_setup_next(user_data);
}

static int mux_attach(const struct device *mux, const struct device *uart,
		      int dlci_address, void *user_data)
{
	int ret = uart_mux_attach(mux, uart, dlci_address, mux_attach_cb,
				  user_data);
	if (ret < 0) {
		LOG_ERR("Cannot attach DLCI %d (%s) to %s (%d)", dlci_address,
			mux->name, uart->name, ret);
		return ret;
	}

	return 0;
}

static void mux_setup(struct k_work *work)
{
	LOG_INF("inside MUX_SETUP!!!!!!!");
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mgsm_modem *mgsm = CONTAINER_OF(dwork, struct mgsm_modem,
					     mgsm_configure_work);
	const struct device *const uart = DEVICE_DT_GET(MGSM_UART_NODE);
	int ret;

	mgsm_ppp_lock(mgsm);

	switch (mgsm->state) {
	case MGSM_PPP_STATE_CONTROL_CHANNEL:
		/* We need to call this to reactivate mux ISR. Note: This is only called
		 * after re-initing MGSM_ppp.
		 */
		if (mgsm->ppp_dev != NULL) {
			uart_mux_enable(mgsm->ppp_dev);
		}

		/* Get UART device. There is one dev / DLCI */
		if (mgsm->control_dev == NULL) {
			mgsm->control_dev = uart_mux_alloc();
			if (mgsm->control_dev == NULL) {
				LOG_DBG("Cannot get UART mux for %s channel",
					"control");
				goto fail;
			}
		}

		ret = mux_attach(mgsm->control_dev, uart, DLCI_CONTROL, mgsm);
		if (ret < 0) {
			goto fail;
		}

		mgsm->state = MGSM_PPP_STATE_PPP_CHANNEL;
		goto unlock;

	case MGSM_PPP_STATE_PPP_CHANNEL:
		if (mgsm->ppp_dev == NULL) {
			mgsm->ppp_dev = uart_mux_alloc();
			if (mgsm->ppp_dev == NULL) {
				LOG_DBG("Cannot get UART mux for %s channel",
					"PPP");
				goto fail;
			}
		}

		ret = mux_attach(mgsm->ppp_dev, uart, DLCI_PPP, mgsm);
		if (ret < 0) {
			goto fail;
		}

		mgsm->state = MGSM_PPP_STATE_AT_CHANNEL;
		goto unlock;

	case MGSM_PPP_STATE_AT_CHANNEL:
		if (mgsm->at_dev == NULL) {
			mgsm->at_dev = uart_mux_alloc();
			if (mgsm->at_dev == NULL) {
				LOG_DBG("Cannot get UART mux for %s channel",
					"AT");
				goto fail;
			}
		}

		ret = mux_attach(mgsm->at_dev, uart, DLCI_AT, mgsm);
		if (ret < 0) {
			goto fail;
		}

		mgsm->state = MGSM_PPP_STATE_DONE;
		goto unlock;

	case MGSM_PPP_STATE_DONE:
		/* At least the SIMCOM modem expects that the Internet
		 * connection is created in PPP channel. We will need
		 * to attach the AT channel to context iface after the
		 * PPP connection is established in order to give AT commands
		 * to the modem.
		 */
		ret = modem_iface_uart_init_dev(&mgsm->context.iface,
						mgsm->ppp_dev);
		if (ret < 0) {
			LOG_DBG("iface %suart error %d", "PPP ", ret);
			goto fail;
		}

		LOG_INF("PPP channel %d connected to %s",
			DLCI_PPP, mgsm->ppp_dev->name);

		k_work_init_delayable(&mgsm->mgsm_configure_work, mgsm_finalize_connection);
		(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, K_NO_WAIT);
		goto unlock;
	default:
		__ASSERT(0, "%s while in state: %d", "mux_setup", mgsm->state);
		/* In case CONFIG_ASSERT is off, goto fail */
		goto fail;
	}

fail:
	mgsm->state = MGSM_PPP_STATE_ERROR;
unlock:
	mgsm_ppp_unlock(mgsm);
}

static void mgsm_configure(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mgsm_modem *mgsm = CONTAINER_OF(dwork, struct mgsm_modem,
					     mgsm_configure_work);
	int ret = -1;

	mgsm_ppp_lock(mgsm);

	if (mgsm->state == MGSM_PPP_WAIT_AT) {
		goto wait_at;
	}

	if (mgsm->state == MGSM_PPP_START) {
		LOG_DBG("Starting modem %p configuration", mgsm);

		if (mgsm->modem_on_cb != NULL) {
			mgsm->modem_on_cb(mgsm->dev, mgsm->user_data);
		}

		mgsm->state = MGSM_PPP_WAIT_AT;
	}

wait_at:
	LOG_INF("In the wait_at");
	ret = modem_cmd_send_nolock(&mgsm->context.iface,
				    &mgsm->context.cmd_handler,
				    &response_cmds[0],
				    ARRAY_SIZE(response_cmds),
				    "AT", &mgsm->sem_response,
				    MGSM_CMD_AT_TIMEOUT);
	if (ret < 0) {
		LOG_DBG("modem not ready %d", ret);
		goto retry;
	}
	LOG_INF("go to at_rdy");
	mgsm->state = MGSM_PPP_AT_RDY;

	if (IS_ENABLED(CONFIG_GSM_MUX)) {
		if (mux_enable(mgsm) == 0) {
			LOG_DBG("mgsm muxing %s", "enabled");
		} else {
			LOG_DBG("mgsm muxing %s", "disabled");
			goto retry;
		}
		LOG_INF("go to MGSM_PPP_STATE_INIT");
		mgsm->state = MGSM_PPP_STATE_INIT;

		k_work_init_delayable(&mgsm->mgsm_configure_work, mux_setup);
	} else {
		k_work_init_delayable(&mgsm->mgsm_configure_work, mgsm_finalize_connection);
	}

retry:
	(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, K_NO_WAIT);
	mgsm_ppp_unlock(mgsm);
}

void mgsm_ppp_start(const struct device *dev)
{
	LOG_INF("INSIDE mgsm PPP START");
	int ret;
	struct mgsm_modem *mgsm = dev->data;

	mgsm_ppp_lock(mgsm);

	if (mgsm->state != MGSM_PPP_STOP) {
		LOG_ERR("MGSM_ppp is already %s", "started");
		goto unlock;
	}

	mgsm->state = MGSM_PPP_START;

	/* Re-init underlying UART comms */
	ret = modem_iface_uart_init_dev(&mgsm->context.iface, DEVICE_DT_GET(MGSM_UART_NODE));
	if (ret < 0) {
		LOG_ERR("modem_iface_uart_init returned %d", ret);
		mgsm->state = MGSM_PPP_STATE_ERROR;
		goto unlock;
	}

	k_work_init_delayable(&mgsm->mgsm_configure_work, mgsm_configure);
	(void)mgsm_work_reschedule(&mgsm->mgsm_configure_work, K_NO_WAIT);

unlock:
	mgsm_ppp_unlock(mgsm);
}

void mgsm_ppp_stop(const struct device *dev)
{
	struct mgsm_modem *mgsm = dev->data;
	struct net_if *iface = mgsm->iface;
	struct k_work_sync work_sync;

	if (mgsm->state == MGSM_PPP_STOP) {
		LOG_ERR("MGSM_ppp is already %s", "stopped");
		return;
	}

	(void)k_work_cancel_delayable_sync(&mgsm->mgsm_configure_work, &work_sync);
	if (IS_ENABLED(CONFIG_GSM_MUX)) {
		(void)k_work_cancel_delayable_sync(&mgsm->rssi_work_handle, &work_sync);
	}

	mgsm_ppp_lock(mgsm);

	/* wait for the interface to be properly down */
	if (net_if_is_up(iface)) {
		(void)(net_if_l2(iface)->enable(iface, false));
		(void)k_sem_take(&mgsm->sem_if_down, K_FOREVER);
	}

	if (IS_ENABLED(CONFIG_GSM_MUX)) {
		if (mgsm->ppp_dev != NULL) {
			uart_mux_disable(mgsm->ppp_dev);
		}

		if (modem_cmd_handler_tx_lock(&mgsm->context.cmd_handler,
								MGSM_CMD_LOCK_TIMEOUT) < 0) {
			LOG_WRN("Failed locking modem cmds!");
		}
	}

	if (mgsm->modem_off_cb != NULL) {
		mgsm->modem_off_cb(mgsm->dev, mgsm->user_data);
	}

	mgsm->state = MGSM_PPP_STOP;
	mgsm->net_state = MGSM_NET_INIT;
	mgsm_ppp_unlock(mgsm);
}

void mgsm_ppp_register_modem_power_callback(const struct device *dev,
					   mgsm_modem_power_cb modem_on,
					   mgsm_modem_power_cb modem_off,
					   void *user_data)
{
	LOG_INF(" mgsm_ppp_register_modem_power_callback");
	struct mgsm_modem *mgsm = dev->data;

	mgsm_ppp_lock(mgsm);

	mgsm->modem_on_cb = modem_on;
	mgsm->modem_off_cb = modem_off;

	mgsm->user_data = user_data;
	mgsm_ppp_unlock(mgsm);
}

const struct mgsm_ppp_modem_info *mgsm_ppp_modem_info(const struct device *dev)
{
	struct mgsm_modem *mgsm = dev->data;

	return &mgsm->minfo;
}

static void mgsm_mgmt_event_handler(struct net_mgmt_event_callback *cb,
			  uint32_t mgmt_event, struct net_if *iface)
{
	LOG_INF("Inside mgsm_mgm_evt_handler");
	if ((mgmt_event & NET_EVENT_IF_DOWN) != mgmt_event) {
		return;
	}

	/* Right now we only support 1 mgsm instance */
	if (iface != mgsm.iface) {
		return;
	}

	if (mgmt_event == NET_EVENT_IF_DOWN) {
		LOG_INF("mgsm network interface down");
		/* raise semaphore to indicate the interface is down */
		k_sem_give(&mgsm.sem_if_down);
		return;
	}
}
/**
 * @brief: Enable the LNA antenna tuning circuit.
 *
 * @return: negative value indicating error otherwise 0 (= success)
 */
static int lna_enable_init()
{
    int ret = EXIT_FAILURE;

    // LNA has little to do with modem.
    // assert(device_is_ready(slna.port));
    if (!device_is_ready(slna.port))
    {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&slna, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        return -EINVAL;
    }

    return ret;
}

/**
 * @brief: Enable the LDO (lmt87 & LED) circuit.
 *
 * @return: negative value indicating error otherwise 0 (= success)
 */
static int ldo_enable_init()
{
    int ret = EXIT_FAILURE;
#ifndef LDO_POWER
    return 0;
#endif
    // assert(device_is_ready(sldo.port));
    if (!device_is_ready(sldo.port))
    {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&sldo, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        return -EINVAL;
    }
    ret = gpio_pin_set_dt(&sldo, 0);
    if (ret < 0)
    {
        return ret;
    }
    return ret;
}
/**
 * @brief: Enable the LTE circuit to power the BG95
 *
 * @return: negative value indicating error otherwise 0 (= success)
 */
static int lte_enable_init()
{
    int ret = EXIT_FAILURE;

    // assert(device_is_ready(slte.port));
    if (!device_is_ready(slte.port))
    {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&slte, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        return -EINVAL;
    }

    return ret;
}
/**
 * @brief: Enable the bg95 pwr pin
 *
 * @return: negative value indicating error otherwise 0 (= success)
 */
static int lbg95_pwr_init()
{
    int ret = EXIT_FAILURE;

    // assert(device_is_ready(sbg95_pwr.port));
    if (!device_is_ready(sbg95_pwr.port))
    {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&sbg95_pwr, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        return -EINVAL;
    }

    return ret;
}

/* Func: pin_init
 * Desc: Boot up the Modem.
 */
static void pin_init(void)
{
	int ret = -1;
	ret = lna_enable_init();
	if (ret <0 )
	{
		LOG_DBG("lna failed %d", ret);
	}
	ret =  ldo_enable_init();
	if (ret <0 )
	{
		LOG_DBG("ldo failed %d", ret);
	}
	ret = lte_enable_init();
	if (ret <0 )
	{
		LOG_DBG("lte failed %d", ret);
	}
	ret = lbg95_pwr_init();
	if (ret <0 )
	{
		LOG_DBG("lbg95 pwr failed %d", ret);
	}
	LOG_INF("Setting Modem Pins");
		/* MDM_POWER -> 1 for 500-1000 msec. */
	gpio_pin_set_dt(&sbg95_pwr, 1);
	k_sleep(K_MSEC(750));

	/* MDM_POWER -> 0 and wait for ~2secs as UART remains in "inactive" state
	 * for some time after the power signal is enabled.
	 */
	gpio_pin_set_dt(&sbg95_pwr, 0);
	k_sleep(K_SECONDS(2));

	LOG_INF("... Done!");

}

static int mgsm_init(const struct device *dev)
{
	LOG_INF("inside mgsm_init");
	pin_init();
	struct mgsm_modem *mgsm = dev->data;
	int ret;

	LOG_DBG("Generic mgsm modem (%p)", mgsm);

	(void)k_mutex_init(&mgsm->lock);
	mgsm->dev = dev;

	const struct modem_cmd_handler_config cmd_handler_config = {
		.match_buf = &mgsm->cmd_match_buf[0],
		.match_buf_len = sizeof(mgsm->cmd_match_buf),
		.buf_pool = &mgsm_recv_pool,
		.alloc_timeout = K_NO_WAIT,
		.eol = "\r",
		.user_data = NULL,
		.response_cmds = response_cmds,
		.response_cmds_len = ARRAY_SIZE(response_cmds),
		.unsol_cmds = NULL,
		.unsol_cmds_len = 0,
	};

	(void)k_sem_init(&mgsm->sem_response, 0, 1);
	(void)k_sem_init(&mgsm->sem_if_down, 0, 1);

	ret = modem_cmd_handler_init(&mgsm->context.cmd_handler, &mgsm->cmd_handler_data,
				     &cmd_handler_config);
	if (ret < 0) {
		LOG_DBG("cmd handler error %d", ret);
		return ret;
	}

#if defined(CONFIG_MODEM_SHELL)
	/* modem information storage */
	mgsm->context.data_manufacturer = mgsm->minfo.mdm_manufacturer;
	mgsm->context.data_model = mgsm->minfo.mdm_model;
	mgsm->context.data_revision = mgsm->minfo.mdm_revision;
	mgsm->context.data_imei = mgsm->minfo.mdm_imei;
#if defined(CONFIG_MODEM_SIM_NUMBERS)
	mgsm->context.data_imsi = mgsm->minfo.mdm_imsi;
	// mgsm->context.data_iccid = mgsm->minfo.mdm_iccid;
#endif	/* CONFIG_MODEM_SIM_NUMBERS */
	mgsm->context.data_rssi = &mgsm->minfo.mdm_rssi;
#endif	/* CONFIG_MODEM_SHELL */

	mgsm->context.is_automatic_oper = false;

	const struct modem_iface_uart_config uart_config = {
		.rx_rb_buf = &mgsm->mgsm_rx_rb_buf[0],
		.rx_rb_buf_len = sizeof(mgsm->mgsm_rx_rb_buf),
		.hw_flow_control = DT_PROP(MGSM_UART_NODE, hw_flow_control),
		.dev = DEVICE_DT_GET(MGSM_UART_NODE),
	};

	ret = modem_iface_uart_init(&mgsm->context.iface, &mgsm->mgsm_data, &uart_config);
	if (ret < 0) {
		LOG_DBG("iface uart error %d", ret);
		return ret;
	}
	LOG_INF("iface uart is enabled");

	ret = modem_context_register(&mgsm->context);
	if (ret < 0) {
		LOG_DBG("context error %d", ret);
		return ret;
	}

	/* Initialize to stop state so that it can be started later */
	mgsm->state = MGSM_PPP_STOP;

	mgsm->net_state = MGSM_NET_INIT;

	LOG_DBG("iface->read %p iface->write %p",
		mgsm->context.iface.read, mgsm->context.iface.write);

	(void)k_thread_create(&mgsm->rx_thread, mgsm_rx_stack,
			      K_KERNEL_STACK_SIZEOF(mgsm_rx_stack),
			      (k_thread_entry_t) mgsm_rx,
			      mgsm, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	(void)k_thread_name_set(&mgsm->rx_thread, "mgsm_rx");

	/* initialize the work queue */
	k_work_queue_init(&mgsm->workq);
	k_work_queue_start(&mgsm->workq, mgsm_workq_stack, K_KERNEL_STACK_SIZEOF(mgsm_workq_stack),
			   K_PRIO_COOP(7), NULL);
	(void)k_thread_name_set(&mgsm->workq.thread, "mgsm_workq");

	if (IS_ENABLED(CONFIG_GSM_MUX)) {
		k_work_init_delayable(&mgsm->rssi_work_handle, rssi_handler);
	}

	mgsm->iface = ppp_net_if();
	if (mgsm->iface == NULL) {
		LOG_ERR("Couldn't find ppp net_if!");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&mgsm->mgsm_mgmt_cb, mgsm_mgmt_event_handler,
				     NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&mgsm->mgsm_mgmt_cb);

	if (IS_ENABLED(CONFIG_MGSM_PPP_AUTOSTART)) {
		mgsm_ppp_start(dev);
	}

	return 0;
}

// DEVICE_DT_DEFINE(DT_DRV_INST(0), mgsm_init, NULL, &mgsm, NULL,
// 		 POST_KERNEL, CONFIG_MODEM_mgsm_INIT_PRIORITY, NULL);
DEVICE_DT_DEFINE(DT_INST(0, quectel_bg95_ppp), mgsm_init, NULL, &mgsm, NULL,
		 POST_KERNEL, CONFIG_MODEM_MGSM_INIT_PRIORITY, NULL);