/**
 * @file mqtt.c Message Queue Telemetry Transport (MQTT) client
 *
 * Copyright (C) 2017 Alfred E. Heggestad
 */

#include <mosquitto.h>
#include <re.h>
#include <baresip.h>
#include "mqtt.h"

static char tls_alpn[256] = "";
static char broker_host[256] = "127.0.0.1";
/* Broker CA file for TLS usage, default none */
static char broker_cafile[256] = "";
/* Authentication user name, default none */
static char mqttusername[256] = "";
/* Authentication password, default none */
static char mqttpassword[256] = "";
/* Client ID - default "baresip" */
static char mqttclientid[256] = "baresip";
/* Base topic for MQTT - default "baresip" - i.e. /baresip/event */
static char mqttbasetopic[128] = "baresip";
static char mqttpublishtopic[256] = "";
static char mqttsubscribetopic[256] = "";
static uint32_t mqttpublishqos = 0;
static uint32_t mqttsubscribeqos = 0;


static uint32_t broker_port = 1883;
/* Time for one connect attempt (TCP, TLS, CONNACK) before retrying [ms] */
static uint32_t connect_timeout = 3000;
/* Random pause before the next connect attempt [ms]. Spreads the retries of
 * many agents started at the same time (SIT-471). */
static const uint32_t retry_delay_min = 500;
static const uint32_t retry_delay_max = 2000;
static const int keepalive = 60;

static struct mqtt s_mqtt;

extern int publish_buffered_messages(struct mqtt *mqtt);

static void mqtt_connect_start(struct mqtt *mqtt);
static void fd_handler(int flags, void *arg);


static void tmr_retry(void *data)
{
	mqtt_connect_start(data);
}


/* Listen for writability only while libmosquitto has something to write
 * (TCP connect, TLS handshake, pending packets); otherwise the socket would
 * report writable all the time. */
static void update_listen(struct mqtt *mqtt)
{
	int flags = FD_READ;
	int err;

	if (!mqtt->fhs)
		return;

	if (mosquitto_want_write(mqtt->mosq))
		flags |= FD_WRITE;

	err = fd_listen(&mqtt->fhs, mqtt->fd, flags, fd_handler, mqtt);
	if (err)
		warning("mqtt: fd_listen failed (%m)\n", err);
}


static void schedule_retry(struct mqtt *mqtt)
{
	uint32_t delay;

	/* A lost connection is reported by the disconnect callback and by
	 * the read/write that detected it; schedule only once. */
	if (mqtt->retry_pending)
		return;
	mqtt->retry_pending = true;

	delay = retry_delay_min +
		rand_u32() % (retry_delay_max - retry_delay_min + 1);

	mqtt->fhs = fd_close(mqtt->fhs);
	tmr_cancel(&mqtt->tmr);

	if (mqtt->conn_attempts)
		warning("mqtt: not connected after attempt %u, "
			"retrying in %u ms\n", mqtt->conn_attempts, delay);
	else
		warning("mqtt: reconnecting in %u ms\n", delay);

	tmr_start(&mqtt->tmr_conn, delay, tmr_retry, mqtt);
}


static void fd_handler(int flags, void *arg)
{
	struct mqtt *mqtt = arg;
	int ret;
	(void)flags;

	/* While connecting, loop_read also drives the TLS handshake */
	ret = mosquitto_loop_read(mqtt->mosq, 1);
	if (ret == MOSQ_ERR_SUCCESS)
		ret = mosquitto_loop_write(mqtt->mosq, 1);

	if (ret != MOSQ_ERR_SUCCESS && !mqtt->is_connected) {
		if (!mqtt->retry_pending)
			warning("mqtt: connect attempt failed (%s)\n",
				mosquitto_strerror(ret));
		schedule_retry(mqtt);
		return;
	}

	update_listen(mqtt);
}


static void tmr_handler(void *data)
{
	struct mqtt *mqtt = data;
	int ret;

	tmr_start(&mqtt->tmr, 500, tmr_handler, mqtt);

	if (!mqtt->is_connected)
		return;

	ret = mosquitto_loop_misc(mqtt->mosq);
	if (ret != MOSQ_ERR_SUCCESS) {
		warning("mqtt: error in loop (%s)\n", mosquitto_strerror(ret));
	}

	update_listen(mqtt);
}


static void tmr_connect_timeout(void *data)
{
	struct mqtt *mqtt = data;

	if (mqtt->is_connected)
		return;

	warning("mqtt: no connection to %s:%d within %u ms\n",
		broker_host, broker_port, connect_timeout);
	schedule_retry(mqtt);
}


