#include "app.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "devcfg.h"
#include "foreground.h"
#include "keysend.h"
#include "media.h"
#include "volume.h"
#include "proto.h"
#include "usbdev.h"
#include "watchdog.h"

/*
 * The state colours, in the firmware's own order. The defaults are what the
 * device falls back to with no file of its own, repeated here so the interface
 * shows the truth before it has read anything back.
 */
static const app_led_slot_t LED_SLOTS[APP_LED_SLOT_COUNT] = {
    { "on_boot",         "Boot",         "attached, not talking yet",   0xFF0000 },
    { "on_connected",    "Connected",    "a host has configured it",    0x00FF00 },
    { "on_receive",      "Receive",      "a packet is arriving",        0xFF00FF },
    { "on_disconnected", "Disconnected", "no host, the bus is unpowered", 0xFF8000 },
};

/*
 * RRGGBB in the case the rest of the interface uses.
 *
 * keys_format_hex() writes lowercase, which is right for the file it was
 * written for; the colour fields here show what app_sync_hex() produces, and
 * the two must not disagree inside one field.
 */
static void app_format_hex(uint32_t rgb, char *out, size_t size)
{
    snprintf(out, size, "%06X", (unsigned)(rgb & 0xFFFFFFu));
}

const app_led_slot_t *app_led_slot(int index)
{
    if (index < 0 || index >= APP_LED_SLOT_COUNT) {
        return NULL;
    }
    return &LED_SLOTS[index];
}

#define USB_VID     0x303A

/* The controller itself, and the receiver that relays it over the radio. */
#define USB_PID_CONTROLLER 0x6901
#define USB_PID_DONGLE     0x6902

/*
 * Both variants speak the same protocol. The bus is searched for both at once
 * and the first entry here that is attached wins, so the controller takes
 * precedence and the dongle is the fallback.
 */
static const struct {
    uint16_t    pid;
    const char *name;
} USB_VARIANTS[] = {
    { USB_PID_CONTROLLER, "IOMeeter Controller" },
    { USB_PID_DONGLE,     "IOMeeter Dongle" },
};

#define USB_VARIANT_COUNT ((int)(sizeof(USB_VARIANTS) / sizeof(USB_VARIANTS[0])))

#define SEND_TIMEOUT_MS  500

/* Near-zero so a frame is never held up waiting on the device. */
#define POLL_TIMEOUT_MS  1

/*
 * A moving fader sends faster than the frame rate. Draining only a few per
 * frame lets a backlog build, which shows up as the interface lagging behind
 * the physical control; the per-packet work is small enough to take many.
 */
#define POLL_MAX_PACKETS 32

void app_hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0, gf = 0, bf = 0;

    if      (h <  60) { rf = c; gf = x; }
    else if (h < 120) { rf = x; gf = c; }
    else if (h < 180) { gf = c; bf = x; }
    else if (h < 240) { gf = x; bf = c; }
    else if (h < 300) { rf = x; bf = c; }
    else              { rf = c; bf = x; }

    *r = (uint8_t)lroundf((rf + m) * 255.0f);
    *g = (uint8_t)lroundf((gf + m) * 255.0f);
    *b = (uint8_t)lroundf((bf + m) * 255.0f);
}

void app_rgb_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8, float *h, float *s, float *v)
{
    float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f;
    float max = fmaxf(r, fmaxf(g, b));
    float min = fminf(r, fminf(g, b));
    float d = max - min;

    float hue = 0.0f;
    if (d > 0.0f) {
        if      (max == r) hue = 60.0f * fmodf((g - b) / d, 6.0f);
        else if (max == g) hue = 60.0f * (((b - r) / d) + 2.0f);
        else               hue = 60.0f * (((r - g) / d) + 4.0f);
    }
    if (hue < 0.0f) {
        hue += 360.0f;
    }

    *h = hue;
    *s = (max <= 0.0f) ? 0.0f : d / max;
    *v = max;
}

void app_battery_text(const app_t *a, char *out, size_t size)
{
    float whole = (float)(int)a->battery;

    /* Only the starting value has a fraction, so this is really asking
       whether the device has reported yet. */
    if (a->battery - whole > 0.001f) {
        snprintf(out, size, "%.2f%%", (double)a->battery);
    } else {
        snprintf(out, size, "%d%%", (int)a->battery);
    }
}

