#include "keys.h"

#include "appname.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

void keys_defaults(keys_t *k)
{
    memset(k, 0, sizeof(*k));

    for (int i = 0; i < KEYS_COUNT; i++) {
        for (int p = 0; p < KEYS_PROFILES_MAX; p++) {
            k->binding[i][p].color = KEYS_COLOR_DEFAULT;
        }
    }

    snprintf(k->profiles[0].name, KEYS_PROFILE_NAME_MAX, "Default");
    k->profile_count = 1;
    k->profile = 0;
}

keys_binding_t *keys_binding(keys_t *k, int id, int profile)
{
    if (id < 0 || id >= KEYS_COUNT) {
        return NULL;
    }
    if (profile < 0 || profile >= KEYS_PROFILES_MAX) {
        profile = k->profile;
    }
    return &k->binding[id][profile];
}

const keys_binding_t *keys_binding_const(const keys_t *k, int id, int profile)
{
    return keys_binding((keys_t *)k, id, profile);
}

bool keys_binding_empty(const keys_binding_t *b)
{
    return b->name[0] == 0 && b->macro.count == 0 && b->macro.text[0] == 0 &&
           b->macro.media[0] == 0 && b->color == KEYS_COLOR_DEFAULT;
}

int keys_add_profile(keys_t *k, const char *name)
{
    if (k->profile_count >= KEYS_PROFILES_MAX) {
        return -1;
    }

    int index = k->profile_count;

    if (name != NULL && name[0] != 0) {
        snprintf(k->profiles[index].name, KEYS_PROFILE_NAME_MAX, "%s", name);
    } else {
        snprintf(k->profiles[index].name, KEYS_PROFILE_NAME_MAX, "Profile %d",
                 index + 1);
    }

    k->profiles[index].app_count = 0;

    /* The slot may already carry bindings, read from a file that bound more
       profiles than it named, so only an untouched one is reset. */
    for (int i = 0; i < KEYS_COUNT; i++) {
        if (keys_binding_empty(&k->binding[i][index])) {
            k->binding[i][index].color = KEYS_COLOR_DEFAULT;
        }
    }

    k->profile_count++;
    return index;
}

void keys_rename_profile(keys_t *k, int profile, const char *name)
{
    if (profile < 0 || profile >= k->profile_count) {
        return;
    }
    if (name == NULL || name[0] == 0) {
        return;
    }
    snprintf(k->profiles[profile].name, KEYS_PROFILE_NAME_MAX, "%s", name);
}

/* Case-insensitive, and blind to a trailing extension, so one list matches
   "Discord.exe" on Windows and "discord" on Linux. */

bool keys_remove_profile(keys_t *k, int profile)
{
    /* The pad is meaningless without one, and every key would have nothing
       to be bound under. */
    if (profile < 0 || profile >= k->profile_count || k->profile_count <= 1) {
        return false;
    }

    for (int p = profile; p + 1 < k->profile_count; p++) {
        k->profiles[p] = k->profiles[p + 1];

        for (int i = 0; i < KEYS_COUNT; i++) {
            k->binding[i][p] = k->binding[i][p + 1];
        }
    }

    int last = k->profile_count - 1;
    memset(&k->profiles[last], 0, sizeof(k->profiles[last]));

    for (int i = 0; i < KEYS_COUNT; i++) {
        memset(&k->binding[i][last], 0, sizeof(k->binding[i][last]));
        k->binding[i][last].color = KEYS_COLOR_DEFAULT;
    }

    k->profile_count--;

    /* Follow the shift, and fall back onto the last one when the profile
       that was showing is the one that went. */
    if (k->profile > profile) {
        k->profile--;
    }
    if (k->profile >= k->profile_count) {
        k->profile = k->profile_count - 1;
    }

    return true;
}

bool keys_add_app(keys_t *k, int profile, const char *path)
{
    if (profile < 0 || profile >= k->profile_count || path == NULL ||
        path[0] == 0) {
        return false;
    }

    keys_profile_t *p = &k->profiles[profile];
    if (p->app_count >= KEYS_APPS_MAX) {
        return false;
    }

    const char *name = appname_base(path);

    for (int i = 0; i < p->app_count; i++) {
        if (appname_same(p->apps[i].name, name)) {
            return false;
        }
    }

    /* Matching is blind to the suffix either way, so dropping it costs
       nothing and is what the list has to show. */
    int shown = (int)appname_length(name);

    snprintf(p->apps[p->app_count].path, KEYS_APP_PATH_MAX, "%s", path);
    snprintf(p->apps[p->app_count].name, KEYS_APP_NAME_MAX, "%.*s", shown, name);
    p->app_count++;
    return true;
}

