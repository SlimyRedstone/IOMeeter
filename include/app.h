/*
 * Application state shared between the GUI and the USB transport.
 *
 * Holds the connection, the colour currently being edited, the text buffers the
 * UI types into, and a ring buffer of traffic. The USB side is polled with a
 * near-zero timeout so a frame never blocks on the device.
 */

#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Deliberately no #include "usbdev.h": that pulls in libusb.h, which on Windows
 * pulls in windows.h, whose Rectangle/CloseWindow/ShowCursor collide with
 * raylib. The UI includes this header, so the device stays behind a pointer.
 */
typedef struct usbdev usbdev_t;

#include "config.h"
#include "proto.h"

#define APP_LOG_CAPACITY   256
#define APP_LOG_TEXT_MAX   200

#define APP_HEX_MAX        8

/*
 * The colours the controller keeps for itself, one per state it can be in.
 * Ordered as the firmware declares them (config_led_state_t), and stored on
 * the device rather than here: they have to be right before a host has said
 * anything, which is the whole point of the boot and disconnected colours.
 */
#define APP_LED_SLOT_COUNT 4

typedef struct {
    const char *key;        /*!< its name inside the "led" object     */
    const char *label;
    const char *hint;       /*!< when the controller shows it         */
    uint32_t    def;        /*!< what the firmware falls back to      */
} app_led_slot_t;

/** One slot's description, or NULL when @p index is out of range. */
const app_led_slot_t *app_led_slot(int index);

/* What the charge reads as before the device has said anything. */
#define APP_BATTERY_DEFAULT 69.96f

/* Four faders, 12-bit like the DAC range they are meant to drive. */
#define APP_FADER_COUNT    4
#define APP_FADER_MAX      4095

/* Written beside the executable, next to resources/. */
#define APP_CONFIG_PATH    "config.json"

typedef enum {
    APP_LOG_TX,        /*!< something we sent            */
    APP_LOG_RX,        /*!< a reply                      */
    APP_LOG_EVENT,     /*!< unprompted device traffic    */
    APP_LOG_ERROR,
} app_log_kind_t;

typedef struct {
    app_log_kind_t kind;
    char           text[APP_LOG_TEXT_MAX];
} app_log_entry_t;

typedef struct {
    usbdev_t *dev;
    bool      connected;

    /* Friendly name of whichever variant is attached, or "" when none is.
       On the dongle it also says whether the controller is on the far end. */
    char      device_name[32];

    /*
     * Whether the controller is reachable. Always true on a direct connection,
     * where the controller is the device itself; on the dongle it follows the
     * {"pair":{"status":...}} reports, the dongle being a fixture that stays
     * in its port while the controller comes and goes.
     */
    bool      controller_linked;

    /* The reconnect attempt runs every few seconds, so a failure is only
       worth logging when it is the first since the last success. */
    bool      connect_failure_logged;

    /* Reassembles the IN endpoint, which is a byte stream rather than a
       sequence of messages. */
    proto_framer_t framer;

    /* Colour being edited, kept as HSV so the wheel and the brightness slider
       stay independent. */
    float hue;          /*!< 0..360 */
    float sat;          /*!< 0..1   */
    float val;          /*!< 0..1   */

    char hex[APP_HEX_MAX];

    /* RRGGBB per state slot, as the controller stores them: read back with
       {"get":"config"} and written as one "config" command. */
    char led_hex[APP_LED_SLOT_COUNT][APP_HEX_MAX];

    /*
     * What every one of those is shown at, 0 to 1. Held here as a fraction to
     * match the wheel's own brightness and the slider that sets it; the
     * controller stores it as a percentage, which is the only place it is
     * counted that way.
     */
    float led_brightness;

    /* Fader values (0..APP_FADER_MAX, bottom to top) and their names. */
    config_slider_t sliders[APP_FADER_COUNT];

    /* The macro pad: thirty keys per profile, and which profile is live. */
    keys_t keys;

    /* Set when a fader moves or is renamed; the UI flushes it periodically so
       a drag does not write the file on every frame. */
    bool config_dirty;

    /* Where the configuration was last loaded from or saved to, which is
       what "Reload config" re-reads. Defaults to APP_CONFIG_PATH. */
    char config_path[CONFIG_PATH_MAX];

    /* From the "debug" key. When false the traffic console is hidden. */
    config_opts_t opts;

    /*
     * Charge the device last reported, as a percentage. The starting value is
     * the only fractional one there is: the device sends whole numbers, so a
     * reading that is not 69.96 has come from {"battery":N} and is exact.
     */
    float battery;

    /*
     * Rises each time the device reports a slider move. The UI watches it and
     * locks its own sliders for a moment, so a fader being moved on the device
     * is not fought by the pointer.
     */
    unsigned long slider_extern_seq;

    /* Per fader, so the interface can tell which one the device moved
       rather than only that something moved. */
    unsigned long slider_extern_at[APP_FADER_COUNT];

    /*
     * Rises each time a key's macro is played. The interface watches it and
     * lights the key for a moment; keeping a count rather than a time means
     * nothing here needs a clock, and a press from anywhere -- a click today,
     * the pad itself later -- lights it the same way.
     */
    unsigned long key_press_at[KEYS_COUNT];

    /*
     * Set when the user moves a fader, cleared once the device has been told.
     * Reported as "update" so the device knows whether the value it just asked
     * for is newer than the one it holds.
     */
    bool slider_pending[APP_FADER_COUNT];

    /* Set when an inbound update changed a value; the volume is pushed once at
       the end of the poll rather than per packet. */
    bool slider_volume_dirty[APP_FADER_COUNT];

    /* Latches once a fader's applications match no audio session, so the
       warning is written when that starts rather than on every frame. */
    bool slider_unmatched[APP_FADER_COUNT];

    bool live_send;

    /* Ring buffer; oldest entry is dropped once it fills. */
    app_log_entry_t log[APP_LOG_CAPACITY];
    int             log_count;
    int             log_first;

    /* Rises on every appended line. log_count stops changing once the ring is
       full, so it cannot be used to detect new traffic. */
    unsigned long   log_seq;

    /* Most recent notification. The UI watches notice_seq for changes and runs
       its own timer, so nothing here depends on a clock. */
    char            notice[APP_LOG_TEXT_MAX];
    unsigned long   notice_seq;
} app_t;