uint32_t app_rgb(const app_t *a)
{
    uint8_t r, g, b;
    app_hsv_to_rgb(a->hue, a->sat, a->val, &r, &g, &b);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void app_set_rgb(app_t *a, uint32_t rgb)
{
    app_rgb_to_hsv((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                   &a->hue, &a->sat, &a->val);
    app_sync_hex(a);
}

void app_sync_hex(app_t *a)
{
    snprintf(a->hex, sizeof(a->hex), "%06X", (unsigned)app_rgb(a));
}

void app_log(app_t *a, app_log_kind_t kind, const char *fmt, ...)
{
    int slot;
    if (a->log_count < APP_LOG_CAPACITY) {
        slot = (a->log_first + a->log_count) % APP_LOG_CAPACITY;
        a->log_count++;
    } else {
        slot = a->log_first;
        a->log_first = (a->log_first + 1) % APP_LOG_CAPACITY;
    }

    a->log[slot].kind = kind;
    a->log_seq++;

    va_list args;
    va_start(args, fmt);
    vsnprintf(a->log[slot].text, APP_LOG_TEXT_MAX, fmt, args);
    va_end(args);
}

/*
 * Set while the transfer started on connect is running, so that a document
 * arriving from the controller is applied rather than only reported.
 */
static bool s_pull_to_apply;

/*
 * Consecutive receive failures that were not timeouts. One of these is not
 * worth acting on, but a run of them is either a removal or a wedged device,
 * and both want the handle released.
 */
static int s_poll_fails;

/* When the bus was last searched for the controller. */
static time_t s_probe_at;

/*
 * How often that search runs while the dongle is what is open. The same
 * interval as the reconnect sweep, and the same cost: one enumeration.
 */
#define CONTROLLER_PROBE_SECONDS 5

/*
 * Set while the dongle is being handed over to the controller, so the log
 * reads as the one move it is rather than as the dongle dropping out.
 */
static bool s_swapping;

bool app_pair_link_lost(const char *status)
{
    return status != NULL &&
           (strcmp(status, "disconnected") == 0 || strcmp(status, "lost") == 0);
}

bool app_pair_link_up(const char *status)
{
    return status != NULL &&
           (strcmp(status, "connected") == 0 || strcmp(status, "paired") == 0);
}

/* Whether what is open is the dongle rather than the controller. */
static bool app_on_dongle(const app_t *a)
{
    return a->connected && a->dev != NULL && a->dev->pid == USB_PID_DONGLE;
}

/*
 * Name what the interface is talking to.
 *
 * On the dongle that includes the far end of the radio link, since a dongle
 * with no controller behind it is connected and useless, and the two look
 * identical otherwise.
 */
static void app_label_device(app_t *a)
{
    const char *name = "IOMeeter";
    bool dongle = false;

    for (int i = 0; a->dev != NULL && i < USB_VARIANT_COUNT; i++) {
        if (USB_VARIANTS[i].pid == a->dev->pid) {
            name = USB_VARIANTS[i].name;
            dongle = (USB_VARIANTS[i].pid == USB_PID_DONGLE);
            break;
        }
    }

    if (dongle && !a->controller_linked) {
        snprintf(a->device_name, sizeof(a->device_name), "Dongle, no controller");
    } else {
        snprintf(a->device_name, sizeof(a->device_name), "%s", name);
    }
}

bool app_poll_failure_fatal(int rc, int fails, bool present)
{
    /* The tidy answer, and the one Linux usually gives. */
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        return true;
    }

    /* Asked the bus, and the device is not on it. */
    if (fails >= APP_POLL_FAIL_CHECK && !present) {
        return true;
    }

    /*
     * Still listed, but nothing has come off it in a long time. Letting go is
     * what lets the reconnect sweep open it again; holding on achieves
     * nothing and keeps the interface claimed against every other process.
     */
    return fails >= APP_POLL_FAIL_GIVEUP;
}

/*
 * Puts the configuration on the controller, which keeps it as bytes so that it
 * follows the hardware. The file on this machine is still written: it is what
 * the program starts from before anything is plugged in.
 */
void app_config_push(app_t *a)
{
    if (!a->connected) {
        return;
    }

    /* Not indented: nothing on the controller reads this, and every byte of
       it is one more byte to slice up and push across the wire. */
    char *text = config_to_text(a->sliders, APP_FADER_COUNT, a->debug,
                                &a->keys, false);
    if (text == NULL) {
        app_log(a, APP_LOG_ERROR, "could not build the configuration");
        return;
    }

    size_t len = strlen(text);
    if (devcfg_push(a, text, len)) {
        app_log(a, APP_LOG_EVENT, "writing %u bytes to the controller",
                (unsigned)len);
    } else {
        app_log(a, APP_LOG_ERROR, "could not send the configuration: %s",
                devcfg_error());
    }
    free(text);
}

/* Applies a finished transfer, or reports one that failed. */
static void app_devcfg_settle(app_t *a)
{
    switch (devcfg_state()) {
    case DEVCFG_DONE: {
        size_t len = 0;
        const char *doc = devcfg_document(&len);

        if (!s_pull_to_apply) {
            app_log(a, APP_LOG_EVENT, "configuration written to the controller");
            break;
        }
        s_pull_to_apply = false;

        if (doc == NULL || len == 0) {
            /* Nothing stored there yet; this machine's copy stands, and the
               next save puts it on the device. */
            app_log(a, APP_LOG_EVENT,
                    "the controller holds no configuration yet");
            a->config_dirty = true;
            break;
        }

        if (config_from_text(doc, a->sliders, APP_FADER_COUNT, &a->debug,
                             &a->keys)) {
            app_log(a, APP_LOG_EVENT, "loaded %u bytes from the controller",
                    (unsigned)len);
        } else {
            /* config_from_text has already replaced everything with the
               defaults, so the file is what puts the strip back. */
            app_log(a, APP_LOG_ERROR,
                    "the controller's configuration did not parse");
            app_config_load(a);
        }
        break;
    }

    case DEVCFG_FAILED:
        app_log(a, APP_LOG_ERROR, "controller configuration: %s",
                devcfg_error());
        s_pull_to_apply = false;
        break;

    default:
        return;         /* idle or still running */
    }

    devcfg_reset();
}

void app_config_load(app_t *a)
{
    if (config_load(APP_CONFIG_PATH, a->sliders, APP_FADER_COUNT, &a->debug,
                    &a->keys)) {
        app_log(a, APP_LOG_EVENT, "loaded %s", APP_CONFIG_PATH);
    } else {
        /* Absent or unreadable: start from the defaults and write them out so
           the file exists for editing. */
        app_log(a, APP_LOG_EVENT, "no %s, using defaults", APP_CONFIG_PATH);
        a->config_dirty = true;
    }
}

void app_config_save(app_t *a)
{
    if (!a->config_dirty) {
        return;
    }
    a->config_dirty = false;

    if (!config_save(APP_CONFIG_PATH, a->sliders, APP_FADER_COUNT, a->debug,
                     &a->keys)) {
        app_log(a, APP_LOG_ERROR, "could not write %s", APP_CONFIG_PATH);
    }

    app_config_push(a);
}

bool app_config_save_as(app_t *a, const char *path)
{
    if (!config_save(path, a->sliders, APP_FADER_COUNT, a->debug, &a->keys)) {
        app_log(a, APP_LOG_ERROR, "could not write %s", path);
        return false;
    }

    snprintf(a->config_path, sizeof(a->config_path), "%s", path);
    app_log(a, APP_LOG_EVENT, "saved %s", path);
    return true;
}

bool app_config_load_from(app_t *a, const char *path)
{
    if (!config_load(path, a->sliders, APP_FADER_COUNT, &a->debug, &a->keys)) {
        /* config_load has already filled in the defaults, so the strip stays
           usable; only report that the file was not understood. */
        app_log(a, APP_LOG_ERROR, "could not read %s, defaults applied", path);
        a->config_dirty = true;
        return false;
    }

    snprintf(a->config_path, sizeof(a->config_path), "%s", path);
    app_log(a, APP_LOG_EVENT, "loaded %s", path);
    a->config_dirty = true;     /* mirror it into the working config.json */
    return true;
}

bool app_config_reload(app_t *a)
{
    char path[CONFIG_PATH_MAX];

    /* app_config_load_from writes config_path, so it cannot be read from
       the struct while that call is in progress. */
    snprintf(path, sizeof(path), "%s",
             a->config_path[0] ? a->config_path : APP_CONFIG_PATH);

    bool ok = app_config_load_from(a, path);
    if (ok) {
        for (int i = 0; i < APP_FADER_COUNT; i++) {
            app_apply_volume(a, i);
        }
    }
    return ok;
}

void app_notify(app_t *a, const char *text)
{
    snprintf(a->notice, sizeof(a->notice), "%s", text ? text : "");
    a->notice_seq++;
}

void app_log_clear(app_t *a)
{
    a->log_count = 0;
    a->log_first = 0;
    a->log_seq++;
}

const app_log_entry_t *app_log_at(const app_t *a, int index)
{
    if (index < 0 || index >= a->log_count) {
        return NULL;
    }
    return &a->log[(a->log_first + index) % APP_LOG_CAPACITY];
}

void app_init(app_t *a)
{
    memset(a, 0, sizeof(*a));

    a->hue = 0.0f;
    a->sat = 1.0f;
    a->val = 1.0f;
    a->live_send = false;
    a->debug = CONFIG_DEBUG_DEFAULT;
    a->battery = APP_BATTERY_DEFAULT;

    for (int i = 0; i < APP_LED_SLOT_COUNT; i++) {
        app_format_hex(LED_SLOTS[i].def, a->led_hex[i], APP_HEX_MAX);
    }

    /* What the firmware falls back to with no file of its own. */
    a->led_brightness = 1.0f;

    app_sync_hex(a);

    snprintf(a->config_path, sizeof(a->config_path), "%s", APP_CONFIG_PATH);
    app_config_load(a);

    watchdog_phase("volume_start");
    if (volume_start()) {
        app_log(a, APP_LOG_EVENT, "mixer ready");
    } else {
        app_log(a, APP_LOG_ERROR, "volume control disabled: %s",
                volume_last_error());
    }

    watchdog_phase("keysend_start");
    if (keysend_start()) {
        app_log(a, APP_LOG_EVENT, "macro keys ready");
    } else {
        app_log(a, APP_LOG_ERROR, "macros disabled: %s", keysend_last_error());
    }

    watchdog_phase("foreground_init");
    if (!foreground_init()) {
        app_log(a, APP_LOG_ERROR, "profiles will not switch on their own: %s",
                foreground_last_error());
    }

    app_log(a, APP_LOG_EVENT, "not connected");
}

void app_shutdown(app_t *a)
{
    app_config_save(a);
    volume_stop();
    keysend_stop();
    foreground_shutdown();

    if (a->connected) {
        app_disconnect(a);
    }
    free(a->dev);
    a->dev = NULL;

    /* Last of all: every device borrowed this context. */
    usbdev_shutdown();
}

bool app_connect(app_t *a)
{
    if (a->connected) {
        return true;
    }

    if (a->dev == NULL) {
        a->dev = calloc(1, sizeof(*a->dev));
        if (a->dev == NULL) {
            app_log(a, APP_LOG_ERROR, "out of memory");
            return false;
        }
    }

    uint16_t pids[USB_VARIANT_COUNT];
    for (int i = 0; i < USB_VARIANT_COUNT; i++) {
        pids[i] = USB_VARIANTS[i].pid;
    }

    if (usbdev_open_any(a->dev, USB_VID, pids, USB_VARIANT_COUNT) != 0) {
        /* Retried on a timer, so this must not fill the log with one line per
           attempt while nothing is plugged in. */
        if (!a->connect_failure_logged) {
            a->connect_failure_logged = true;

            /* "No device found" is the same sentence whether nothing is
               plugged in, the product id has moved, or a udev rule is
               missing, and those want different answers. */
            char why[USBDEV_REASON_MAX] = "";
            usbdev_explain_absence(USB_VID, pids, USB_VARIANT_COUNT,
                                   why, sizeof(why));

            app_log(a, APP_LOG_ERROR, "no IOMeeter device opened: %s", why);
        }
        return false;
    }

    a->connect_failure_logged = false;

    /* A fresh connection is a fresh link: on the controller it is the device
       itself, and on the dongle nothing has reported otherwise yet. */
    a->controller_linked = true;
    app_label_device(a);

    a->connected = true;
    s_poll_fails = 0;

    /* Anything half-received from a previous session is meaningless now. */
    proto_framer_reset(&a->framer);

    app_log(a, APP_LOG_EVENT, "connected: %s (%04X:%04X)",
            a->device_name, USB_VID, a->dev->pid);
    app_log(a, APP_LOG_EVENT, "interface %d, OUT 0x%02X, IN 0x%02X",
            a->dev->interface, a->dev->ep_out, a->dev->ep_in);

    /* What the hardware carries wins over what this machine last had. */
    if (devcfg_pull(a)) {
        s_pull_to_apply = true;
        app_log(a, APP_LOG_EVENT, "reading the configuration from the controller");
    }
    return true;
}

void app_disconnect(app_t *a)
{
    if (!a->connected) {
        return;
    }
    devcfg_reset();
    s_pull_to_apply = false;
    s_poll_fails = 0;

    usbdev_close(a->dev);
    a->connected = false;
    a->device_name[0] = '\0';

    if (!s_swapping) {
        app_log(a, APP_LOG_EVENT, "disconnected");
    }
}

void app_send_json(app_t *a, const char *json)
{
    if (!a->connected) {
        app_log(a, APP_LOG_ERROR, "not connected");
        return;
    }

    app_log(a, APP_LOG_TX, "-> %s", json);

    if (usbdev_send(a->dev, json, strlen(json), SEND_TIMEOUT_MS) != 0) {
        app_log(a, APP_LOG_ERROR, "send failed, dropping the connection");
        app_disconnect(a);
    }
}

/*
 * Take the stored colours out of a {"config":{"led":{...}}} reply.
 *
 * A slot the device did not mention keeps what it had, and one that is not a
 * colour is left alone rather than shown as something the controller is not
 * doing: the firmware ignores it in exactly the same way.
 */
static void app_read_led_states(app_t *a, const cJSON *config)
{
    /* Beside the colours rather than inside them, because it scales all of
       them. Counted in percent on the device and as a fraction here. */
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(config, "brightness");

    if (cJSON_IsNumber(level)) {
        float fraction = (float)level->valuedouble / 100.0f;

        if (fraction < 0.0f) {
            fraction = 0.0f;
        }
        if (fraction > 1.0f) {
            fraction = 1.0f;
        }
        a->led_brightness = fraction;
    }

    const cJSON *led = cJSON_GetObjectItemCaseSensitive(config, "led");
    if (!cJSON_IsObject(led)) {
        return;
    }

    for (int i = 0; i < APP_LED_SLOT_COUNT; i++) {
        const cJSON *slot = cJSON_GetObjectItemCaseSensitive(led,
                                                             LED_SLOTS[i].key);
        uint32_t rgb;

        if (cJSON_IsString(slot) && keys_parse_hex(slot->valuestring, &rgb)) {
            app_format_hex(rgb, a->led_hex[i], APP_HEX_MAX);
        }
    }
}

/*
 * @param root The document the framer parsed on its way to finding the end of
 *             the message, or NULL when the message was not JSON. Owned by the
 *             framer, so nothing here may keep it.
 */
static void app_handle_packet(app_t *a, const unsigned char *data, int len,
                              cJSON *root)
{
    proto_kind_t kind = proto_classify(data, len);

    if (kind == PROTO_EVENT || root == NULL) {
        app_log(a, APP_LOG_EVENT, "<- %.*s", len, (const char *)data);
        return;
    }

    if (kind == PROTO_INTERRUPT) {
        app_log(a, APP_LOG_EVENT, "<- %.*s", len, (const char *)data);

        const cJSON *report = cJSON_GetObjectItemCaseSensitive(root, "interrupt");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(report, "message");

        if (cJSON_IsString(message) && message->valuestring &&
            message->valuestring[0]) {
            app_notify(a, message->valuestring);
        } else {
            app_notify(a, "interrupt");
        }

        return;
    }

    /*
     * A transfer in progress claims its own replies. It only takes an "ok"
     * while it is pushing, so the acknowledgement of some other command
     * cannot be mistaken for one of its slices unless the two overlap.
     */
    if (devcfg_on_message(a, root)) {
        return;
    }

    /* {"get":{"slider":{"id":N}}} -- the device is asking for a slider's state.
       Checked first because a get and a set both carry a "slider" object. */
    const cJSON *get = cJSON_GetObjectItemCaseSensitive(root, "get");
    if (get != NULL) {
        const cJSON *slider = cJSON_GetObjectItemCaseSensitive(get, "slider");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(slider, "id");

        if (cJSON_IsNumber(id)) {
            app_reply_slider(a, id->valueint);
        } else {
            app_log(a, APP_LOG_ERROR, "unsupported get: %.*s", len,
                    (const char *)data);
        }

        return;
    }

    app_log(a, APP_LOG_RX, "<- %.*s", len, (const char *)data);

    /* Commands arrive wrapped in "set"; answers to a get come back bare, so
       both shapes are accepted. */
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "set");
    if (body == NULL) {
        body = root;
    }

    /* {"battery":N}, bare or wrapped in a set like everything else. */
    const cJSON *battery = cJSON_GetObjectItemCaseSensitive(body, "battery");
    if (cJSON_IsNumber(battery)) {
        float level = (float)battery->valuedouble;

        if (level < 0.0f) {
            level = 0.0f;
        }
        if (level > 100.0f) {
            level = 100.0f;
        }
        a->battery = level;
    }

    /*
     * {"pair":{"status":"disconnected"}} -- the controller has left the radio
     * link, usually because it has just been plugged into this machine, where
     * it can be reached directly.
     *
     * The dongle is not touched. It is a fixture: it stays in its port, and it
     * is the controller that comes and goes, so the report says the controller
     * is away rather than that anything here should be let go of. The handover
     * happens below, once there is something to hand over to.
     */
    const cJSON *pair = cJSON_GetObjectItemCaseSensitive(body, "pair");
    if (pair != NULL) {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(pair, "status");
        const char *state = cJSON_IsString(status) ? status->valuestring : NULL;

        /* Only the dongle relays a link. The controller reporting its own
           pairing says nothing about reaching it: it is right here. */
        if (app_on_dongle(a)) {
            if (app_pair_link_lost(state)) {
                a->controller_linked = false;
                s_probe_at = 0;     /* look for it now, not in a few seconds */
                app_log(a, APP_LOG_EVENT, "the controller left the radio link");
                app_label_device(a);
            } else if (app_pair_link_up(state)) {
                a->controller_linked = true;
                app_log(a, APP_LOG_EVENT, "the controller is on the radio link");
                app_label_device(a);
            }
        }

        return;
    }

    /* {"set":{"slider":{"id":N,"value":V}}} -- the device moved one of its own
       sliders, so mirror it and let the UI lock the pointer out briefly. */
    const cJSON *slider = cJSON_GetObjectItemCaseSensitive(body, "slider");
    if (slider != NULL) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(slider, "id");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(slider, "value");

        if (cJSON_IsNumber(id) && cJSON_IsNumber(value) &&
            id->valueint >= 0 && id->valueint < APP_FADER_COUNT) {
            /* The identifier is checked, so the position has to be too. A
               figure past the top of the travel puts the knob off the window,
               and is then written back into the file. */
            int level = value->valueint;

            if (level < 0) {
                level = 0;
            }
            if (level > APP_FADER_MAX) {
                level = APP_FADER_MAX;
            }

            a->sliders[id->valueint].value = level;
            a->slider_extern_seq++;
            a->slider_extern_at[id->valueint]++;
            /* Coalesced: a physical fader sends far faster than the frame rate,
               and each push reaches the audio session graph. */
            a->slider_volume_dirty[id->valueint] = true;
        }

        return;
    }

    const cJSON *led = cJSON_GetObjectItemCaseSensitive(body, "led");
    if (cJSON_IsString(led) && led->valuestring) {
        unsigned rgb;
        if (sscanf(led->valuestring, "%6x", &rgb) == 1) {
            app_set_rgb(a, rgb);
        }

        return;
    }

    /* Only the colours are taken out of it: the rest of what the controller
       stores is its own business, and keeping a copy nothing reads was just
       another thing to go stale. */
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(body, "config");
    if (config != NULL) {
        app_read_led_states(a, config);
    }

}

