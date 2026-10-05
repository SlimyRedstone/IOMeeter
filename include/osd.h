/*
 * The on-screen display: what the pad just did, shown over everything else.
 *
 * The point of it is the case where IOMeeter has no window at all. A key on
 * the surface changes something on this machine, and with the interface in the
 * tray there is nothing to say which key, or where a fader ended up. This puts
 * that on the screen for a second and takes it away again.
 *
 * It is a window of its own, not part of the interface: raylib owns exactly
 * one window and the overlay has to outlive that one being hidden. One API,
 * two implementations:
 *
 *   Windows  A layered, topmost, click-through tool window, painted with GDI.
 *            WS_EX_TRANSPARENT is what lets the pointer through it, and
 *            WS_EX_NOACTIVATE stops it stealing the focus from a game.
 *
 *   X11      An override-redirect window the window manager is told not to
 *            manage, typed as a notification and kept above. Input passes
 *            through it by way of an empty shape input region.
 *
 * Over a *borderless* fullscreen window -- which is what games are set to when
 * they say "fullscreen" these days -- a topmost window draws on top, so this
 * works. Over genuinely exclusive fullscreen it cannot: the display is the
 * application's and nothing composites with it. Reaching that needs the
 * graphics API hooked the way Steam and Discord do, which is a different
 * project.
 *
 * Everything here is called from the main loop and from nowhere else. A window
 * belongs to the thread that created it, and the interface's own loop already
 * pumps every window on its thread, hidden to the tray or not, so there is no
 * second thread and nothing to lock.
 */

#ifndef OSD_H
#define OSD_H

#include <stdbool.h>

/* Longest line the panel will show. A fader's name is shorter than this and a
   key's name is capped at KEYS_NAME_MAX, so nothing is ever cut off. */
#define OSD_TEXT_MAX 64

/*
 * Where on the screen the panel sits.
 *
 * The order is the order the interface offers them in, and the names are what
 * config.json stores, so neither can be rearranged without the other.
 */
typedef enum {
    OSD_TOP_LEFT = 0,
    OSD_TOP_CENTER,
    OSD_TOP_RIGHT,
    OSD_BOTTOM_LEFT,
    OSD_BOTTOM_CENTER,
    OSD_BOTTOM_RIGHT,
    OSD_POSITION_COUNT
} osd_position_t;

/* How long the panel stays up, in milliseconds. A value outside the range is
   brought into it rather than rejected, the way a macro's timing is. */
#define OSD_MS_MIN     200
#define OSD_MS_MAX     10000
#define OSD_MS_DEFAULT 1500

/** Short label for @p where, as the interface shows it. Never NULL. */
const char *osd_position_label(osd_position_t where);

/** The name @p where is written under in config.json. Never NULL. */
const char *osd_position_id(osd_position_t where);

/** Read a position back from its stored name. Anything unknown is top right. */
osd_position_t osd_position_parse(const char *name);

/** @p ms brought into OSD_MS_MIN..OSD_MS_MAX. */
int osd_clamp_ms(int ms);

/**
 * Create the window, hidden.
 *
 * @return false when the platform has no way to put a window over everything
 *         else, after which every call here is a no-op and osd_last_error()
 *         says why.
 */
bool osd_start(void);

void osd_stop(void);

bool osd_available(void);

/** Why the overlay is unavailable, for logging. Never NULL. */
const char *osd_last_error(void);

/**
 * The settings the interface owns, pushed down rather than read from here.
 *
 * Cheap enough to call every frame, which is what the main loop does: the
 * checkbox, a configuration reloaded from the file and a document arriving
 * from the controller all change these, and one store covers all three.
 */
void osd_configure(bool enabled, osd_position_t where, int ms);

/**
 * Show one line, for the configured time.
 *
 * @param title What was pressed, in the user's own words where there are any.
 */
void osd_show_text(const char *title);

/**
 * Show a fader: its name, where it sits, and a bar.
 *
 * @param name  The fader's name.
 * @param value Position, 0..@p max.
 * @param max   Top of the fader's travel.
 */
void osd_show_level(const char *name, int value, int max);

/**
 * Take the panel away once its time is up.
 *
 * Called every pass of the main loop, including the passes where the interface
 * is hidden and draws nothing: the overlay is the half of the program that
 * still has something to say then.
 */
void osd_tick(void);

/**
 * Whether the panel is on the screen right now.
 *
 * For the main loop's own pacing: with the interface hidden there is nothing
 * to draw and it idles, but the overlay is still following a fader, so it has
 * to run at the panel's rate for as long as this says yes.
 */
bool osd_visible(void);

#endif /* OSD_H */
