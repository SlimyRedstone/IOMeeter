/*
 * The macro pad: thirty keys, each with a name, a colour and a macro, held
 * separately for every profile.
 *
 * Stored in config.json alongside the faders:
 *
 *   {"debug":false,"sliders":[],"keys":[],"profile":0,
 *    "profiles":[{"name":"Default","apps":[]}]}
 *
 * with one "keys" entry per key that has anything set:
 *
 *   {"id":0,"macro":[{"profile":0,"name":"Build","color":"ff00ff",
 *                     "timings":[20,20,20],
 *                     "cmds":["CTRL_LEFT","SHIFT_LEFT","M"]}]}
 *
 * "macro" is an array because one key carries a separate binding per profile,
 * so switching profile re-binds the whole pad at once. "profile" is the one
 * currently selected and "profiles" holds their names, which the schema in the
 * brief did not cover but a named selector needs.
 *
 * A profile also carries the applications that select it: whenever one of them
 * is the window in front, the pad switches to that profile on its own. A bare
 * string is still accepted in "profiles" and read as a name with no
 * applications, so a file written before this loads unchanged.
 *
 * A macro is one of three things, never more than one. With "cmds" it is a
 * chord: each entry is pressed in order, waiting its timing first, and they
 * are released in the reverse order afterwards, under the names keysend.h
 * accepts. With "text" it is a phrase, typed out as it stands. With "media"
 * it is the name of an application whose playback the key toggles, which is
 * not a keystroke at all -- a media key is global and lands on whichever
 * program played last, so naming one is the only way to reach it. An entry
 * carrying none of the three is rejected and reported.
 *
 * A chord may also carry "target", the application its keys are delivered to
 * instead of to the desktop. That is how a program which publishes no media
 * session is reached: VLC toggles on its own space bar, so a chord of SPACE
 * aimed at it pauses that window and nothing else.
 *
 * Not thread safe; call from one thread.
 */

#ifndef KEYS_H
#define KEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Declared rather than included: this header is pulled in by the interface,
   which has no business seeing the JSON library. */
struct cJSON;

#define KEYS_COLUMNS            6
#define KEYS_ROWS               5
#define KEYS_COUNT              (KEYS_COLUMNS * KEYS_ROWS)

/* Twenty-four characters, as specified, plus the terminator. */
#define KEYS_NAME_MAX           25

#define KEYS_PROFILES_MAX       8
#define KEYS_PROFILE_NAME_MAX   24

/* Applications that select a profile, and the room their paths need. The
   dimensions match the slider lists in config.h, which are picked the same
   way and matched against the same executable names. */
#define KEYS_APPS_MAX           8
#define KEYS_APP_PATH_MAX       260
#define KEYS_APP_NAME_MAX       64

/* Steps in one macro, and the longest key name any of them may hold. */
#define KEYS_MACRO_MAX          16
#define KEYS_CMD_MAX            24

/* A text macro types this instead of pressing keys. */
#define KEYS_TEXT_MAX           128

/*
 * Milliseconds a single step may wait. A value outside the range is brought
 * into it rather than rejected: a negative is taken as its distance from zero,
 * so -1 means 1, and anything past the top is pinned there.
 */
#define KEYS_TIMING_MIN         0
#define KEYS_TIMING_MAX         10000
#define KEYS_TIMING_DEFAULT     20

/* Unbound keys, and the colour a new binding starts at. */
#define KEYS_COLOR_DEFAULT      0x2a2a2au

typedef struct {
    char cmds[KEYS_MACRO_MAX][KEYS_CMD_MAX];
    int  timings[KEYS_MACRO_MAX];   /*!< milliseconds to wait before each */
    int  count;

    /* Used instead of the chord above, and only when count is zero. */
    char text[KEYS_TEXT_MAX];

    /*
     * Used instead of either of those: the application whose playback the key
     * toggles. Held as the executable's name rather than its path, because
     * that is what it can be matched by once it is running -- the same
     * identity the faders address an application with.
     */
    char media[KEYS_APP_NAME_MAX];

    /*
     * Where the chord above is delivered. Empty means the desktop, which is
     * every macro that existed before this: the keys go out as if typed, and
     * whatever is in front receives them. Named, they are posted to that
     * program's window instead, so a key reaches it while something else has
     * the focus.
     */
    char target[KEYS_APP_NAME_MAX];
} keys_macro_t;

/* What one key does under one profile. */
typedef struct {
    char         name[KEYS_NAME_MAX];
    uint32_t     color;             /*!< 0xRRGGBB */
    keys_macro_t macro;
} keys_binding_t;

/* One application whose being in front selects a profile. */
typedef struct {
    char path[KEYS_APP_PATH_MAX];   /*!< as chosen in the picker */
    char name[KEYS_APP_NAME_MAX];   /*!< basename, what the match is on */
} keys_app_t;

typedef struct {
    char       name[KEYS_PROFILE_NAME_MAX];
    keys_app_t apps[KEYS_APPS_MAX];
    int        app_count;

    /* Fallen back to when whatever is in front belongs to no profile. At
       most one profile carries this; keys_set_default() maintains that. */
    bool       is_default;
} keys_profile_t;

