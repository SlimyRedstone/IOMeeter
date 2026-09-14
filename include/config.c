#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/*
 * Ceiling on the configuration file, as a guard against reading something that
 * is not one at all rather than as an expected size. The real file grows with
 * the macro pad -- thirty keys across eight profiles, each with a name, a
 * colour and a macro -- and passes 8 KB with a fairly ordinary setup, half as
 * much again for the indentation.
 */
#define FILE_MAX (4 * 1024 * 1024)

static const struct {
    int value;
    const char *name;
} DEFAULTS[] = {
    { 2024, "Main Audio" },
    { 4024, "Game Audio" },
    { 1024, "Music Audio" },
    { 3024, "Discord Audio" },
};

const char *config_basename(const char *path)
{
    const char *last = path;

    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == 0x5C) {   /* forward slash or backslash */
            last = p + 1;
        }
    }
    return last;
}

bool config_add_app(config_slider_t *slider, const char *path)
{
    if (slider == NULL || path == NULL || path[0] == 0) {
        return false;
    }
    if (slider->app_count >= CONFIG_APPS_MAX) {
        return false;
    }

    for (int i = 0; i < slider->app_count; i++) {
        if (strcmp(slider->apps[i].path, path) == 0) {
            return false;
        }
    }

    config_app_t *app = &slider->apps[slider->app_count];
    snprintf(app->path, CONFIG_PATH_MAX, "%s", path);
    snprintf(app->name, CONFIG_NAME_MAX, "%s", config_basename(path));
    slider->app_count++;
    return true;
}

void config_remove_app(config_slider_t *slider, int index)
{
    if (slider == NULL || index < 0 || index >= slider->app_count) {
        return;
    }

    for (int i = index; i < slider->app_count - 1; i++) {
        slider->apps[i] = slider->apps[i + 1];
    }
    slider->app_count--;
}

void config_defaults(config_slider_t *out, int count, bool *debug)
{
    int known = (int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0]));

    if (debug) {
        *debug = CONFIG_DEBUG_DEFAULT;
    }

    for (int i = 0; i < count; i++) {
        out[i].id = i;
        out[i].app_count = 0;
        if (i < known) {
            out[i].value = DEFAULTS[i].value;
            snprintf(out[i].name, CONFIG_NAME_MAX, "%s", DEFAULTS[i].name);
        } else {
            out[i].value = 0;
            snprintf(out[i].name, CONFIG_NAME_MAX, "Slider %d", i + 1);
        }
    }
}

/*
 * Read the whole file, whatever its size. Returns NULL on any failure; the
 * caller frees.
 *
 * The allocation follows the file rather than a fixed window: reading only the
 * first N bytes of a longer file hands cJSON a truncated document, which fails
 * to parse, which is indistinguishable here from having no configuration at
 * all -- so the whole thing would be replaced by defaults on the next save.
 */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0 || size > FILE_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char *text = malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(f);
        return NULL;
    }

    /* Short of the file size on a text-mode translation or a concurrent
       truncation, which is not an error: the terminator goes where the
       reading stopped. */
    size_t len = fread(text, 1, (size_t)size, f);
    fclose(f);

    text[len] = '\0';
    return text;
}

/* Copy a cJSON string member into a fixed buffer, leaving it alone if absent. */
static void copy_string(const cJSON *object, const char *key,
                        char *dest, size_t dest_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dest, dest_size, "%s", item->valuestring);
    }
}

static void load_apps(const cJSON *slider, config_slider_t *out)
{
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(slider, "apps");
    if (!cJSON_IsArray(apps)) {
        return;
    }

    out->app_count = 0;

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, apps) {
        if (!cJSON_IsObject(entry) || out->app_count >= CONFIG_APPS_MAX) {
            continue;
        }

        config_app_t *app = &out->apps[out->app_count];
        app->path[0] = 0;
        app->name[0] = 0;

        copy_string(entry, "path", app->path, CONFIG_PATH_MAX);
        copy_string(entry, "name", app->name, CONFIG_NAME_MAX);

        /* The name is only a cache of the basename, so it can be rebuilt. */
        if (app->name[0] == 0 && app->path[0] != 0) {
            snprintf(app->name, CONFIG_NAME_MAX, "%s", config_basename(app->path));
        }

        if (app->path[0] != 0) {
            out->app_count++;
        }
    }
}

bool config_load(const char *path, config_slider_t *out, int count, bool *debug,
                 keys_t *keys)
{
    config_defaults(out, count, debug);
    if (keys) {
        keys_defaults(keys);
    }

    char *text = read_file(path);
    if (text == NULL) {
        return false;
    }

    bool ok = config_from_text(text, out, count, debug, keys);
    free(text);
    return ok;
}

