/*
 * Applying fader positions to the system mixer, off the interface's thread.
 *
 * mixer.h talks to Core Audio or PulseAudio, and either can block for seconds
 * at a time: a virtual audio driver that is slow to answer stalls the COM call
 * inside it. Doing that on the render thread stops the window pumping
 * messages, which is what Windows reports as "not responding" -- the loop is
 * not broken, it is waiting.
 *
 * So a worker thread owns the mixer entirely. The interface posts the gain it
 * wants and returns immediately; the worker applies it whenever the audio
 * stack lets it. Positions are coalesced, so a fader moving faster than the
 * mixer can keep up costs one write per settled value rather than a backlog.
 *
 * COM apartments are per-thread, so mixer_init() and mixer_shutdown() run on
 * that same worker; nothing else may call mixer.h directly.
 */

#ifndef VOLUME_H
#define VOLUME_H

#include <stdbool.h>

#define VOLUME_SLIDERS  8
#define VOLUME_APPS     16
#define VOLUME_NAME_MAX 64

/**
 * Start the worker and connect to the system mixer on it.
 *
 * Blocks until the mixer has reported success or failure, so the caller can
 * log the outcome, but no later call blocks.
 *
 * @return what mixer_init() returned on the worker.
 */
bool volume_start(void);

/** Stop the worker and disconnect. Safe to call when never started. */
void volume_stop(void);

/** Whether the mixer connected. False makes volume_set() a no-op. */
bool volume_available(void);

/**
 * Ask for @p gain on every application named.
 *
 * Returns at once. A pending position for the same slider is replaced rather
 * than queued.
 *
 * @param slider Index below VOLUME_SLIDERS.
 * @param names  Executable basenames; copied, so the caller keeps ownership.
 * @param count  How many, capped at VOLUME_APPS.
 * @param gain   0..1.
 */
void volume_set(int slider, const char *const *names, int count, float gain);

/**
 * Whether the last applied position for @p slider reached any audio session.
 *
 * Reflects the worker's most recent pass, so it lags a fader movement by
 * however long the mixer took.
 */
bool volume_matched(int slider);

/**
 * Why the mixer is unavailable, for logging. Never NULL.
 *
 * Only meaningful once volume_start() has returned, at which point the worker
 * has finished connecting and the text stops changing.
 */
const char *volume_last_error(void);

#endif /* VOLUME_H */