/* One volume push per slider per poll, however many packets arrived. */
static void apply_dirty_volumes(app_t *a)
{
    for (int i = 0; i < APP_FADER_COUNT; i++) {
        if (a->slider_volume_dirty[i]) {
            a->slider_volume_dirty[i] = false;
            app_apply_volume(a, i);
        }
    }
}

/* Trampoline: the framer hands back one message at a time. */
static void app_on_message(void *user, const unsigned char *msg, int len,
                           struct cJSON *json)
{
    app_handle_packet((app_t *)user, msg, len, (cJSON *)json);
}

/*
 * Take the controller over from the dongle as soon as it is on the bus.
 *
 * Runs for as long as the dongle is what is open, not only after a lost link:
 * the controller can be unplugged and plugged back in at any time, and it
 * announces nothing on the way in -- it is no longer talking to the dongle,
 * and it is not yet talking to this client either. The bus is the only thing
 * that knows.
 *
 * Nothing is released until the controller is there to replace it, so a link
 * that dropped for some other reason -- the controller switched off, or
 * carried out of range -- leaves the connection exactly as it was.
 */
static void app_watch_for_controller(app_t *a)
{
    if (!app_on_dongle(a)) {
        return;
    }

    time_t now = time(NULL);
    if (now - s_probe_at < CONTROLLER_PROBE_SECONDS) {
        return;
    }
    s_probe_at = now;

    if (!usbdev_attached(USB_VID, USB_PID_CONTROLLER)) {
        return;
    }

    app_log(a, APP_LOG_EVENT,
            "the controller is on USB, taking it over from the dongle");

    /* One move, not a drop and a find: the dongle is let go of here only
       because the controller is already waiting to be opened. */
    s_swapping = true;
    app_disconnect(a);
    s_swapping = false;

    app_connect(a);             /* the controller is the first preference */
}

