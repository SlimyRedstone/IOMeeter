/*
 * Persistence for the fader strip, read and written as config.json in the
 * working directory. A desktop launcher sets that with Path=, so an installed
 * copy keeps its configuration in the data directory rather than in $HOME:
 *
 *   {"debug":true,"sliders":[
 *     {"id":0,"value":2024,"name":"Main Audio",
 *      "apps":[{"path":"C:/.../Discord.exe","name":"Discord.exe"}]},
 *     ...
 *   ]}
 *
 * "debug" controls whether the interface shows its traffic console. The file
 * is always written indented, since it is meant to be opened and edited by
 * hand. The macro pad adds "keys", "profile" and "profiles" to the same
 * object; keys.h owns that half of the schema and this only hands it the
 * parsed root.
 *
 * The shape is fixed and small, so this scans rather than pulling in a JSON
 * library: the host client would otherwise need cJSON built for every vcpkg
 * triplet and every distro it runs on.
 *
 * A missing, unreadable or malformed file is not an error. Whatever cannot be
 * read falls back to the built-in defaults, so the interface always starts.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#include "keys.h"

#define CONFIG_NAME_MAX 32
#define CONFIG_PATH_MAX 260
#define CONFIG_APPS_MAX 16

/* Top of a fader's travel, matching APP_FADER_MAX. Values read from the file
   are held to it, so a hand-edited number cannot put a knob off the window. */
#define CONFIG_SLIDER_MAX 4095

/* One executable whose volume a slider controls. */
typedef struct {
    char path[CONFIG_PATH_MAX];   /*!< as chosen in the file dialog */
    char name[CONFIG_NAME_MAX];   /*!< basename, what the mixer matches on */
} config_app_t;

typedef struct {
    int  id;
    int  value;
    char name[CONFIG_NAME_MAX];

    config_app_t apps[CONFIG_APPS_MAX];
    int          app_count;
} config_slider_t;

/**
 * Append an app to a slider, ignoring duplicates by path.
 *
 * @return false if the slider is full or the path is already present.
 */
bool config_add_app(config_slider_t *slider, const char *path);

/** Remove the app at @p index, closing the gap. */
void config_remove_app(config_slider_t *slider, int index);

/** Basename of @p path, without directories. */
const char *config_basename(const char *path);

#define CONFIG_DEBUG_DEFAULT true

/**
 * Fill @p out with the built-in defaults.
 *
 * @param debug Receives the default debug flag. May be NULL.
 */
void config_defaults(config_slider_t *out, int count, bool *debug);

/**
 * Read @p path into @p out.
 *
 * Entries are placed by their "id" field. Anything absent from the file keeps
 * its default, so a partially written file still yields a usable strip.
 *
 * @param debug Receives the "debug" flag, or its default when absent. May be NULL.
 * @param keys  Receives the macro pad, or its defaults when absent. May be NULL.
 * @return true if the file was read and at least one slider was recognised.
 */
bool config_load(const char *path, config_slider_t *out, int count, bool *debug,
                 keys_t *keys);

/**
 * Serialise the configuration to a JSON string, in the same shape as the file.
 *
 * The caller owns the result and releases it with free(). Used to put the
 * configuration on the controller, which stores it as bytes so that it follows
 * the hardware rather than the machine it was set up on.
 *
 * @param debug  Written as "debug".
 * @param keys   Written as "keys", "profile" and "profiles". May be NULL.
 * @param pretty Indent the output. Set for the file, which is read by people,
 *               and clear for the copy sent to the controller, which is not:
 *               indentation adds about a third to a document that already has
 *               to be split into slices to cross the wire.
 * @return NULL if the document could not be built.
 */
char *config_to_text(const config_slider_t *in, int count, bool debug,
                     const keys_t *keys, bool pretty);

/**
 * Read a configuration from @p text, which need not be NUL-terminated JSON
 * from a file -- it is what came back off the wire.
 *
 * Behaves as config_load() otherwise: anything absent keeps its default.
 *
 * @return true if the text parsed and at least one slider was recognised.
 */
bool config_from_text(const char *text, config_slider_t *out, int count,
                      bool *debug, keys_t *keys);

/**
 * Write @p in to @p path, creating it if necessary.
 *
 * @param keys Written as "keys", "profile" and "profiles". May be NULL, which
 *             leaves those out entirely rather than writing them empty.
 */
bool config_save(const char *path, const config_slider_t *in, int count,
                 bool debug, const keys_t *keys);

#endif /* CONFIG_H */
