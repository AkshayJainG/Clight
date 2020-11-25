#include "idler.h"
#include "utils.h"

static void receive_waiting_acstate(const msg_t *msg, UNUSED const void *userdata);
static void receive_inhibited(const msg_t *const msg, UNUSED const void* userdata);
static int on_new_idle(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void upower_timeout_callback(void);
static void inhibit_callback(const bool pause);

static sd_bus_slot *slot, *dpms_slot;
static char client[PATH_MAX + 1];

DECLARE_MSG(display_req, DISPLAY_REQ);

MODULE("DPMS");

static void init(void) {
    M_SUB(UPOWER_UPD);
    M_SUB(INHIBIT_UPD);
    M_SUB(SUSPEND_UPD);
    M_SUB(DPMS_TO_REQ);
    M_SUB(SIMULATE_REQ);
    m_become(waiting_acstate);
}

static bool check(void) {
    return true;
}

static bool evaluate(void) {
    return !conf.dpms_conf.disabled;
}

static void receive_waiting_acstate(const msg_t *msg, UNUSED const void *userdata) {
    switch (MSG_TYPE()) {
    case UPOWER_UPD: {
        int r = idle_init(client, &slot, conf.dpms_conf.timeout[state.ac_state], on_new_idle);
        if (r != 0) {
            WARN("Failed to init.\n");
            m_poisonpill(self());
        } else {
            m_unbecome();
            
            SYSBUS_ARG(args, CLIGHTD_SERVICE, "/org/clightd/clightd/Dpms", "org.clightd.clightd.Dpms", "Changed");
            add_match(&args, &dpms_slot, on_new_idle);
        }
        break;
    }
    default:
        break;
    }
}

static void receive(const msg_t *const msg, UNUSED const void* userdata) {
    switch (MSG_TYPE()) {
    case UPOWER_UPD:
        upower_timeout_callback();
        break;
    case INHIBIT_UPD:
        inhibit_callback(state.inhibited);
        break;
    case SUSPEND_UPD:
        inhibit_callback(state.suspended);
        break;
    case DPMS_TO_REQ: {
        timeout_upd *up = (timeout_upd *)MSG_DATA();
        if (VALIDATE_REQ(up)) {
            conf.dpms_conf.timeout[up->state] = up->new;
            if (up->state == state.ac_state) {
                upower_timeout_callback();
            }
        }
        break;
    }
    case SIMULATE_REQ: {
        /* Validation is useless here; only for coherence */
        if (VALIDATE_REQ((void *)msg->ps_msg->message)) {
            idle_client_reset(client, conf.dpms_conf.timeout[state.ac_state]);
        }
        break;
    }
    default:
        break;
    }
}

static void receive_inhibited(const msg_t *const msg, UNUSED const void* userdata) {
    switch (MSG_TYPE()) {
    case UPOWER_UPD:
        upower_timeout_callback();
        break;
    case INHIBIT_UPD:
        inhibit_callback(state.inhibited);
        break;
    case SUSPEND_UPD:
        inhibit_callback(state.suspended);
        break;
    case DPMS_TO_REQ: {
        timeout_upd *up = (timeout_upd *)MSG_DATA();
        if (VALIDATE_REQ(up)) {
            conf.dpms_conf.timeout[up->state] = up->new;
            if (up->state == state.ac_state) {
                upower_timeout_callback();
            }
        }
        break;
    }
    default:
        /* SIMULATE_REQ is not handled while inhibited */
        break;
    }
}

static void destroy(void) {
    idle_client_destroy(client);
    if (slot) {
        slot = sd_bus_slot_unref(slot);
    }
    
    if (dpms_slot) {
        dpms_slot = sd_bus_slot_unref(dpms_slot);
    }
}

static int on_new_idle(sd_bus_message *m, UNUSED void *userdata, UNUSED sd_bus_error *ret_error) {
    int idle;
    
    /* Only account for our display for Dpms.Changed signals */
    if (!strcmp(sd_bus_message_get_member(m), "Changed")) {
        const char *state_dpy = fetch_display();
        const char *display = NULL;
        int level;
        sd_bus_message_read(m, "si", &display, &level);
        if (display && state_dpy && strcmp(display, state_dpy)) {
            return 0;
        }
        
        if ((display == NULL) != (state_dpy == NULL)) {
            return 0;
        }
        idle = level > 0;
    } else {
        sd_bus_message_read(m, "b", &idle);
    }
    
    /* Unused in requests! */
    display_req.display.old = state.display_state;
    if (idle) {
        display_req.display.new = DISPLAY_OFF;
    } else {
        display_req.display.new = DISPLAY_ON;
    }
    M_PUB(&display_req);
    
    return 0;
}

/* Reset dimmer timeout */
static void upower_timeout_callback(void) {
    idle_set_timeout(client, conf.dpms_conf.timeout[state.ac_state]);
}

/*
 * If we're getting inhibited, stop idle client.
 * Else, restart it.
 */
static void inhibit_callback(const bool pause) {
    if (!pause) {
        DEBUG("Resuming DPMS.\n");
        idle_client_start(client, conf.dpms_conf.timeout[state.ac_state]);
        m_unbecome();
    } else {
        DEBUG("Pausing DPMS.\n");
        idle_client_stop(client);
        m_become(inhibited);
    }
}
