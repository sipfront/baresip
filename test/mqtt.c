/**
 * @file test/mqtt.c  Baresip selftest -- MQTT connect behaviour
 *
 * Runs the mqtt module against a fake broker on localhost: a TCP listener
 * that answers CONNECT with CONNACK, late, or never. Skipped when the mqtt
 * module is not built.
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "test.h"


#define DEBUG_MODULE "test_mqtt"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/* Restores the selftest configuration (see main.c) after each test */
static const char *base_config = "ausrc_format    s16\n";

static const char *event_marker = "sit471-buffered-event";


enum broker_mode {
	BROKER_SILENT,		/* accepts, never sends CONNACK */
	BROKER_LATE_CONNACK,	/* sends CONNACK after a delay */
	BROKER_DROP,		/* CONNACK, then closes the first connection */
};


struct fake_broker {
	enum broker_mode mode;
	struct tcp_sock *ts;
	struct tcp_conn *tc;
	struct sa laddr;
	struct tmr tmr;
	unsigned accepts;	/* TCP connections accepted */
	unsigned connects;	/* CONNECT packets received */
	bool connack_sent;
	bool marker_seen;	/* buffered event arrived as PUBLISH */
	unsigned stop_accepts;	/* stop the main loop at this many accepts */
	int err;
};


static bool contains(const uint8_t *buf, size_t len, const char *str)
{
	size_t n = strlen(str);
	size_t i;

	for (i = 0; n && i + n <= len; i++) {
		if (!memcmp(buf + i, str, n))
			return true;
	}

	return false;
}


static void send_connack(void *arg)
{
	struct fake_broker *fb = arg;
	static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
	struct mbuf *mb;

	if (!fb->tc)
		return;

	mb = mbuf_alloc(sizeof(connack));
	if (!mb) {
		fb->err = ENOMEM;
		re_cancel();
		return;
	}
	(void)mbuf_write_mem(mb, connack, sizeof(connack));
	mb->pos = 0;
	fb->err = tcp_send(fb->tc, mb);
	mem_deref(mb);
	fb->connack_sent = true;
}


static void drop_connection(void *arg)
{
	struct fake_broker *fb = arg;

	fb->tc = mem_deref(fb->tc);
}


static void recv_handler(struct mbuf *mb, void *arg)
{
	struct fake_broker *fb = arg;
	const uint8_t *buf = mbuf_buf(mb);
	size_t len = mbuf_get_left(mb);

	if (len && buf[0] == 0x10) {
		++fb->connects;

		switch (fb->mode) {

		case BROKER_SILENT:
			break;

		case BROKER_LATE_CONNACK:
			/* below mqtt_connect_timeout (200 ms) */
			tmr_start(&fb->tmr, 100, send_connack, fb);
			break;

		case BROKER_DROP:
			send_connack(fb);
			if (fb->accepts == 1)
				tmr_start(&fb->tmr, 100, drop_connection, fb);
			break;
		}
	}

	if (contains(buf, len, event_marker)) {
		fb->marker_seen = true;
		re_cancel();
	}
}


static void close_handler(int err, void *arg)
{
	struct fake_broker *fb = arg;
	(void)err;

	fb->tc = mem_deref(fb->tc);
}


static void conn_handler(const struct sa *peer, void *arg)
{
	struct fake_broker *fb = arg;
	(void)peer;

	/* one client at a time: a new connection replaces the old one */
	fb->tc = mem_deref(fb->tc);
	fb->err = tcp_accept(&fb->tc, fb->ts, NULL, recv_handler,
			     close_handler, fb);
	if (fb->err) {
		re_cancel();
		return;
	}

	++fb->accepts;
	if (fb->stop_accepts && fb->accepts >= fb->stop_accepts)
		re_cancel();
}


static void broker_destructor(void *arg)
{
	struct fake_broker *fb = arg;

	tmr_cancel(&fb->tmr);
	mem_deref(fb->tc);
	mem_deref(fb->ts);
}