void keys_remove_app(keys_t *k, int profile, int index)
{
    if (profile < 0 || profile >= k->profile_count) {
        return;
    }

    keys_profile_t *p = &k->profiles[profile];
    if (index < 0 || index >= p->app_count) {
        return;
    }

    for (int i = index; i + 1 < p->app_count; i++) {
        p->apps[i] = p->apps[i + 1];
    }

    p->app_count--;
    memset(&p->apps[p->app_count], 0, sizeof(p->apps[p->app_count]));
}

void keys_set_default(keys_t *k, int profile)
{
    for (int p = 0; p < KEYS_PROFILES_MAX; p++) {
        k->profiles[p].is_default = (p == profile);
    }
}

int keys_default_profile(const keys_t *k)
{
    for (int p = 0; p < k->profile_count; p++) {
        if (k->profiles[p].is_default) {
            return p;
        }
    }
    return -1;
}

int keys_profile_for(const keys_t *k, const char *process)
{
    if (process == NULL || process[0] == 0) {
        return -1;
    }

    const char *name = appname_base(process);

    for (int p = 0; p < k->profile_count; p++) {
        for (int i = 0; i < k->profiles[p].app_count; i++) {
            if (appname_same(k->profiles[p].apps[i].name, name)) {
                return p;
            }
        }
    }
    return -1;
}

int keys_clamp_timing(int ms)
{
    /* A negative is read as its distance from zero rather than thrown away,
       so a stray minus sign costs the sign and not the value. */
    if (ms < 0) {
        ms = -ms;
    }
    if (ms > KEYS_TIMING_MAX) {
        ms = KEYS_TIMING_MAX;
    }
    if (ms < KEYS_TIMING_MIN) {
        ms = KEYS_TIMING_MIN;
    }
    return ms;
}

void keys_macro_set_text(keys_macro_t *m, const char *text)
{
    /* The three are exclusive, so taking a phrase drops whatever was there:
       that is the shape the file has to be written in. */
    m->count = 0;
    memset(m->cmds, 0, sizeof(m->cmds));
    memset(m->timings, 0, sizeof(m->timings));
    m->media[0] = 0;
    m->target[0] = 0;

    snprintf(m->text, KEYS_TEXT_MAX, "%s", text ? text : "");
}

void keys_macro_set_target(keys_macro_t *m, const char *app)
{
    /* Not a kind of its own: it says where the chord goes, so it leaves the
       steps alone. */
    snprintf(m->target, KEYS_APP_NAME_MAX, "%s", app ? appname_base(app) : "");
}

void keys_macro_set_media(keys_macro_t *m, const char *app)
{
    m->count = 0;
    memset(m->cmds, 0, sizeof(m->cmds));
    memset(m->timings, 0, sizeof(m->timings));
    m->text[0] = 0;
    m->target[0] = 0;

    /* A path is accepted because that is what a file dialog hands back, but
       only its last part is kept: a program is matched while it runs, by the
       name of the file it was started from. */
    snprintf(m->media, KEYS_APP_NAME_MAX, "%s", app ? appname_base(app) : "");
}

bool keys_macro_add(keys_macro_t *m, const char *cmd, int timing)
{
    if (m->count >= KEYS_MACRO_MAX || cmd == NULL || cmd[0] == 0) {
        return false;
    }

    /* Likewise the other way round. */
    m->text[0] = 0;
    m->media[0] = 0;

    timing = keys_clamp_timing(timing);

    snprintf(m->cmds[m->count], KEYS_CMD_MAX, "%s", cmd);
    m->timings[m->count] = timing;
    m->count++;
    return true;
}

void keys_macro_remove(keys_macro_t *m, int index)
{
    if (index < 0 || index >= m->count) {
        return;
    }

    for (int i = index; i + 1 < m->count; i++) {
        memcpy(m->cmds[i], m->cmds[i + 1], KEYS_CMD_MAX);
        m->timings[i] = m->timings[i + 1];
    }

    m->count--;
    m->cmds[m->count][0] = 0;
    m->timings[m->count] = 0;
}

static int hex_digit(char c)
{
    if (c >= 48 && c <= 57) {           /* 0-9 */
        return c - 48;
    }
    if (c >= 97 && c <= 102) {          /* a-f */
        return c - 97 + 10;
    }
    if (c >= 65 && c <= 70) {           /* A-F */
        return c - 65 + 10;
    }
    return -1;
}