/*
 * Report a media toggle that failed.
 *
 * It ran on the worker, where there was nobody to tell: the key was pressed
 * frames ago and whoever pressed it is owed an answer either way.
 */
static void app_media_settle(app_t *a)
{
    static unsigned seen;
    static unsigned seen_macros;

    unsigned failures = media_failures();

    if (failures != seen) {
        seen = failures;
        app_log(a, APP_LOG_ERROR, "%s", media_last_error());
    }

    /* A macro aimed at a program that is not there fails the same way, on the
       same thread, and is owed the same answer. */
    failures = keysend_failures();

    if (failures != seen_macros) {
        seen_macros = failures;
        app_log(a, APP_LOG_ERROR, "%s", keysend_failure());
    }
}

void app_poll(app_t *a)
{
    /* Before the connection is looked at: a pad key works with nothing
       plugged in, and so does the failure it may report. */
    app_media_settle(a);

    if (!a->connected) {
        return;
    }

    /* An early return below must not strand a pending change. */
    apply_dirty_volumes(a);

    devcfg_tick(a);
    app_devcfg_settle(a);

    /* Can close one device and open another, so nothing below may assume the
       connection this frame started with. */
    app_watch_for_controller(a);
    if (!a->connected) {
        return;
    }

    for (int i = 0; i < POLL_MAX_PACKETS; i++) {
        unsigned char buf[512];
        int len = 0;

        watchdog_phase("usbdev_recv");
        int rc = usbdev_recv(a->dev, buf, sizeof(buf), &len, POLL_TIMEOUT_MS);

        /*
         * Taken first, and whatever the result: a short timeout on a bulk
         * endpoint routinely expires with part of a transfer already in the
         * buffer, and those bytes are the front of a message whose remainder
         * is still on its way.
         */
        if (len > 0) {
            /* One read is not one message: WinUSB in particular returns
               several concatenated, and can split a long one in half. */
            proto_framer_push(&a->framer, buf, len, app_on_message, a);

            /* Handling a message can close the device -- a failed reply drops
               the connection -- and the next read would then be issued on a
               handle that is gone. */
            if (!a->connected) {
                return;
            }
        }

        if (rc == LIBUSB_ERROR_TIMEOUT) {
            s_poll_fails = 0;
            return;         /* nothing more waiting */
        }

        if (rc != 0) {
            /*
             * A removal does not reach us the same way everywhere.
             * LIBUSB_ERROR_NO_DEVICE is the tidy answer, but WinUSB reports an
             * unplugged device as an I/O error, which this used to treat as
             * transient and retry forever: the handle stayed open, the
             * interface stayed claimed, and because the client still believed
             * it was connected the reconnect sweep never ran. So anything that
             * is not a timeout is counted, and a run of them asks the bus.
             */
            s_poll_fails++;

            bool present = true;
            if (rc != LIBUSB_ERROR_NO_DEVICE &&
                s_poll_fails >= APP_POLL_FAIL_CHECK) {
                present = usbdev_present(a->dev);
            }

            if (app_poll_failure_fatal(rc, s_poll_fails, present)) {
                app_log(a, APP_LOG_ERROR, present
                        ? "device stopped responding (%s), releasing it"
                        : "device unplugged (%s)", libusb_error_name(rc));
                app_disconnect(a);
                return;
            }
            return;         /* transient; try again next frame */
        }

        s_poll_fails = 0;
    }

    apply_dirty_volumes(a);
}

