#include "devcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

/*
 * Bytes of document per message. The device reassembles packets until the
 * whole JSON object parses, and its buffer is 2K; escaping can double a slice
 * on the way out, so this leaves room for that and for the wrapper.
 */
#define SLICE_BYTES 384

/* Matches CONFIG_HOST_MAX in the firmware. */
#define DOCUMENT_MAX (32 * 1024)

/* A transfer that has not moved in this long has lost its device. */
#define QUIET_SECONDS 5.0

typedef enum {
    STEP_NONE = 0,
    STEP_PULL_SIZE,     /* asked for the size          */
    STEP_PULL_SLICE,    /* asking for slices           */
    STEP_PUSH_BEGIN,    /* asked to open the transfer  */
    STEP_PUSH_SLICE,    /* sending slices              */
    STEP_PUSH_COMMIT,   /* asked to commit             */
} step_t;

static devcfg_state_t s_state;
static step_t         s_step;
static char           s_error[128] = "";

static char  *s_doc;        /* the document being read, or the one to write */
static size_t s_len;        /* its length in bytes                          */
static size_t s_cap;
static size_t s_at;         /* how much has been read or sent               */

static clock_t s_last;      /* when the transfer last moved                 */

/* --------------------------------------------------------------- helpers -- */

static void fail(const char *why)
{
    snprintf(s_error, sizeof(s_error), "%s", why);
    s_state = DEVCFG_FAILED;
    s_step = STEP_NONE;
}

static void touch(void)
{
    s_last = clock();
}

static bool grow(size_t need)
{
    if (need <= s_cap) {
        return true;
    }
    if (need > DOCUMENT_MAX + 1) {
        return false;
    }

    size_t want = s_cap ? s_cap : 2048;
    while (want < need) {
        want *= 2;
    }
    if (want > DOCUMENT_MAX + 1) {
        want = DOCUMENT_MAX + 1;
    }

    char *bigger = realloc(s_doc, want);
    if (bigger == NULL) {
        return false;
    }
    s_doc = bigger;
    s_cap = want;
    return true;
}

/*
 * Largest slice starting at @p from that ends on a character boundary. The
 * bytes go into a JSON string, and half a UTF-8 sequence would not survive
 * being printed and parsed again -- one application path with an accent in it
 * is enough to hit this.
 */
static size_t slice_len(const char *text, size_t from, size_t len)
{
    size_t take = len - from;
    if (take <= SLICE_BYTES) {
        return take;
    }
    take = SLICE_BYTES;

    size_t cut = take;
    while (cut > 0 && ((unsigned char)text[from + cut - 1] & 0xC0) == 0x80) {
        cut--;                              /* back over continuation bytes */
    }
    if (cut > 0) {
        unsigned char lead = (unsigned char)text[from + cut - 1];
        size_t need = (lead < 0x80)           ? 1
                    : ((lead & 0xE0) == 0xC0) ? 2
                    : ((lead & 0xF0) == 0xE0) ? 3
                    : ((lead & 0xF8) == 0xF0) ? 4
                                              : 1;
        if ((take - (cut - 1)) < need) {
            take = cut - 1;                 /* leave the whole character */
        }
    }
    return take;
}

/* Sends one object, escaping whatever is inside it. Takes ownership of @p host. */
static bool send_host(app_t *a, cJSON *host)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *set = (root != NULL) ? cJSON_AddObjectToObject(root, "set") : NULL;

    if (set == NULL || !cJSON_AddItemToObject(set, "host", host)) {
        cJSON_Delete(root);
        cJSON_Delete(host);
        return false;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (text == NULL) {
        return false;
    }

    app_send_json(a, text);
    cJSON_free(text);
    touch();
    return true;
}

static bool ask_slice(app_t *a, size_t off)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *get = (root != NULL) ? cJSON_AddObjectToObject(root, "get") : NULL;
    cJSON *host = (get != NULL) ? cJSON_AddObjectToObject(get, "host") : NULL;

    if (host == NULL ||
        cJSON_AddNumberToObject(host, "off", (double)off) == NULL) {
        cJSON_Delete(root);
        return false;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (text == NULL) {
        return false;
    }

    app_send_json(a, text);
    cJSON_free(text);
    touch();
    return true;
}

static bool send_next_slice(app_t *a)
{
    size_t take = slice_len(s_doc, s_at, s_len);
    if (take == 0) {
        fail("a slice came out empty");
        return false;
    }

    char *piece = malloc(take + 1);
    if (piece == NULL) {
        fail("out of memory");
        return false;
    }
    memcpy(piece, s_doc + s_at, take);
    piece[take] = '\0';

    cJSON *host = cJSON_CreateObject();
    bool built = (host != NULL) &&
                 cJSON_AddStringToObject(host, "text", piece) != NULL;
    free(piece);

    if (!built) {
        cJSON_Delete(host);
        fail("out of memory");
        return false;
    }

    if (!send_host(a, host)) {
        fail("could not send a slice");
        return false;
    }

    s_at += take;
    s_step = STEP_PUSH_SLICE;
    return true;
}

/* ------------------------------------------------------------ public API -- */

void devcfg_reset(void)
{
    free(s_doc);
    s_doc = NULL;
    s_len = 0;
    s_cap = 0;
    s_at = 0;
    s_step = STEP_NONE;
    s_state = DEVCFG_IDLE;
    s_error[0] = '\0';
}

bool devcfg_pull(app_t *a)
{
    if (a == NULL || !a->connected || s_state == DEVCFG_BUSY) {
        return false;
    }

    devcfg_reset();
    s_state = DEVCFG_BUSY;
    s_step = STEP_PULL_SIZE;
    touch();

    app_get(a, "host");
    return true;
}