bool keys_parse_hex(const char *text, uint32_t *rgb)
{
    if (text == NULL) {
        return false;
    }
    if (text[0] == 35) {                /* a leading # is accepted */
        text++;
    }

    uint32_t value = 0;

    for (int i = 0; i < 6; i++) {
        int digit = hex_digit(text[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (uint32_t)digit;
    }

    /* Anything after the sixth digit means the field is still being typed. */
    if (text[6] != 0) {
        return false;
    }

    *rgb = value;
    return true;
}

void keys_format_hex(uint32_t rgb, char *out, size_t size)
{
    snprintf(out, size, "%06x", (unsigned)(rgb & 0xFFFFFFu));
}

/* ----------------------------------------------------------------- json --- */

/*
 * "cmds" wins where both are present, "text" is the fallback, and an entry
 * carrying neither is not a macro at all.
 *
 * @return false when the entry describes nothing playable.
 */
static bool load_macro(const cJSON *entry, keys_macro_t *out)
{
    const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(entry, "cmds");
    const cJSON *timings = cJSON_GetObjectItemCaseSensitive(entry, "timings");

    if (!cJSON_IsArray(cmds)) {
        const cJSON *media = cJSON_GetObjectItemCaseSensitive(entry, "media");

        if (cJSON_IsString(media) && media->valuestring &&
            media->valuestring[0] != 0) {
            keys_macro_set_media(out, media->valuestring);
            return true;
        }

        const cJSON *text = cJSON_GetObjectItemCaseSensitive(entry, "text");

        if (cJSON_IsString(text) && text->valuestring) {
            keys_macro_set_text(out, text->valuestring);

            /* A phrase waits before it is typed, the same as a chord does. */
            const cJSON *at = cJSON_IsArray(timings)
                            ? cJSON_GetArrayItem((cJSON *)timings, 0)
                            : NULL;
            if (cJSON_IsNumber(at)) {
                out->timings[0] = keys_clamp_timing(at->valueint);
            } else {
                out->timings[0] = KEYS_TIMING_DEFAULT;
            }
            return true;
        }

        return false;
    }

    out->count = 0;
    out->text[0] = 0;
    out->media[0] = 0;
    out->target[0] = 0;

    /* Where the chord goes, if anywhere in particular. */
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(entry, "target");
    if (cJSON_IsString(target) && target->valuestring) {
        keys_macro_set_target(out, target->valuestring);
    }

    int position = 0;
    const cJSON *cmd = NULL;

    cJSON_ArrayForEach(cmd, cmds) {
        if (!cJSON_IsString(cmd) || cmd->valuestring == NULL) {
            position++;
            continue;
        }

        /* Timings are positional, and a short list is not an error: the steps
           it does not reach fall back to the default gap. */
        int timing = KEYS_TIMING_DEFAULT;
        const cJSON *at = cJSON_IsArray(timings)
                        ? cJSON_GetArrayItem((cJSON *)timings, position)
                        : NULL;

        if (cJSON_IsNumber(at)) {
            timing = at->valueint;
        }

        keys_macro_add(out, cmd->valuestring, timing);
        position++;
    }

    return true;
}

static void load_profile_apps(const cJSON *entry, keys_profile_t *out)
{
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(entry, "apps");
    if (!cJSON_IsArray(apps)) {
        return;
    }

    out->app_count = 0;

    const cJSON *app = NULL;
    cJSON_ArrayForEach(app, apps) {
        if (out->app_count >= KEYS_APPS_MAX) {
            break;
        }

        keys_app_t *slot = &out->apps[out->app_count];
        slot->path[0] = 0;
        slot->name[0] = 0;

        /* A bare string is accepted as the executable name on its own, which
           is all the match needs and all the running-application picker has
           when a process will not give up its path. */
        if (cJSON_IsString(app) && app->valuestring) {
            snprintf(slot->path, KEYS_APP_PATH_MAX, "%s", app->valuestring);
        } else if (cJSON_IsObject(app)) {
            const cJSON *path = cJSON_GetObjectItemCaseSensitive(app, "path");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(app, "name");

            if (cJSON_IsString(path) && path->valuestring) {
                snprintf(slot->path, KEYS_APP_PATH_MAX, "%s", path->valuestring);
            }
            if (cJSON_IsString(name) && name->valuestring) {
                snprintf(slot->name, KEYS_APP_NAME_MAX, "%s", name->valuestring);
            }
        }

        if (slot->name[0] == 0 && slot->path[0] != 0) {
            const char *base = appname_base(slot->path);
            snprintf(slot->name, KEYS_APP_NAME_MAX, "%.*s",
                     (int)appname_length(base), base);
        }
        if (slot->name[0] != 0) {
            out->app_count++;
        }
    }
}

/*
 * "profiles" holds an object per profile. A bare string is still read, as a
 * name with no applications, because that is what the first version wrote.
 */
static void load_profiles(const cJSON *root, keys_t *out)
{
    const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsArray(profiles)) {
        return;
    }

    int count = 0;
    const cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, profiles) {
        if (count >= KEYS_PROFILES_MAX) {
            break;
        }

        keys_profile_t *slot = &out->profiles[count];

        if (cJSON_IsString(entry) && entry->valuestring) {
            snprintf(slot->name, KEYS_PROFILE_NAME_MAX, "%s", entry->valuestring);
        } else if (cJSON_IsObject(entry)) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");

            if (cJSON_IsString(name) && name->valuestring) {
                snprintf(slot->name, KEYS_PROFILE_NAME_MAX, "%s",
                         name->valuestring);
            }

            const cJSON *fallback =
                cJSON_GetObjectItemCaseSensitive(entry, "default");
            slot->is_default = cJSON_IsTrue(fallback) ? true : false;

            load_profile_apps(entry, slot);
        }

        if (slot->name[0] == 0) {
            snprintf(slot->name, KEYS_PROFILE_NAME_MAX, "Profile %d", count + 1);
        }
        count++;
    }

    if (count > 0) {
        out->profile_count = count;
    }

    /* Only one profile may be the fallback, however the file was edited. */
    int fallback = keys_default_profile(out);
    keys_set_default(out, fallback);
}