void app_set_led(app_t *a)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"set\":{\"led\":\"%06X\"}}",
             (unsigned)app_rgb(a));
    app_send_json(a, json);
}

void app_send_slider(app_t *a, int id, int value)
{
    char json[96];
    snprintf(json, sizeof(json),
             "{\"set\":{\"slider\":{\"id\":%d,\"value\":%d}}}", id, value);
    app_send_json(a, json);
}

/*
 * Faders are linear but loudness is not: a straight 0..1 mapping puts almost
 * all of the audible change in the bottom of the travel. Cubing approximates
 * the taper the system mixer applies, so the fader feels even.
 */
static float slider_to_gain(int value, int max)
{
    float t = (max > 0) ? (float)value / (float)max : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * t;
}

void app_apply_volume(app_t *a, int id)
{
    if (id < 0 || id >= APP_FADER_COUNT || !volume_available()) {
        return;
    }

    float gain = slider_to_gain(a->sliders[id].value, APP_FADER_MAX);

    /* Handed to the worker and applied there: the audio stack can take
       seconds to answer, and the interface must not wait for it. */
    /* Initialised because a fader with no applications hands the worker an
       array it never reads; the compiler cannot see that far and reports it
       as passing something uninitialised. */
    const char *names[CONFIG_APPS_MAX] = { NULL };
    int count = a->sliders[id].app_count;

    if (count > CONFIG_APPS_MAX) {
        count = CONFIG_APPS_MAX;
    }
    for (int i = 0; i < count; i++) {
        names[i] = a->sliders[id].apps[i].name;
    }

    volume_set(id, names, count, gain);

    /*
     * An application that is not playing anything has no session to set, which
     * is normal. A fader whose whole list matches nothing is the usual reason
     * it appears to do nothing at all, so that is worth saying once. The
     * answer comes from the worker's last pass, so it trails a movement.
     */
    bool unmatched = (count > 0 && !volume_matched(id));

    if (unmatched && !a->slider_unmatched[id]) {
        app_log(a, APP_LOG_ERROR,
                "%s: no audio session matches its applications",
                a->sliders[id].name);
    }
    a->slider_unmatched[id] = unmatched;
}

