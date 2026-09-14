/*
 * The configuration, kept on the controller instead of only on this machine.
 *
 * The device stores the document as bytes it never reads, so plugging the
 * hardware into another computer brings the fader strip, the profiles and the
 * macro pad with it. The web page at host/web writes the same document through
 * the same commands, which is how it can configure the controller with this
 * program closed.
 *
 * The document is far larger than one JSON buffer, so it moves in slices:
 *
 *   {"set":{"host":{"begin":8502}}}     open a transfer
 *   {"set":{"host":{"text":"..."}}}     append, repeated
 *   {"set":{"host":{"commit":true}}}    replace what is stored
 *   {"get":"host"}                      -> {"host":{"size":8502}}
 *   {"get":{"host":{"off":1024}}}       -> {"host":{"off":..,"text":..,"eof":..}}
 *
 * The transport here is asynchronous -- replies arrive on the poll loop -- so
 * this is a state machine rather than a function that blocks. Start a transfer,
 * feed it replies from the packet handler, and drive it from the poll loop
 * until it reports that it has finished.
 *
 * One transfer at a time, and not thread safe: everything runs on the thread
 * that owns the poll loop.
 */

#ifndef DEVCFG_H
#define DEVCFG_H

#include <stdbool.h>
#include <stddef.h>

#include "app.h"

struct cJSON;

typedef enum {
    DEVCFG_IDLE = 0,    /*!< nothing in progress                       */
    DEVCFG_BUSY,        /*!< a transfer is under way                   */
    DEVCFG_DONE,        /*!< it finished; a pull's document is ready   */
    DEVCFG_FAILED,      /*!< it did not; devcfg_error() says why       */
} devcfg_state_t;

/** Abandon whatever is in progress and release its buffer. */
void devcfg_reset(void);

/**
 * Begin reading the stored document.
 *
 * @return false if a transfer is already running or the command could not be
 *         sent. A device holding nothing still completes, with a zero-length
 *         document.
 */
bool devcfg_pull(app_t *a);

/**
 * Begin writing @p text, which is copied, so the caller may free it at once.
 *
 * @param len Bytes of @p text, not characters.
 */
bool devcfg_push(app_t *a, const char *text, size_t len);

/**
 * Offer a decoded reply to the transfer in progress.
 *
 * @return true when the reply belonged to it and the caller should not handle
 *         it again.
 */
bool devcfg_on_message(app_t *a, const struct cJSON *root);

/** Send the next step, and give up on a transfer that has gone quiet. */
void devcfg_tick(app_t *a);

devcfg_state_t devcfg_state(void);

/** Why the last transfer failed. Never NULL. */
const char *devcfg_error(void);

/**
 * The document a completed pull read.
 *
 * NUL-terminated, owned here, and valid until the next transfer starts.
 *
 * @param out_len Receives its length in bytes. May be NULL.
 * @return NULL unless a pull has finished.
 */
const char *devcfg_document(size_t *out_len);

#endif /* DEVCFG_H */