static int broker_alloc(struct fake_broker **fbp, enum broker_mode mode)
{
	struct fake_broker *fb;
	int err;

	fb = mem_zalloc(sizeof(*fb), broker_destructor);
	if (!fb)
		return ENOMEM;

	fb->mode = mode;
	tmr_init(&fb->tmr);

	err  = sa_set_str(&fb->laddr, "127.0.0.1", 0);
	err |= tcp_listen(&fb->ts, &fb->laddr, conn_handler, fb);
	err |= tcp_sock_local_get(fb->ts, &fb->laddr);
	if (err) {
		mem_deref(fb);
		return err;
	}

	*fbp = fb;
	return 0;
}


/* Points the mqtt module at the fake broker and loads it */
static int mqtt_load(const struct fake_broker *fb, uint64_t *load_ms)
{
	char cfg[256];
	uint64_t t0;
	int err;

	if (re_snprintf(cfg, sizeof(cfg),
			"%s"
			"mqtt_broker_host 127.0.0.1\n"
			"mqtt_broker_port %u\n"
			"mqtt_connect_timeout 200\n"
			"mqtt_basetopic sit471test\n",
			base_config, sa_port(&fb->laddr)) < 0)
		return ENOMEM;

	err = conf_configure_buf((uint8_t *)cfg, str_len(cfg));
	if (err)
		return err;

	t0 = tmr_jiffies();
	err = module_load(".", "mqtt");
	if (load_ms)
		*load_ms = tmr_jiffies() - t0;

	return err;
}


static void mqtt_unload(void)
{
	module_unload("mqtt");
	(void)conf_configure_buf((uint8_t *)base_config,
				 str_len(base_config));
}


/*
 * A broker that accepts the connection but never answers must neither
 * block module loading nor stop the client from retrying (SIT-471).
 */
int test_mqtt_connect_nonblocking(void)
{
	struct fake_broker *fb = NULL;
	uint64_t load_ms = 0;
	int err;

	err = broker_alloc(&fb, BROKER_SILENT);
	TEST_ERR(err);

	err = mqtt_load(fb, &load_ms);
	if (err == ENOENT) {
		info("test_mqtt: mqtt module not built, skipping\n");
		err = 0;
		goto out;
	}
	TEST_ERR(err);

	ASSERT_TRUE(load_ms < 500);

	/* timeout 200 ms + jitter <= 2000 ms per attempt */
	fb->stop_accepts = 3;
	err = re_main_timeout(7000);
	TEST_ERR(err);
	TEST_ERR(fb->err);

	ASSERT_TRUE(fb->accepts >= 3);
	ASSERT_TRUE(!fb->connack_sent);

 out:
	mqtt_unload();
	mem_deref(fb);

	return err;
}


/*
 * Events raised before the broker acknowledged the connection are buffered
 * and published once it did.
 */
int test_mqtt_buffered_events(void)
{
	struct fake_broker *fb = NULL;
	int err;

	err = broker_alloc(&fb, BROKER_LATE_CONNACK);
	TEST_ERR(err);

	err = mqtt_load(fb, NULL);
	if (err == ENOENT) {
		info("test_mqtt: mqtt module not built, skipping\n");
		err = 0;
		goto out;
	}
	TEST_ERR(err);

	err = bevent_app_emit(UA_EVENT_CUSTOM, NULL, "%s", event_marker);
	TEST_ERR(err);
	ASSERT_TRUE(!fb->connack_sent);

	err = re_main_timeout(5000);
	TEST_ERR(err);
	TEST_ERR(fb->err);

	ASSERT_TRUE(fb->connack_sent);
	ASSERT_TRUE(fb->marker_seen);

 out:
	mqtt_unload();
	mem_deref(fb);

	return err;
}


/*
 * A lost connection is re-established without blocking.
 */
int test_mqtt_reconnect(void)
{
	struct fake_broker *fb = NULL;
	int err;

	err = broker_alloc(&fb, BROKER_DROP);
	TEST_ERR(err);

	err = mqtt_load(fb, NULL);
	if (err == ENOENT) {
		info("test_mqtt: mqtt module not built, skipping\n");
		err = 0;
		goto out;
	}
	TEST_ERR(err);

	fb->stop_accepts = 2;
	err = re_main_timeout(5000);
	TEST_ERR(err);
	TEST_ERR(fb->err);

	ASSERT_TRUE(fb->accepts >= 2);

 out:
	mqtt_unload();
	mem_deref(fb);

	return err;
}