bool app_key_press(app_t *a, int id)
{
    const keys_binding_t *binding = keys_binding_const(&a->keys, id, a->keys.profile);

    if (binding == NULL ||
        (binding->macro.count == 0 && binding->macro.text[0] == 0 &&
         binding->macro.media[0] == 0)) {
        return false;
    }

    /*
     * A media target presses nothing, so it goes out whether or not keys can
     * be synthesised: a desktop with no XTEST can still tell a player to
     * pause. It rides the same queue because the search it does is slow.
     */
    if (binding->macro.media[0] != 0) {
        if (!keysend_play_media(binding->macro.media)) {
            app_log(a, APP_LOG_ERROR, "macro queue is full");
            return false;
        }
        a->key_press_at[id]++;
        app_log(a, APP_LOG_EVENT, "key %d: play/pause %s", id,
                binding->macro.media);
        return true;
    }

    if (!keysend_available()) {
        app_log(a, APP_LOG_ERROR, "macros disabled: %s", keysend_last_error());
        return false;
    }

    bool queued;

    if (binding->macro.count == 0) {
        queued = keysend_play_text(binding->macro.text,
                                   binding->macro.timings[0]);
    } else {
        const char *cmds[KEYS_MACRO_MAX];

        for (int i = 0; i < binding->macro.count; i++) {
            cmds[i] = binding->macro.cmds[i];
        }
        /* Aimed at one program, or at whatever is in front. */
        if (binding->macro.target[0] != 0) {
            queued = keysend_play_to(binding->macro.target, cmds,
                                     binding->macro.timings,
                                     binding->macro.count);
        } else {
            queued = keysend_play(cmds, binding->macro.timings,
                                  binding->macro.count);
        }
    }

    if (!queued) {
        app_log(a, APP_LOG_ERROR, "macro queue is full");
        return false;
    }

    a->key_press_at[id]++;

    const char *what = binding->name[0] ? binding->name
                     : (binding->macro.count > 0 ? binding->macro.cmds[0]
                                                 : binding->macro.text);

    app_log(a, APP_LOG_EVENT, "key %d: %s", id, what);
    return true;
}