/*
 * Starts one non-blocking connect attempt. The attempt ends with CONNACK
 * (connect_callback), with a socket error (fd_handler) or after
 * connect_timeout; the latter two schedule the next attempt. baresip keeps
 * running meanwhile; events are buffered until connected (SIT-471).
 */
static void mqtt_connect_start(struct mqtt *mqtt)
{
	int ret;
	int err;

	++mqtt->conn_attempts;
	mqtt->retry_pending = false;
	mqtt->is_connected = 0;
	mqtt->fhs = fd_close(mqtt->fhs);

	info("mqtt: connecting to %s:%d (attempt %u)\n",
	     broker_host, broker_port, mqtt->conn_attempts);

	/* Closes a previous socket; DNS resolution still blocks briefly */
	ret = mosquitto_connect_async(mqtt->mosq, broker_host, broker_port,
				      keepalive);
	if (ret != MOSQ_ERR_SUCCESS) {
		warning("mqtt: failed to start connect to %s:%d (%s)\n",
			broker_host, broker_port, mosquitto_strerror(ret));
		schedule_retry(mqtt);
		return;
	}

	mqtt->fd = mosquitto_socket(mqtt->mosq);
	err = fd_listen(&mqtt->fhs, mqtt->fd, FD_READ, fd_handler, mqtt);
	if (err) {
		warning("mqtt: fd_listen failed (%m)\n", err);
		schedule_retry(mqtt);
		return;
	}
	update_listen(mqtt);

	tmr_start(&mqtt->tmr, 500, tmr_handler, mqtt);
	tmr_start(&mqtt->tmr_conn, connect_timeout, tmr_connect_timeout, mqtt);
}


/*
 * This is called when the broker sends a CONNACK message
 * in response to a connection.
 */
static void connect_callback(struct mosquitto *mosq, void *obj, int result)
{
	struct mqtt *mqtt = obj;
	int err;
	(void)mosq;

	if (result != MOSQ_ERR_SUCCESS) {
		warning("mqtt: could not connect to broker (%s) \n",
			mosquitto_strerror(result));
		schedule_retry(mqtt);
		return;
	}

	info("mqtt: connected to broker at %s:%d after %u attempt(s)\n",
	     broker_host, broker_port, mqtt->conn_attempts);

	tmr_cancel(&mqtt->tmr_conn);
	mqtt->conn_attempts = 0;
	mqtt->is_connected = 1;

	err = publish_buffered_messages(mqtt);
	if (err) {
		warning("mqtt: publish_buffered_messages failed (%m)\n", err);
	}

	err = mqtt_subscribe_start(mqtt);
	if (err) {
		warning("mqtt: subscribe_init failed (%m)\n", err);
	}
}


static void disconnect_callback(struct mosquitto *mosq, void *obj, int rc)
{
	struct mqtt *mqtt = obj;
	(void) mosq;

	/* Check for expected disconnect */
	if (rc == 0)
		return;

	warning("mqtt: connection lost (%s)\n", mosquitto_strerror(rc));
	mqtt->is_connected = 0;
	schedule_retry(mqtt);
}


