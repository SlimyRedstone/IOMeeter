/*
 * Toggling one application's playback, rather than whatever played last.
 *
 * A media key is global. Windows hands VK_MEDIA_PLAY_PAUSE to whichever
 * program owns the system media session, and the desktops do much the same, so
 * a pad key that sends one cannot say whether it means the browser or the
 * music player. This addresses an application by name instead, which is the
 * only way to pause one of them and leave the other playing.
 *
 * One API, two implementations chosen at compile time:
 *
 *   Windows  The media session the application publishes -- the same list the
 *            volume flyout shows -- told to toggle itself. Sending the window
 *            a WM_APPCOMMAND instead does not work: a window that does not
 *            handle it passes it to DefWindowProc, which hands it to the
 *            shell, which turns it back into the global key that toggles
 *            whatever played last. An application that publishes no session
 *            cannot be reached, and the call says which ones do.
 *
 *   Linux    MPRIS over D-Bus, which is what desktop media players publish and
 *            what every "previous/next" applet already drives. Spoken through
 *            dbus-send rather than by linking a D-Bus client, so this needs
 *            nothing at build time that the desktop does not already ship.
 *
 * Applications are named by the executable, the same identity the faders use:
 * "chrome" and "chrome.exe" both mean the same program, and the match ignores
 * case and any path in front.
 *
 * Every call blocks -- finding the window means asking every program on the
 * desktop, and an application that is busy can take its time answering. It
 * therefore runs on keysend's worker thread and nowhere else, in the same way
 * mixer.h runs on volume.h's.
 */

#ifndef MEDIA_H
#define MEDIA_H

#include <stdbool.h>

/** Longest application name this will take, matching keys.h. */
#define MEDIA_NAME_MAX 64

/** Longest explanation media_last_error() will give back. */
#define MEDIA_ERROR_MAX 200

/**
 * Whether this build can address an application at all.
 *
 * False says the platform has no way in -- no XTEST-style fallback exists for
 * this -- and every toggle will fail for the same reason.
 */
bool media_available(void);

/** Why the last call failed, or "" when it did not. */
const char *media_last_error(void);

/**
 * How many toggles have failed since the program started.
 *
 * The work happens on a worker thread, so a failure cannot be handed back to
 * whoever pressed the key. The interface watches this count instead and
 * reports media_last_error() whenever it moves, the same way it watches a
 * transfer settle.
 */
unsigned media_failures(void);

/**
 * Start or stop @p app playing, leaving every other program alone.
 *
 * @param app Executable name or path. Only the file name is used.
 * @return false when nothing of that name is running, when it is running but
 *         will not take the command, or when the platform cannot send one.
 *         media_last_error() says which.
 */
bool media_toggle(const char *app);

#endif /* MEDIA_H */
