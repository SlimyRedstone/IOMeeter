/*
 * Clay + raylib front end, laid out to mirror host/web/index.html.
 *
 * A header with a connection badge, then a tab bar: "Main" carries the fader
 * strip, "Configuration" the NeoPixel card with its colour wheel and
 * brightness. The traffic log sits below both, and only when "debug" is set.
 */

#ifndef UI_H
#define UI_H

#include "app.h"

/*
 * The heights the layout was designed around, quoted for a full-size fader
 * track. The window no longer opens at these -- it opens at the minimum below
 * -- but the difference between them and FADER_TRACK_HEIGHT is what everything
 * other than the strip needs, which is how ui_fader_height() shares out the
 * real window. The traffic console only exists when "debug" is set, hence two.
 */
#define UI_WINDOW_HEIGHT_DEBUG  900
#define UI_WINDOW_HEIGHT_PLAIN  750

/*
 * The floor the window will not go below. One figure whatever is on show:
 * the traffic console takes the room the cards above it have left rather than
 * asking for more, so turning it on never moves the window.
 */
#define UI_MIN_HEIGHT     680

/* Tab strip between the header and the tab's own containers. Counted in the
   two heights above, since it is chrome the fader strip does not get. */
#define UI_TAB_BAR_HEIGHT  44

#define UI_WINDOW_HEIGHT_FOR(debug)     ((debug) ? UI_WINDOW_HEIGHT_DEBUG : UI_WINDOW_HEIGHT_PLAIN)

/* The interface runs at the monitor's refresh rate. Used only when the driver
   does not report one. */
#define UI_FALLBACK_FPS   60

/* How often the bus is swept for a device while none is connected. */
#define UI_CONNECT_SCAN_SECONDS 5.0

/* Minimum the layout stays usable at, and the size it opens at. */
#define UI_WINDOW_MIN_WIDTH   1000
#define UI_WINDOW_MIN_HEIGHT  UI_MIN_HEIGHT

/**
 * Open the window, run the event loop until it is closed, then clean up.
 * Returns the process exit status.
 */
int ui_run(app_t *app);

#endif /* UI_H */
