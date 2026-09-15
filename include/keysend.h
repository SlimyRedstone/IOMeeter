/*
 * Synthesised keyboard input, for the macro pad.
 *
 * One API, two implementations chosen at compile time: SendInput on Windows,
 * the XTEST extension elsewhere. XTEST talks to the X server, so it also
 * covers XWayland clients but not a native Wayland session, which has no
 * equivalent an unprivileged process may use.
 *
 * Keys are named rather than numbered, because a configuration file has to
 * survive being carried between the two: "CTRL_LEFT" means the same thing on
 * both, while a scan code does not. keysend_names() lists the names worth
 * offering; a few older spellings are still understood without being listed,
 * so a configuration written before they were renamed still plays.
 *
 * A chord is played by keysend_play(). Every step waits its own timing, is
 * pressed, and once the list is exhausted they are released in the reverse
 * order, so {"cmds":["CTRL_LEFT","SHIFT_LEFT","M"]} arrives as Ctrl+Shift+M.
 *
 * A phrase goes through keysend_play_text() instead. Windows enters it through
 * the clipboard, pasting being the only reliable way into an arbitrary
 * program, and puts the previous contents back afterwards. Elsewhere the
 * characters are typed straight out through XTEST, which needs no clipboard
 * and so cannot disturb it: owning an X selection means servicing requests for
 * it, which a worker thread has no event loop to do.
 *
 * Playback happens on a worker thread. A macro of any length therefore costs
 * the caller nothing: the interface must never sit through the timings.
 */

#ifndef KEYSEND_H
#define KEYSEND_H

#include <stdbool.h>

/* For media_action_t: a media macro presses no key, but it rides this queue. */
#include "media.h"

/* Steps in one chord, the longest key name, and the longest phrase. All three
   match the limits keys.h stores, which is where every macro comes from. */
#define KEYSEND_STEPS_MAX 16
#define KEYSEND_NAME_MAX  24
#define KEYSEND_TEXT_MAX  128

/* An application named as a macro's target, matching keys.h. */
#define KEYSEND_APP_MAX   64

/**
 * Start the playback thread and open whatever the platform needs.
 *
 * @return false when input cannot be synthesised, after which keysend_play()
 *         is a no-op and keysend_last_error() says why.
 */
bool keysend_start(void);

void keysend_stop(void);

bool keysend_available(void);

/** Why playback is unavailable, for logging. Never NULL. */
const char *keysend_last_error(void);

/**
 * Queue a macro and return immediately.
 *
 * @param cmds    Key names, pressed in order.
 * @param timings Milliseconds to wait before each press; may be NULL for none.
 * @param count   Number of steps, capped at KEYSEND_STEPS_MAX.
 * @return false when the queue is full or nothing can be sent.
 */
bool keysend_play(const char *const *cmds, const int *timings, int count);

/**
 * Play a chord into one application's window instead of the desktop.
 *
 * The keys are posted to the window rather than synthesised, so they arrive
 * whatever has the focus and nothing else sees them. That has a limit worth
 * knowing: a program reads the state of Ctrl, Shift and Alt from the keyboard
 * itself, not from what it was sent, so a targeted chord carries its modifiers
 * only as far as the program bothers to look. Single keys -- the space bar
 * that pauses VLC -- arrive exactly as intended.
 *
 * @param app Executable name or path; only the file name is matched.
 * @return false when the queue is full or nothing is running yet. Whether the
 *         application was there at all is answered later, through
 *         keysend_failures().
 */
bool keysend_play_to(const char *app, const char *const *cmds,
                     const int *timings, int count);

/**
 * How many macros could not be delivered, and why the last one failed.
 *
 * A macro plays on a worker thread, where there is nobody to tell. The
 * interface watches the count and reports the reason when it moves, as it
 * does for media.h.
 */
unsigned keysend_failures(void);
const char *keysend_failure(void);

/**
 * Drive one application's playback, as a macro with no keystroke in it.
 *
 * Queued like any other macro, because the search it does is slow enough to
 * be worth keeping off the thread that noticed the key.
 *
 * @param app    Executable name or path; see media.h for how it is matched.
 * @param action What the application is told to do.
 * @return false when the queue is full or nothing is running yet.
 */
bool keysend_play_media(const char *app, media_action_t action);

/**
 * Queue a phrase to be typed, and return immediately.
 *
 * @param text     Entered as it stands.
 * @param delay_ms Milliseconds to wait first.
 * @return false when the queue is full, the text is empty, or nothing can be
 *         sent.
 */
bool keysend_play_text(const char *text, int delay_ms);

/** True when @p name is a key this can send. */
bool keysend_known(const char *name);

/**
 * Canonical name for a key the user pressed, for recording a macro.
 *
 * This is the key's position, which only matches its label on a US layout.
 * Prefer keysend_name_of_char() wherever a character is available.
 *
 * @param glfw_key The code raylib reports, which is GLFW's.
 * @return The name, or NULL when that key has none.
 */
const char *keysend_name_of(int glfw_key);

/**
 * Canonical name for a character the keyboard produced.
 *
 * Positions and labels part company on every layout but the US one, where a
 * recorder that reads positions writes down Q for the key marked A. The
 * character the layout actually produced does not have that problem, and it
 * follows a layout switched after the window opened, which the position table
 * does not.
 *
 * @param codepoint What GetCharPressed() returned; case is ignored.
 * @return The name, or NULL when no key here sends that character.
 */
const char *keysend_name_of_char(int codepoint);

/**
 * List the accepted names.
 *
 * @param out Receives pointers to static strings.
 * @param max Capacity of @p out.
 * @return Number written.
 */
int keysend_names(const char **out, int max);

#endif /* KEYSEND_H */