bool devcfg_push(app_t *a, const char *text, size_t len)
{
    if (a == NULL || !a->connected || text == NULL || len == 0 ||
        s_state == DEVCFG_BUSY) {
        return false;
    }
    if (len > DOCUMENT_MAX) {
        devcfg_reset();
        fail("the configuration is larger than the device will take");
        return false;
    }

    devcfg_reset();

    if (!grow(len + 1)) {
        fail("out of memory");
        return false;
    }
    memcpy(s_doc, text, len);
    s_doc[len] = '\0';
    s_len = len;
    s_at = 0;

    s_state = DEVCFG_BUSY;
    s_step = STEP_PUSH_BEGIN;

    cJSON *host = cJSON_CreateObject();
    if (host == NULL ||
        cJSON_AddNumberToObject(host, "begin", (double)len) == NULL) {
        cJSON_Delete(host);
        fail("out of memory");
        return false;
    }
    if (!send_host(a, host)) {
        fail("could not open the transfer");
        return false;
    }
    return true;
}

bool devcfg_on_message(app_t *a, const struct cJSON *root_in)
{
    const cJSON *root = (const cJSON *)root_in;

    if (s_state != DEVCFG_BUSY || root == NULL) {
        return false;
    }

    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");

    /* A refusal ends the transfer whichever half it belonged to. */
    if (cJSON_IsFalse(ok)) {
        const cJSON *why = cJSON_GetObjectItemCaseSensitive(root, "error");
        fail(cJSON_IsString(why) ? why->valuestring : "the device refused");
        return true;
    }

    switch (s_step) {
    case STEP_PULL_SIZE: {
        if (!cJSON_IsObject(host)) {
            return false;
        }
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(host, "size");
        if (!cJSON_IsNumber(size)) {
            fail("the device did not report a size");
            return true;
        }

        long total = (long)size->valuedouble;
        if (total <= 0) {
            /* Nothing stored yet, which is not a failure. */
            if (!grow(1)) {
                fail("out of memory");
                return true;
            }
            s_doc[0] = '\0';
            s_len = 0;
            s_state = DEVCFG_DONE;
            s_step = STEP_NONE;
            return true;
        }
        if (total > DOCUMENT_MAX) {
            fail("the device holds more than this build will read");
            return true;
        }
        if (!grow((size_t)total + 1)) {
            fail("out of memory");
            return true;
        }

        s_len = (size_t)total;
        s_at = 0;
        s_step = STEP_PULL_SLICE;

        if (!ask_slice(a, 0)) {
            fail("could not ask for the first slice");
        }
        return true;
    }

    case STEP_PULL_SLICE: {
        if (!cJSON_IsObject(host)) {
            return false;
        }
        const cJSON *off = cJSON_GetObjectItemCaseSensitive(host, "off");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(host, "text");
        const cJSON *eof = cJSON_GetObjectItemCaseSensitive(host, "eof");

        if (!cJSON_IsString(text) || !cJSON_IsNumber(off)) {
            fail("a slice arrived malformed");
            return true;
        }
        if ((size_t)off->valuedouble != s_at) {
            fail("a slice arrived out of order");
            return true;
        }

        size_t n = strlen(text->valuestring);
        if (s_at + n > s_len) {
            fail("the device sent more than it said it had");
            return true;
        }

        memcpy(s_doc + s_at, text->valuestring, n);
        s_at += n;
        s_doc[s_at] = '\0';
        touch();

        if (cJSON_IsTrue(eof) || s_at >= s_len) {
            s_len = s_at;
            s_state = DEVCFG_DONE;
            s_step = STEP_NONE;
            return true;
        }
        if (n == 0) {
            fail("the device stopped sending");
            return true;
        }

        if (!ask_slice(a, s_at)) {
            fail("could not ask for the next slice");
        }
        return true;
    }

    case STEP_PUSH_BEGIN:
        if (!cJSON_IsTrue(ok)) {
            return false;
        }
        send_next_slice(a);
        return true;

    case STEP_PUSH_SLICE: {
        if (!cJSON_IsTrue(ok)) {
            return false;
        }
        if (s_at < s_len) {
            send_next_slice(a);
            return true;
        }

        cJSON *commit = cJSON_CreateObject();
        if (commit == NULL ||
            cJSON_AddBoolToObject(commit, "commit", 1) == NULL) {
            cJSON_Delete(commit);
            fail("out of memory");
            return true;
        }
        if (!send_host(a, commit)) {
            fail("could not commit");
            return true;
        }
        s_step = STEP_PUSH_COMMIT;
        return true;
    }

    case STEP_PUSH_COMMIT:
        if (!cJSON_IsTrue(ok)) {
            return false;
        }
        s_state = DEVCFG_DONE;
        s_step = STEP_NONE;
        return true;

    case STEP_NONE:
    default:
        return false;
    }
}

void devcfg_tick(app_t *a)
{
    if (s_state != DEVCFG_BUSY) {
        return;
    }
    if (a == NULL || !a->connected) {
        fail("the device went away");
        return;
    }

    double waited = (double)(clock() - s_last) / (double)CLOCKS_PER_SEC;
    if (waited > QUIET_SECONDS) {
        fail("the device stopped answering");
    }
}

devcfg_state_t devcfg_state(void)
{
    return s_state;
}

const char *devcfg_error(void)
{
    return s_error[0] ? s_error : "no error";
}

const char *devcfg_document(size_t *out_len)
{
    if (s_state != DEVCFG_DONE || s_doc == NULL) {
        return NULL;
    }
    if (out_len) {
        *out_len = s_len;
    }
    return s_doc;
}