/* One "macro" array: the bindings of a single key, one entry per profile. */
static void load_key(const cJSON *key, int id, keys_t *out)  /* NOLINT */
{
    const cJSON *macros = cJSON_GetObjectItemCaseSensitive(key, "macro");
    if (!cJSON_IsArray(macros)) {
        return;
    }

    int slot = 0;
    const cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, macros) {
        if (!cJSON_IsObject(entry)) {
            slot++;
            continue;
        }

        const cJSON *p = cJSON_GetObjectItemCaseSensitive(entry, "profile");
        int profile = cJSON_IsNumber(p) ? p->valueint : slot;
        slot++;

        if (profile < 0 || profile >= KEYS_PROFILES_MAX) {
            continue;
        }

        keys_binding_t *binding = &out->binding[id][profile];

        const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            snprintf(binding->name, KEYS_NAME_MAX, "%s", name->valuestring);
        }

        const cJSON *color = cJSON_GetObjectItemCaseSensitive(entry, "color");
        if (cJSON_IsString(color) && color->valuestring) {
            keys_parse_hex(color->valuestring, &binding->color);
        }

        if (!load_macro(entry, &binding->macro)) {
            /* Neither key list nor phrase: nothing to play, and silently
               dropping it would leave the key looking bound. */
            fprintf(stderr,
                    "config: key %d profile %d has no \"cmds\" and no "
                    "\"text\"; ignored\n", id, profile);
        }

        /* A binding for a profile the "profiles" list never named still needs
           a selector entry, or there would be no way to reach it. */
        if (profile >= out->profile_count) {
            out->profile_count = profile + 1;
            if (out->profiles[profile].name[0] == 0) {
                snprintf(out->profiles[profile].name, KEYS_PROFILE_NAME_MAX,
                         "Profile %d", profile + 1);
            }
        }
    }
}

void keys_from_json(const struct cJSON *root_in, keys_t *out)
{
    const cJSON *root = (const cJSON *)root_in;

    keys_defaults(out);

    if (root == NULL) {
        return;
    }

    load_profiles(root, out);

    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys");
    if (cJSON_IsArray(keys)) {
        int position = 0;
        const cJSON *key = NULL;

        cJSON_ArrayForEach(key, keys) {
            if (!cJSON_IsObject(key)) {
                position++;
                continue;
            }

            /* Entries land by id, as the sliders do, so the file may list them
               in any order and leave out the keys nothing is bound to. */
            const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(key, "id");
            int id = cJSON_IsNumber(id_item) ? id_item->valueint : position;
            position++;

            if (id >= 0 && id < KEYS_COUNT) {
                load_key(key, id, out);
            }
        }
    }

    const cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (cJSON_IsNumber(active) && active->valueint >= 0 &&
        active->valueint < out->profile_count) {
        out->profile = active->valueint;
    }
}