bool config_from_text(const char *text, config_slider_t *out, int count,
                      bool *debug, keys_t *keys)
{
    config_defaults(out, count, debug);
    if (keys) {
        keys_defaults(keys);
    }

    if (text == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        return false;
    }

    /* Read before the sliders are looked at, so a file with a malformed
       "sliders" array still gives the pad back what it had. */
    if (keys) {
        keys_from_json(root, keys);
    }

    if (debug) {
        const cJSON *flag = cJSON_GetObjectItemCaseSensitive(root, "debug");
        if (cJSON_IsBool(flag)) {
            *debug = cJSON_IsTrue(flag) ? true : false;
        }
    }

    const cJSON *sliders = cJSON_GetObjectItemCaseSensitive(root, "sliders");
    if (!cJSON_IsArray(sliders)) {
        cJSON_Delete(root);
        return false;
    }

    int found = 0;
    int position = 0;

    const cJSON *slider = NULL;
    cJSON_ArrayForEach(slider, sliders) {
        if (!cJSON_IsObject(slider)) {
            continue;
        }

        /* Entries land by id, so the file may list them in any order; the
           position in the array is only a fallback for a missing id. */
        const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(slider, "id");
        int id = cJSON_IsNumber(id_item) ? id_item->valueint : position;
        position++;

        if (id < 0 || id >= count) {
            continue;
        }

        const cJSON *value = cJSON_GetObjectItemCaseSensitive(slider, "value");
        if (cJSON_IsNumber(value)) {
            /* A hand-edited or corrupted file must not put a fader outside its
               travel: the knob would be drawn off the window and the value
               written straight back out again. */
            int level = value->valueint;

            if (level < 0) {
                level = 0;
            }
            if (level > CONFIG_SLIDER_MAX) {
                level = CONFIG_SLIDER_MAX;
            }
            out[id].value = level;
        }

        copy_string(slider, "name", out[id].name, CONFIG_NAME_MAX);
        load_apps(slider, &out[id]);

        out[id].id = id;
        found++;
    }

    cJSON_Delete(root);
    return found > 0;
}

char *config_to_text(const config_slider_t *in, int count, bool debug,
                     const keys_t *keys, bool pretty)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return false;
    }

    bool built = (cJSON_AddBoolToObject(root, "debug", debug) != NULL);
    cJSON *sliders = cJSON_AddArrayToObject(root, "sliders");
    built = built && (sliders != NULL);

    for (int i = 0; built && i < count; i++) {
        cJSON *slider = cJSON_CreateObject();
        if (slider == NULL || !cJSON_AddItemToArray(sliders, slider)) {
            cJSON_Delete(slider);
            built = false;
            break;
        }

        built = cJSON_AddNumberToObject(slider, "id", in[i].id) &&
                cJSON_AddNumberToObject(slider, "value", in[i].value) &&
                cJSON_AddStringToObject(slider, "name", in[i].name);

        cJSON *apps = cJSON_AddArrayToObject(slider, "apps");
        built = built && (apps != NULL);

        for (int a = 0; built && a < in[i].app_count; a++) {
            cJSON *app = cJSON_CreateObject();
            if (app == NULL || !cJSON_AddItemToArray(apps, app)) {
                cJSON_Delete(app);
                built = false;
                break;
            }

            built = cJSON_AddStringToObject(app, "path", in[i].apps[a].path) &&
                    cJSON_AddStringToObject(app, "name", in[i].apps[a].name);
        }
    }

    if (built && keys) {
        built = keys_to_json(root, keys);
    }

    char *text = NULL;
    if (built) {
        text = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    return text;
}

bool config_save(const char *path, const config_slider_t *in, int count,
                 bool debug, const keys_t *keys)
{
    /* The file is for reading, so it is always indented. */
    char *text = config_to_text(in, count, debug, keys, true);

    if (text == NULL) {
        fprintf(stderr, "could not build %s\n", path);
        return false;
    }

    /*
     * Written beside the target and moved into place, rather than over it.
     * Truncating the real file first and then failing -- a full disk, a pulled
     * drive, the process killed -- would destroy the only copy of a
     * configuration that is now saved deliberately rather than continuously.
     */
    char temp[CONFIG_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", path);

    FILE *f = fopen(temp, "wb");
    if (f == NULL) {
        fprintf(stderr, "could not write %s\n", temp);
        cJSON_free(text);
        return false;
    }

    fputs(text, f);
    cJSON_free(text);

    bool ok = (ferror(f) == 0);
    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(temp);
        fprintf(stderr, "could not write %s\n", path);
        return false;
    }

    /* rename() will not replace an existing file everywhere, so the old one
       goes first. The data is safe in the temporary either way. */
    remove(path);

    if (rename(temp, path) != 0) {
        fprintf(stderr, "could not replace %s\n", path);
        return false;
    }
    return true;
}
