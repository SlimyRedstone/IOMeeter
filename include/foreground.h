/*
 * Which desktop applications are running, and which one the user is looking
 * at.
 *
 * "Desktop" means a program with a window of its own on screen. Services,
 * COM surrogates and the shell's own hidden windows are all filtered out, so
 * what comes back is the list a person would recognise from their taskbar,
 * one entry per executable however many windows it has open.
 *
 * One API, two implementations chosen at compile time: the window list on
 * Windows, the EWMH properties an X11 window manager publishes elsewhere. A
 * native Wayland session has no equivalent any unprivileged process may read,
 * the same limit keysend.h runs into.
 *
 * Applications are matched on executable basename, the same way mixer.h does,
 * so one configuration recognises the same program on both systems. They are
 * shown by the name their author gave them -- "Google Chrome" rather than
 * "chrome.exe" -- taken from the version resource on Windows and from the
 * window class on X11, since neither basenames nor window titles are what a
 * person would call the program.
 *
 * Not thread safe; call from one thread. Enumerating touches every window on
 * the desktop, so poll it rather than calling it every frame.
 */

#ifndef FOREGROUND_H
#define FOREGROUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOREGROUND_NAME_MAX  64
#define FOREGROUND_TITLE_MAX 96
#define FOREGROUND_MAX       48

typedef struct {
    char process[FOREGROUND_NAME_MAX];   /*!< executable basename, matched on */
    char name[FOREGROUND_TITLE_MAX];     /*!< what to call it on screen */
    char title[FOREGROUND_TITLE_MAX];    /*!< window caption, to tell copies
                                              of one program apart */
} foreground_app_t;

/**
 * Connect to the window system.
 *
 * @return false when the window list cannot be read, after which every other
 *         call is a no-op and foreground_last_error() says why.
 */
bool foreground_init(void);

void foreground_shutdown(void);

bool foreground_available(void);

/** Why the window list is unavailable, for logging. Never NULL. */
const char *foreground_last_error(void);

/**
 * List the applications that have a window on screen.
 *
 * Deduplicated by executable, so a browser with six windows appears once.
 * This program is left out of its own list.
 *
 * @param out Receives up to @p max applications.
 * @param max Capacity of @p out.
 * @return Number written, or 0.
 */
int foreground_list(foreground_app_t *out, int max);

/**
 * Executable basename of the window currently in front.
 *
 * This program does not count as being in front of itself: while its own
 * window has focus the answer is "nothing", so a caller acting on whatever is
 * in front does not act on the fact that the user is configuring it.
 *
 * @param out  Receives the basename.
 * @param size Capacity of @p out.
 * @return false when nothing is in front, the window belongs to us, or the
 *         owner cannot be read, in which case @p out is left empty.
 */
bool foreground_active(char *out, size_t size);

/**
 * A window belonging to @p app, for sending it something directly.
 *
 * The main window is preferred -- the one on the taskbar -- falling back to
 * any window the program owns, because a player in full screen may have
 * dropped its caption and stopped looking like a main window.
 *
 * @param app Executable name or path. Only the file name is compared, without
 *            its extension, so "vlc" and "vlc.exe" both find VLC.
 * @return An HWND on Windows and an X11 Window elsewhere, widened to carry
 *         either; 0 when nothing of that name has a window. Only keysend.h
 *         uses it, to aim a keystroke at one program.
 */
uintptr_t foreground_window_for(const char *app);

#endif /* FOREGROUND_H */