/* Serialise @p root, send it, and dispose of it either way. */
static void app_send_cjson(app_t *a, cJSON *root, const char *what)
{
    char *text = (root != NULL) ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);

    if (text == NULL) {
        app_log(a, APP_LOG_ERROR, "could not build the %s command", what);
        return;
    }

    app_send_json(a, text);
    cJSON_free(text);
}

void app_reply_slider(app_t *a, int id)
{
    if (id < 0 || id >= APP_FADER_COUNT) {
        app_log(a, APP_LOG_ERROR, "slider %d out of range", id);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *set = (root != NULL) ? cJSON_AddObjectToObject(root, "set") : NULL;
    cJSON *slider = (set != NULL) ? cJSON_AddObjectToObject(set, "slider") : NULL;

    if (slider == NULL ||
        cJSON_AddNumberToObject(slider, "id", id) == NULL ||
        cJSON_AddNumberToObject(slider, "value", a->sliders[id].value) == NULL ||
        cJSON_AddStringToObject(slider, "name", a->sliders[id].name) == NULL ||
        cJSON_AddBoolToObject(slider, "update", a->slider_pending[id]) == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }

    app_send_cjson(a, root, "slider");
    a->slider_pending[id] = false;
}

void app_get(app_t *a, const char *what)
{
    cJSON *root = cJSON_CreateObject();

    if (root != NULL && cJSON_AddStringToObject(root, "get", what) == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    app_send_cjson(a, root, "get");
}

void app_set_led_states(app_t *a)
{
    cJSON *root   = cJSON_CreateObject();
    cJSON *set    = (root != NULL) ? cJSON_AddObjectToObject(root, "set") : NULL;
    cJSON *config = (set != NULL) ? cJSON_AddObjectToObject(set, "config") : NULL;
    cJSON *led    = (config != NULL) ? cJSON_AddObjectToObject(config, "led") : NULL;

    for (int i = 0; led != NULL && i < APP_LED_SLOT_COUNT; i++) {
        uint32_t rgb;

        /* Half a colour is not worth sending: the firmware would reject the
           whole object and the other three would go with it. */
        if (!keys_parse_hex(a->led_hex[i], &rgb)) {
            app_log(a, APP_LOG_ERROR, "%s is not a colour: \"%s\"",
                    LED_SLOTS[i].label, a->led_hex[i]);
            cJSON_Delete(root);
            return;
        }

        char hex[APP_HEX_MAX];
        app_format_hex(rgb, hex, sizeof(hex));

        if (cJSON_AddStringToObject(led, LED_SLOTS[i].key, hex) == NULL) {
            led = NULL;
        }
    }

    /* Sent with them, so one write settles what the device shows and how
       brightly it shows it. */
    if (led != NULL &&
        cJSON_AddNumberToObject(config, "brightness",
                                lroundf(a->led_brightness * 100.0f)) == NULL) {
        led = NULL;
    }

    if (led == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    app_send_cjson(a, root, "led states");
}