void app_init(app_t *a);
void app_shutdown(app_t *a);

bool app_connect(app_t *a);
void app_disconnect(app_t *a);

void app_poll(app_t *a);

void app_send_json(app_t *a, const char *json);

void app_set_led(app_t *a);

/**
 * Send the four state colours and the level they are shown at, as
 * {"set":{"config":{"led":{...},"brightness":N}}}.
 *
 * The controller writes them to its own storage, so they survive a reboot and
 * are what it shows before any host has spoken to it. Nothing is sent if one
 * of them is not a colour.
 */
void app_set_led_states(app_t *a);

/**
 * Send {"set":{"slider":{"id":N,"value":V}}}.
 *
 * @param id    Slider index.
 * @param value New position.
 */
void app_send_slider(app_t *a, int id, int value);

/**
 * Answer {"get":{"slider":{"id":N}}} with the slider's full state.
 *
 * @param id Slider index; ignored if out of range.
 */
void app_reply_slider(app_t *a, int id);

/**
 * Push a slider's value to every application it controls.
 *
 * @param id Slider index; ignored if out of range.
 */
void app_apply_volume(app_t *a, int id);
/**
 * Play the macro bound to a key under the active profile.
 *
 * Returns as soon as the macro is queued; keysend.h plays it on its own
 * thread, so a long one does not hold up the frame.
 *
 * @param id Key index; ignored if out of range.
 * @return false when nothing is bound, or when macros are unavailable.
 */
bool app_key_press(app_t *a, int id);

void app_get(app_t *a, const char *what);

void app_config_load(app_t *a);

void app_config_save(app_t *a);

bool app_config_save_as(app_t *a, const char *path);

bool app_config_load_from(app_t *a, const char *path);

/**
 * Whether a {"pair":{"status":S}} report says the radio link is down.
 *
 * @param status The reported state, or NULL when the packet carried none.
 */
bool app_pair_link_lost(const char *status);

/** Whether the same report says the link is up. */
bool app_pair_link_up(const char *status);

/**
 * Whether a run of receive failures means the connection should be dropped.
 *
 * Pulled out of the poll loop so the policy can be exercised without a device
 * on the bus.
 *
 * @param rc      the libusb code the transfer failed with; never 0 and never
 *                LIBUSB_ERROR_TIMEOUT, both of which are successes here.
 * @param fails   consecutive failures so far, this one included.
 * @param present what usbdev_present() says, or true when it was not asked.
 * @return true to release the device.
 */
bool app_poll_failure_fatal(int rc, int fails, bool present);

/** Failures before the bus is asked whether the device is still there. */
#define APP_POLL_FAIL_CHECK 3

/** Failures after which the device is released even if the bus still lists it. */
#define APP_POLL_FAIL_GIVEUP 60

/**
 * Put the configuration on the controller, where it follows the hardware.
 *
 * Does nothing while disconnected. The transfer runs on the poll loop, so this
 * returns before it has finished; the log says how it went.
 */
void app_config_push(app_t *a);

/** Re-read a->config_path, discarding anything changed since. */
bool app_config_reload(app_t *a);

void app_notify(app_t *a, const char *text);

/** Append one line to the log, printf style. */
void app_log(app_t *a, app_log_kind_t kind, const char *fmt, ...);
void app_log_clear(app_t *a);

const app_log_entry_t *app_log_at(const app_t *a, int index);

/**
 * Format the charge for the badge: whole numbers plain, the starting value
 * with its decimals.
 *
 * @param out  Receives text such as "100%" or "69.96%".
 * @param size Capacity of @p out.
 */
void app_battery_text(const app_t *a, char *out, size_t size);

uint32_t app_rgb(const app_t *a);

void app_set_rgb(app_t *a, uint32_t rgb);

void app_sync_hex(app_t *a);

void app_hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b);
void app_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, float *h, float *s, float *v);

#endif /* APP_H */