static bool save_binding(cJSON *macros, const keys_binding_t *binding, int profile)
{
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL || !cJSON_AddItemToArray(macros, entry)) {
        cJSON_Delete(entry);
        return false;
    }

    char hex[8];
    keys_format_hex(binding->color, hex, sizeof(hex));

    if (!cJSON_AddNumberToObject(entry, "profile", profile) ||
        !cJSON_AddStringToObject(entry, "name", binding->name) ||
        !cJSON_AddStringToObject(entry, "color", hex)) {
        return false;
    }

    cJSON *timings = cJSON_AddArrayToObject(entry, "timings");
    if (timings == NULL) {
        return false;
    }

    /* The three are alternatives, so only one of the keys is ever written and
       the reader tells them apart by which is there. */
    if (binding->macro.count == 0 && binding->macro.media[0] != 0) {
        return cJSON_AddStringToObject(entry, "media",
                                       binding->macro.media) != NULL;
    }

    if (binding->macro.count == 0 && binding->macro.text[0] != 0) {
        cJSON *wait = cJSON_CreateNumber(binding->macro.timings[0]);

        if (wait == NULL) {
            return false;
        }
        cJSON_AddItemToArray(timings, wait);

        return cJSON_AddStringToObject(entry, "text",
                                       binding->macro.text) != NULL;
    }

    cJSON *cmds = cJSON_AddArrayToObject(entry, "cmds");
    if (cmds == NULL) {
        return false;
    }

    /* Only written when there is one, so a macro that goes to the desktop
       reads exactly as it always did. */
    if (binding->macro.target[0] != 0 &&
        cJSON_AddStringToObject(entry, "target", binding->macro.target) == NULL) {
        return false;
    }

    for (int i = 0; i < binding->macro.count; i++) {
        cJSON *timing = cJSON_CreateNumber(binding->macro.timings[i]);
        cJSON *cmd = cJSON_CreateString(binding->macro.cmds[i]);

        if (timing == NULL || cmd == NULL) {
            cJSON_Delete(timing);
            cJSON_Delete(cmd);
            return false;
        }

        cJSON_AddItemToArray(timings, timing);
        cJSON_AddItemToArray(cmds, cmd);
    }

    return true;
}

bool keys_to_json(struct cJSON *root_in, const keys_t *in)
{
    cJSON *root = (cJSON *)root_in;

    cJSON *profiles = cJSON_AddArrayToObject(root, "profiles");
    if (profiles == NULL) {
        return false;
    }

    for (int p = 0; p < in->profile_count; p++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL || !cJSON_AddItemToArray(profiles, entry)) {
            cJSON_Delete(entry);
            return false;
        }

        if (cJSON_AddStringToObject(entry, "name", in->profiles[p].name) == NULL) {
            return false;
        }

        if (in->profiles[p].is_default &&
            cJSON_AddBoolToObject(entry, "default", 1) == NULL) {
            return false;
        }

        cJSON *apps = cJSON_AddArrayToObject(entry, "apps");
        if (apps == NULL) {
            return false;
        }

        for (int i = 0; i < in->profiles[p].app_count; i++) {
            cJSON *app = cJSON_CreateObject();
            if (app == NULL || !cJSON_AddItemToArray(apps, app)) {
                cJSON_Delete(app);
                return false;
            }

            if (!cJSON_AddStringToObject(app, "path",
                                         in->profiles[p].apps[i].path) ||
                !cJSON_AddStringToObject(app, "name",
                                         in->profiles[p].apps[i].name)) {
                return false;
            }
        }
    }

    if (cJSON_AddNumberToObject(root, "profile", in->profile) == NULL) {
        return false;
    }

    cJSON *keys = cJSON_AddArrayToObject(root, "keys");
    if (keys == NULL) {
        return false;
    }

    for (int i = 0; i < KEYS_COUNT; i++) {
        /* Only bound keys are written, so a fresh pad leaves "keys" empty
           rather than listing thirty blank entries. */
        bool any = false;
        for (int p = 0; p < in->profile_count && !any; p++) {
            any = !keys_binding_empty(&in->binding[i][p]);
        }
        if (!any) {
            continue;
        }

        cJSON *key = cJSON_CreateObject();
        if (key == NULL || !cJSON_AddItemToArray(keys, key)) {
            cJSON_Delete(key);
            return false;
        }

        if (cJSON_AddNumberToObject(key, "id", i) == NULL) {
            return false;
        }

        cJSON *macros = cJSON_AddArrayToObject(key, "macro");
        if (macros == NULL) {
            return false;
        }

        for (int p = 0; p < in->profile_count; p++) {
            if (keys_binding_empty(&in->binding[i][p])) {
                continue;
            }
            if (!save_binding(macros, &in->binding[i][p], p)) {
                return false;
            }
        }
    }

    return true;
}