static int module_init(void)
{
	int ret;
	int err = 0;

	tmr_init(&s_mqtt.tmr);
	tmr_init(&s_mqtt.tmr_conn);
	s_mqtt.conn_attempts = 0;
	s_mqtt.retry_pending = false;

	mosquitto_lib_init();

	/* Get configuration data */
	conf_get_str(conf_cur(), "mqtt_broker_host",
		     broker_host, sizeof(broker_host));
	conf_get_str(conf_cur(), "mqtt_broker_cafile",
		     broker_cafile, sizeof(broker_cafile));
	conf_get_str(conf_cur(), "mqtt_broker_user",
		     mqttusername, sizeof(mqttusername));
	conf_get_str(conf_cur(), "mqtt_broker_password",
		     mqttpassword, sizeof(mqttpassword));
	conf_get_str(conf_cur(), "mqtt_broker_clientid",
		     mqttclientid, sizeof(mqttclientid));
	conf_get_str(conf_cur(), "mqtt_basetopic",
		     mqttbasetopic, sizeof(mqttbasetopic));
	conf_get_str(conf_cur(), "mqtt_publishtopic",
		     mqttpublishtopic, sizeof(mqttpublishtopic));
	conf_get_str(conf_cur(), "mqtt_subscribetopic",
		     mqttsubscribetopic, sizeof(mqttsubscribetopic));
	conf_get_u32(conf_cur(), "mqtt_publishqos", &mqttpublishqos);
	conf_get_u32(conf_cur(), "mqtt_subscribeqos", &mqttsubscribeqos);
 	conf_get_u32(conf_cur(), "mqtt_broker_port", &broker_port);
	conf_get_str(conf_cur(), "mqtt_tls_alpn",
             tls_alpn, sizeof(tls_alpn));
	conf_get_u32(conf_cur(), "mqtt_connect_timeout", &connect_timeout);

	info("mqtt: connecting to broker at %s:%d as %s topic %s\n",
		broker_host, broker_port, mqttclientid, mqttbasetopic);

	if (*mqttsubscribetopic == '\0') {
		re_snprintf(mqttsubscribetopic, sizeof(mqttsubscribetopic),
				"/%s/command/+", mqttbasetopic);
	}
	if (*mqttpublishtopic == '\0') {
		re_snprintf(mqttpublishtopic, sizeof(mqttpublishtopic),
				"/%s/event", mqttbasetopic);
	}

	info("mqtt: Publishing on %s with QoS %u, " \
			"subscribing to %s with QoS %u\n",
	mqttpublishtopic, mqttpublishqos,
	mqttsubscribetopic, mqttsubscribeqos);

	s_mqtt.basetopic = mqttbasetopic;
	s_mqtt.subtopic = mqttsubscribetopic;
	s_mqtt.pubtopic = mqttpublishtopic;
	s_mqtt.pubqos = (int) mqttpublishqos;
	s_mqtt.subqos = (int) mqttsubscribeqos;


	s_mqtt.mosq = mosquitto_new(mqttclientid, true, &s_mqtt);
	if (!s_mqtt.mosq) {
		warning("mqtt: failed to create client instance\n");
		return ENOMEM;
	}

	if (*tls_alpn != '\0') {
		info("mqtt: setting TLS ALPN to %s\n", tls_alpn);
		ret = mosquitto_string_option(s_mqtt.mosq, MOSQ_OPT_TLS_ALPN,
			tls_alpn);
		if (ret != MOSQ_ERR_SUCCESS)
			return ret == MOSQ_ERR_ERRNO ? errno : EIO;
	}

	err = mqtt_subscribe_init(&s_mqtt);
	if (err)
		return err;

	s_mqtt.is_connected = 0;
	mosquitto_connect_callback_set(s_mqtt.mosq, connect_callback);
	mosquitto_disconnect_callback_set(s_mqtt.mosq, disconnect_callback);

	if (*mqttusername != '\0') {
		ret = mosquitto_username_pw_set(s_mqtt.mosq, mqttusername,
			mqttpassword);
		if (ret != MOSQ_ERR_SUCCESS)
			return ret == MOSQ_ERR_ERRNO ? errno : EIO;
	}

	if (*broker_cafile != '\0') {
		ret = mosquitto_tls_set(s_mqtt.mosq, broker_cafile,
				NULL, NULL, NULL, NULL);
		if (ret != MOSQ_ERR_SUCCESS)
			return ret == MOSQ_ERR_ERRNO ? errno : EIO;
	}

	err = mqtt_publish_init(&s_mqtt);
	if (err)
		return err;

	/* Non-blocking: a slow or stalled broker connection must not keep
	 * baresip from loading the other modules (ctrl_tcp) (SIT-471). */
	s_mqtt.fhs = NULL;
	mqtt_connect_start(&s_mqtt);

	info("mqtt: module loaded\n");

	return 0;
}


static int module_close(void)
{
	s_mqtt.fhs = fd_close(s_mqtt.fhs);

	mqtt_publish_close();

	mqtt_subscribe_close();

	tmr_cancel(&s_mqtt.tmr);
	tmr_cancel(&s_mqtt.tmr_conn);

	if (s_mqtt.mosq) {
		int err = publish_buffered_messages(&s_mqtt);
		if (err) {
			warning(
			"mqtt: publish_buffered_messages "
			"failed on shutdown (%m)\n",
			err);
		}

		mosquitto_disconnect(s_mqtt.mosq);

		mosquitto_destroy(s_mqtt.mosq);
		s_mqtt.mosq = NULL;
	}

	mosquitto_lib_cleanup();

	info("mqtt: module unloaded\n");

	return 0;
}


const struct mod_export DECL_EXPORTS(mqtt) = {
	"mqtt",
	"application",
	module_init,
	module_close
};