typedef struct {
    keys_binding_t binding[KEYS_COUNT][KEYS_PROFILES_MAX];

    keys_profile_t profiles[KEYS_PROFILES_MAX];
    int            profile_count;
    int            profile;         /*!< the one currently selected */
} keys_t;

/** Empty pad with a single profile named "Default". */
void keys_defaults(keys_t *k);

/**
 * @param k       Pad to read.
 * @param id      Key index, 0..KEYS_COUNT-1.
 * @param profile Profile index; out of range is treated as the active one.
 * @return The binding, or NULL if @p id is out of range.
 */
keys_binding_t *keys_binding(keys_t *k, int id, int profile);

/** As keys_binding(), for a pad that must not be modified. */
const keys_binding_t *keys_binding_const(const keys_t *k, int id, int profile);

/** True when nothing has been set on @p b, so it need not be written out. */
bool keys_binding_empty(const keys_binding_t *b);

/**
 * Append a profile, copying no bindings.
 *
 * @param name Shown in the selector; a NULL or empty name is numbered.
 * @return Index of the new profile, or -1 when there is no room.
 */
int keys_add_profile(keys_t *k, const char *name);

/**
 * @param profile Index to rename; ignored when out of range.
 * @param name    New name; ignored when empty.
 */
void keys_rename_profile(keys_t *k, int profile, const char *name);

/**
 * Remove a profile, along with every binding held under it.
 *
 * The profiles above it shift down, and so do their bindings, so the pad a
 * profile carries stays with it. The active profile follows the move.
 *
 * @param profile Index to remove.
 * @return false when the index is out of range, or when it is the only
 *         profile left: the pad always has one.
 */
bool keys_remove_profile(keys_t *k, int profile);

/**
 * Add an application that selects @p profile, ignoring duplicates by name.
 *
 * @param path Full path, or a bare executable name from the running list.
 *             The stored name drops an executable suffix, so the list reads
 *             "WindowsTerminal" rather than "WindowsTerminal.exe".
 * @return false when the list is full, the profile is out of range, or the
 *         application is already there.
 */
bool keys_add_app(keys_t *k, int profile, const char *path);

/** Remove the application at @p index from @p profile, closing the gap. */
void keys_remove_app(keys_t *k, int profile, int index);

/**
 * Make @p profile the one to fall back on, clearing the flag everywhere else.
 *
 * @param profile Index to mark, or -1 to leave the pad with no fallback.
 */
void keys_set_default(keys_t *k, int profile);

/** Index of the fallback profile, or -1 when none is marked. */
int keys_default_profile(const keys_t *k);

/**
 * Give a macro a phrase to type, in place of any chord it held.
 *
 * @param text Typed as it stands; an empty string clears the macro.
 */
void keys_macro_set_text(keys_macro_t *m, const char *text);

/**
 * Give a macro an application whose playback it toggles, in place of whatever
 * it held.
 *
 * @param app A name or a full path; only the file name at the end of it is
 *            kept, since that is what a running program can be matched by.
 *            An empty string clears the macro.
 */
void keys_macro_set_media(keys_macro_t *m, const char *app);

/**
 * Aim a chord at one application, or at the desktop again.
 *
 * @param app A name or a full path, of which only the file name is kept, or
 *            an empty string to send the chord the ordinary way.
 */
void keys_macro_set_target(keys_macro_t *m, const char *app);

/** Bring @p ms into the accepted range: negatives are mirrored, the rest
    pinned to the ends. */
int keys_clamp_timing(int ms);

/**
 * Which profile an application selects.
 *
 * Matching ignores case and any extension, so one configuration works where
 * the same program is "Discord.exe" and "discord".
 *
 * @param process Executable basename of whatever is in front.
 * @return The profile index, or -1 when no profile claims it.
 */
int keys_profile_for(const keys_t *k, const char *process);

/**
 * Append a step to a macro.
 *
 * @param cmd    Command name, as accepted by keysend_known().
 * @param timing Milliseconds to wait before the step, clamped.
 * @return false when the macro is full or @p cmd is empty.
 */
bool keys_macro_add(keys_macro_t *m, const char *cmd, int timing);

/** Remove the step at @p index, closing the gap. */
void keys_macro_remove(keys_macro_t *m, int index);

/**
 * Parse "ff00ff", with or without a leading '#'.
 *
 * @param rgb Receives the colour. Untouched when the text is not six hex
 *            digits, so a half-typed field keeps the previous value.
 * @return true when @p text was a complete colour.
 */
bool keys_parse_hex(const char *text, uint32_t *rgb);

/** Write @p rgb as six lower-case hex digits. */
void keys_format_hex(uint32_t rgb, char *out, size_t size);

/**
 * Read "keys", "profile" and "profiles" out of a parsed config.json.
 *
 * Anything missing or malformed leaves the defaults in place, so a file
 * written before the pad existed still loads.
 */
void keys_from_json(const struct cJSON *root, keys_t *out);

/**
 * Add "keys", "profile" and "profiles" to an object being built.
 *
 * @return false if any allocation failed.
 */
bool keys_to_json(struct cJSON *root, const keys_t *in);

#endif /* KEYS_H */
