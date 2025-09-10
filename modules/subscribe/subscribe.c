#include <re.h>
#include <baresip.h>

static int cmd_subscribe(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
    struct le *le;
    struct ua *ua = NULL;

    // Find the first registered UA
    for (le = uag_list()->head; le; le = le->next) {
        ua = le->data;
        if (ua_isregistered(ua))
            break;
    }

    if (!ua) {
        re_hprintf(pf, "No registered UA available\n");
        return 0;
    }

    const char *line = (const char *)carg->prm;

    if (!line || *line == '\0') {
        re_hprintf(pf, "Usage: /subscribe <target> <event>\n");
        return 0;
    }

    // Duplicate line so strtok doesn't modify const memory
    char buf[256];
    strncpy(buf, line, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    // Parse target and event
    char *target = strtok(buf, " \t");
    char *event  = strtok(NULL, " \t");

    if (!target || !event) {
        re_hprintf(pf, "Usage: /subscribe <target> <event>\n");
        return 0;
    }

    struct sipsub *sub = NULL;
	const char *routev[1];
	routev[0] = ua_outbound(ua);

    // Simple call to sipevent_subscribe with defaults
    int err = sipevent_subscribe(&sub,
                                 uag_sipevent_sock(), // get the UA's sipevent socket
                                 target,               // URI to subscribe
                                 NULL,                 // from_name
                                 account_aor(ua_account(ua)),           // from_uri
                                 event,                // event type
                                 NULL,                 // id
                                 3600,                 // expires
                                 ua_cuser(ua),                 // cuser
                                 routev, routev[0] ? 1 : 0,              // routev, routec
                                 NULL, NULL,           // auth handler, arg
                                 false,                // aref
                                 NULL, NULL, NULL, NULL, // forkh, notifyh, closeh, arg
                                 NULL);                // fmt

    if (err) {
        re_hprintf(pf, "Subscribe failed: %m\n", err);
        return 0;
    }

    re_hprintf(pf, "Subscription sent to %s for event %s\n", target, event);
    return 0;
}

static const struct cmd cmdv[] = {
    { "subscribe", 0, CMD_PRM, "send a subscription", cmd_subscribe }
};

static int module_init(void)
{
    cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
    return 0;
}

static int module_close(void)
{
    cmd_unregister(baresip_commands(), cmdv);
    return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(subscribe) = {
    "subscribe",
    "application",
    module_init,
    module_close
};
