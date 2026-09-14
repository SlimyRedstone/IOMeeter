#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fader.h"
#include "fonts.h"
#include "filedialog.h"
#include "foreground.h"
#include "instance.h"
#include "keysend.h"
#include "watchdog.h"
#include "respath.h"
#include "tray.h"

#include "clay.h"
#include "clay_raylib_renderer.h"
#include "raylib.h"

/* --------------------------------------------------------------- theme --- */

#define COL(r, g, b, a) ((Clay_Color){ (float)(r), (float)(g), (float)(b), (float)(a) })

static const Clay_Color C_BG        = COL(0x0f, 0x0f, 0x0f, 255);
static const Clay_Color C_CARD      = COL(0x20, 0x20, 0x20, 255);
static const Clay_Color C_FIELD     = COL(0x0f, 0x0f, 0x0f, 255);
static const Clay_Color C_LINE      = COL(0x4f, 0x4f, 0x4f, 255);

/* Container outlines used to be drawn in C_LINE, which read as a grid of
   thin lines over the whole window. Kept as a named colour rather than
   deleting every border, so one value brings them back. */
static const Clay_Color C_BORDER    = COL(0x00, 0x00, 0x00, 128);
static const Clay_Color C_SELECT    = COL(0x2d, 0x4f, 0x8f, 255);
static const Clay_Color C_FG        = COL(0xe7, 0xe9, 0xee, 255);
static const Clay_Color C_MUTED     = COL(0x9a, 0xa1, 0xb1, 255);
static const Clay_Color C_ACCENT    = COL(0x5b, 0x8c, 0xff, 255);
static const Clay_Color C_ACCENT_HI = COL(0x7a, 0xa4, 0xff, 255);
static const Clay_Color C_OK        = COL(0x4e, 0xcb, 0x84, 255);
static const Clay_Color C_WARN      = COL(0xe8, 0xa3, 0x4a, 255);

/* Removal, which is worth telling apart from a warning at a glance. */
static const Clay_Color C_DANGER    = COL(0xd6, 0x3a, 0x45, 255);
static const Clay_Color C_DANGER_HI = COL(0xf0, 0x50, 0x5b, 255);
static const Clay_Color C_RX        = COL(0xc7, 0x9b, 0xff, 255);
static const Clay_Color C_TRANSPARENT = COL(0, 0, 0, 0);

enum { FONT_BODY = 0, FONT_MONO = 1, FONT_COUNT = 2 };

static bool s_menu_open;

/*
 * Which tab is showing. The other tab's containers are simply not declared,
 * so Clay_GetElementData() stops finding them and every interaction keyed off
 * a recorded box goes quiet without having to know about tabs at all.
 */
enum { TAB_MAIN = 0, TAB_CONFIG = 1, TAB_COUNT = 2 };

static const char *const s_tab_names[TAB_COUNT] = { "Main", "Configuration" };

static int s_tab = TAB_MAIN;

/* ------------------------------------------------------------ macro pad --- */

#define KEY_SIZE        60.0f
#define KEY_RADIUS      15.0f

/*
 * A pressed key lights up in its own colour for a moment. The pad has no
 * travel and no sound, so this is the only thing that says the press was
 * taken -- and a key bound to nothing stays dark, which says the other thing.
 */
#define KEY_GLOW_SECONDS 0.40
#define KEY_GLOW_SPREAD  9.0f      /* how far past the edge, at its brightest */
#define KEY_GLOW_RINGS   4
#define KEY_GLOW_ALPHA   150.0f
#define KEY_GAP         6
#define KEY_EDITOR_W    430
#define KEY_STEPS_H     190
#define KEY_LABEL_W     76

/* Comfortably more than the table holds; keysend_names() reports the truth. */
#define KEYSEND_NAMES_MAX 160

#define KEY_PICKER_W      300
#define KEY_PICKER_H      340

/* The key's colour wheel: the disc, the brightness slider beside it, and the
   padding around both. */
#define KEY_COLOUR_W      340

/* The card holding the controller's four state colours. Wide enough for the
   longest hint at caption size. */
#define LED_STATES_W      330

/* One state row: a swatch, what it is for, and its six digits. */
#define LED_ROW_H         30
#define LED_SWATCH_W      32
#define LED_HEX_W         92

/* Characters one frame of recording can take in. Far more than a keyboard
   repeats in a frame; the queue is drained either way. */
#define RECORD_CHARS    8

/* How often the window in front is asked about. Enumerating is cheap but not
   free, and a profile that switches within half a second reads as immediate. */
#define UI_FOREGROUND_POLL 0.4

/* The picker takes the size of what it is showing, between a width that stays
   readable and one the window can hold. */
#define PICKER_MIN_W    300
#define PICKER_MAX_W    560
#define PICKER_MAX_H    420

/* The profile card holds short rows -- a name, two buttons, a few executables
   -- which fit into far less width than they are comfortable to read in. */
#define PROFILES_MIN_W  420

/* Profile the running-application picker is adding to, or -1 when closed. */
static int s_picker = -1;

/* Snapshot taken when the picker opened. Enumerating every frame would walk
   every window on the desktop sixty times a second. */
static foreground_app_t s_running[FOREGROUND_MAX];
static int              s_running_count;

/* Phrase typed into the editor. A macro is either this or a chord. */
static char s_key_text[KEYS_TEXT_MAX];

/* Open list of every key name keysend accepts, for adding one without
   having to record it or spell it correctly. */
static bool s_key_picker;
static const char *s_key_names[KEYSEND_NAMES_MAX];
static int         s_key_name_count;

/* Deferred, like every other removal spotted mid-layout. */
static int s_profile_app_remove = -1;
static int s_profile_app_owner  = -1;
static int s_profile_remove     = -1;

/* Key whose editor is open, or -1. */
static int s_key_editor = -1;

/* While set, keys pressed on the real keyboard are appended to the macro. */
static bool s_key_record;

/* The colour is edited as text so it can be half-typed; the binding only takes
   it once six digits are there. */
static char s_key_hex[8];

/* Whether the key's colour wheel is up, and the colour it is working on.
   Its own hue, saturation and value: the NeoPixel wheel on the other tab uses
   the same widget, and picking a key colour must not drag that with it. */
static bool  s_key_wheel;
static float s_key_wheel_h;
static float s_key_wheel_s;
static float s_key_wheel_v = 1.0f;

/* Likewise the timings, which have to be able to be empty while being typed. */
static char s_key_timing[KEYS_MACRO_MAX][8];

/* The application a key toggles the playback of, while it is being typed. */
static char s_key_media[KEYS_APP_NAME_MAX];

/* And the one its chord is aimed at, for a program that answers a keystroke
   but publishes no media session. */
static char s_key_target[KEYS_APP_NAME_MAX];

/* Narrows the key list to the names containing it. A hundred and twenty-odd
   names is more than anyone should have to scroll through to find one. */
static char s_key_filter[24];

/*
 * A step removal requested this frame. Deferred for the same reason the app
 * list defers its own: the click is seen while the list is being laid out, and
 * closing the gap there would shift the array under the loop walking it.
 */
static int s_key_step_remove = -1;

/*
 * When each key was last played, and the count it was last seen at. The count
 * comes from app.h, so a press from anywhere lights the key; the time is kept
 * here because the fade belongs to the interface rather than to the state.
 */
static double        s_key_glow[KEYS_COUNT];
static unsigned long s_key_glow_seen[KEYS_COUNT];

/* Profile being renamed, or -1. */
static int  s_profile_rename = -1;
static char s_profile_buf[KEYS_PROFILE_NAME_MAX];

/*
 * Captions for unnamed keys, and the editor's title. Clay keeps a pointer to
 * text rather than copying it, so neither can be a local.
 */
/*
 * Long enough for the number of an unnamed key, or the front of a name.
 * Clay draws text at whatever length it is given and the pad has no room to
 * clip in, so a name is cut here instead of running across its neighbours.
 */
#define KEY_CAPTION_CHARS 7

static char s_key_caption[KEYS_COUNT][KEY_CAPTION_CHARS + 1];
static char s_key_title[64];
static char s_key_warning[80];
static char s_picker_title[64];

/*
 * An app removal requested this frame. Deferred rather than applied on the
 * spot: the click is detected while the list is being laid out, and deleting
 * there would shift the array under the loop still walking it.
 */
static int s_remove_slider = -1;
static int s_remove_index = -1;

static int  s_rename = -1;
static char s_rename_buf[CONFIG_NAME_MAX];

/*
 * Invoked by the fader library whenever a value changes. Nothing is sent to the
 * device yet, so this only records the movement; wiring a command in means
 * replacing the body.
 */
static void ui_on_fader_change(int id, int value, void *user)
{
    app_t *app = (app_t *)user;
    if (id < 0 || id >= APP_FADER_COUNT) {
        return;
    }

    /* Deliberately not marked dirty: a moving fader must not rewrite the file.
       Names and app lists still do, and the current values ride along with the
       next save those trigger. */
    app->slider_pending[id] = true;
    app_send_slider(app, id, value);
    app_apply_volume(app, id);
}

/* A fader column is wider than its track so filenames have room underneath. */
#define FADER_COLUMN_W  84
#define APP_ROW_H       24
#define APP_LIST_ROWS   5
#define APP_LIST_H      (APP_ROW_H * APP_LIST_ROWS)

#define UI_PI        3.14159265358979323846f
#define WHEEL_PIXELS 200        /* texture resolution of the colour wheel */
#define LOG_ROW_H    24

/* The gap Clay puts between entries, and the least the panel is worth
   showing: it takes what the cards above have left, down to this. */
#define LOG_GAP      1
#define LOG_MIN_H    120

static Font s_fonts[FONT_COUNT];

/* Refresh rate the loop is running at, for the debug counter to judge by. */
static int s_target_fps = UI_FALLBACK_FPS;

/*
 * Height the faders get. The design heights are quoted for a full-size track,
 * so whatever they allot beyond it is the rest of the interface; the strip
 * takes what is left of the real window and is clamped to a usable range.
 */
static int ui_fader_height(bool debug)
{
    int chrome = UI_WINDOW_HEIGHT_FOR(debug) - FADER_TRACK_HEIGHT;
    int available = GetScreenHeight() - chrome;

    if (available > FADER_TRACK_HEIGHT) {
        available = FADER_TRACK_HEIGHT;
    }
    if (available < FADER_MIN_HEIGHT) {
        available = FADER_MIN_HEIGHT;
    }
    return available;
}

static Texture2D s_wheel;
static float     s_wheel_val = -1.0f;

static char *s_focus;
static size_t s_focus_cap;

/*
 * Clay stores a pointer to text, not a copy, and only reads it when the frame
 * is rendered. Each field therefore needs storage that stays untouched for the
 * rest of the frame; one shared buffer made every field show the same string.
 */
/* Enough for every field that can be on screen at once, which the macro editor
   dominates: a name, a colour, and one command and timing per step. */
/*
 * Scratch for the text fields, one slot per field per frame.
 *
 * The worst case is the macro editor with a full chord: a name, a colour, a
 * phrase, a target and an application, sixteen steps with a timing each, the
 * key list's filter and a profile being renamed -- thirty-nine. Anything past
 * the pool is drawn straight from the caller's buffer rather than being given
 * somebody else's slot, so running out costs a caret, not the wrong text.
 */
#define TEXT_SLOTS 48
/* Long enough for the longest buffer any field edits, and the caret after
   it. A path is the roomiest thing that could reasonably end up in one. */
#define TEXT_SLOT_MAX (CONFIG_PATH_MAX + 2)

static char s_text_slots[TEXT_SLOTS][TEXT_SLOT_MAX];
static int  s_text_slot;

/* --------------------------------------------------------------- helpers -- */

/* Clay_String over a runtime buffer. CLAY_STRING only accepts literals. */
static Clay_String dyn(const char *text)
{
    Clay_String s;
    s.isStaticallyAllocated = false;
    s.length = (int32_t)strlen(text);
    s.chars = text;
    return s;
}

/*
 * Part of a string, for text drawn in pieces.
 *
 * Clay carries a length rather than expecting a terminator, and the renderer
 * copies exactly that many bytes, so a slice can point into the middle of
 * something without a buffer of its own.
 */
static Clay_String slice(const char *text, int length)
{
    Clay_String s;
    s.isStaticallyAllocated = false;
    s.length = (int32_t)length;
    s.chars = text;
    return s;
}

static void HandleClayErrors(Clay_ErrorData errorData)
{
    fprintf(stderr, "clay: %.*s\n", (int)errorData.errorText.length,
            errorData.errorText.chars);
}

static Clay_Color mix(Clay_Color base, Clay_Color hi, bool on)
{
    return on ? hi : base;
}

/* ------------------------------------------------------------- widgets --- */

/*
 * Clay reports hover for the element being declared, so a click is "hovered
 * while the mouse was released this frame".
 */
static bool clicked(bool enabled)
{
    return enabled && Clay_Hovered() && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/* On press rather than release, matching the fader rename. */
static bool right_clicked(void)
{
    return Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
}

static bool ui_button(Clay_ElementId id, const char *label, bool primary,
                      bool enabled, bool grow)
{
    bool hit = false;

    Clay_Color fill = primary ? C_ACCENT : C_CARD;
    Clay_Color text = primary ? C_FG : C_FG;
    if (!enabled) {
        fill = C_LINE;
        text = C_MUTED;
    }

    CLAY(id, {
        .layout = {
            .sizing = { .width = grow ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0),
                        .height = CLAY_SIZING_FIXED(38) },
            .padding = { 14, 14, 8, 8 },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = mix(fill, primary ? C_ACCENT_HI : C_LINE,
                               enabled && Clay_Hovered()),
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
    }) {
        hit = clicked(enabled);
        CLAY_TEXT(dyn(label), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FONT_SIZE_ITEM, .textColor = text }));
    }
    return hit;
}

static bool ui_checkbox(Clay_ElementId id, const char *label, bool *value)
{
    bool hit = false;

    CLAY(id, {
        .layout = {
            .sizing = { .height = CLAY_SIZING_FIXED(28) },
            .childGap = 8,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
    }) {
        hit = clicked(true);

        CLAY_AUTO_ID({
            .layout = { .sizing = { CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18) } },
            .backgroundColor = *value ? C_ACCENT : C_FIELD,
            .cornerRadius = CLAY_CORNER_RADIUS(4),
            .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
        }) {}

        CLAY_TEXT(dyn(label), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION, .textColor = C_MUTED }));
    }

    if (hit) {
        *value = !*value;
    }
    return hit;
}

/*
 * Text field. Clay only lays it out; the caret and editing are handled here
 * against the focused buffer.
 */
/*
 * @param height How tall the box is. The rows of state colours want something
 *               shorter than a standalone field, and the text inside is
 *               centred, so one number covers it.
 */
static void ui_text_field_sized(Clay_ElementId id, char *buffer, size_t cap,
                                int font, bool *submitted, float height)
{
    bool focused = (s_focus == buffer);

    CLAY(id, {
        .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0),
                        .height = CLAY_SIZING_FIXED(height) },
            .padding = { 12, 12, 8, 8 },
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = C_FIELD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = focused ? C_ACCENT : C_BORDER, .width = { 1, 1, 1, 1 } },
    }) {
        if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            s_focus = buffer;
            s_focus_cap = cap;
        }

        bool caret = focused && fmodf((float)GetTime(), 1.0f) < 0.5f;
        const char *shown = buffer;

        if (s_text_slot < TEXT_SLOTS) {
            char *slot = s_text_slots[s_text_slot++];

            snprintf(slot, TEXT_SLOT_MAX, "%s%s", buffer, caret ? "_" : "");
            shown = slot;
        }

        CLAY_TEXT(dyn(shown), CLAY_TEXT_CONFIG({
            .fontId = font, .fontSize = FONT_SIZE_BODY,
            .textColor = buffer[0] ? C_FG : C_MUTED }));
    }

    if (submitted) {
        *submitted = focused && IsKeyPressed(KEY_ENTER);
    }
}

static void ui_text_field(Clay_ElementId id, char *buffer, size_t cap,
                          int font, bool *submitted)
{
    ui_text_field_sized(id, buffer, cap, font, submitted, 38.0f);
}

static void ui_pump_text_input(void)
{
    if (s_focus == NULL) {
        return;
    }

    size_t len = strlen(s_focus);

    for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
        if (c >= 32 && c < 127 && len + 1 < s_focus_cap) {
            s_focus[len++] = (char)c;
            s_focus[len] = '\0';
        }
    }

    /* Repeat on hold, so deleting a long value is not one press per character. */
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && len > 0) {
        s_focus[len - 1] = '\0';
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        s_focus = NULL;
    }
}

/*
 * The NeoPixel colour is edited from two places at once: the wheel and the
 * brightness slider write the hex field through app_sync_hex(), and the field
 * has to be able to write back, or the swatch beside it never follows what was
 * typed. Only a value that changed since it was last seen is parsed, and the
 * field is deliberately not rewritten here -- app_set_rgb() would reformat it
 * mid-word and fight the caret.
 */
static void ui_hex_field_apply(app_t *app)
{
    static char seen[APP_HEX_MAX];

    if (strcmp(app->hex, seen) == 0) {
        return;
    }
    snprintf(seen, sizeof(seen), "%s", app->hex);

    uint32_t rgb = 0;
    if (!keys_parse_hex(app->hex, &rgb)) {
        return;         /* still half typed */
    }

    app_rgb_to_hsv((uint8_t)((rgb >> 16) & 0xFF), (uint8_t)((rgb >> 8) & 0xFF),
                   (uint8_t)(rgb & 0xFF), &app->hue, &app->sat, &app->val);
}

/* --------------------------------------------------------- colour wheel -- */

/*
 * HSV disc: hue around the circumference, saturation along the radius.
 *
 * @param val Brightness the disc is drawn at. One texture serves every wheel
 *            in the interface, so it is rebuilt when the wheel on show wants a
 *            different value -- never more than one of them is visible.
 */
static void ui_rebuild_wheel(float val)
{
    if (fabsf(val - s_wheel_val) < 0.004f && s_wheel.id != 0) {
        return;
    }
    s_wheel_val = val;

    Image img = GenImageColor(WHEEL_PIXELS, WHEEL_PIXELS, BLANK);
    Color *px = (Color *)img.data;

    const float centre = WHEEL_PIXELS / 2.0f;
    const float radius = centre - 1.0f;

    for (int y = 0; y < WHEEL_PIXELS; y++) {
        for (int x = 0; x < WHEEL_PIXELS; x++) {
            float dx = (float)x - centre;
            float dy = (float)y - centre;
            float dist = sqrtf(dx * dx + dy * dy);
            Color *p = &px[y * WHEEL_PIXELS + x];

            if (dist > radius) {
                *p = BLANK;
                continue;
            }

            float angle = atan2f(dy, dx) * 180.0f / UI_PI + 90.0f;
            if (angle < 0.0f) {
                angle += 360.0f;
            }

            uint8_t r, g, b;
            app_hsv_to_rgb(angle, fminf(dist / radius, 1.0f), val, &r, &g, &b);

            p->r = r;
            p->g = g;
            p->b = b;
            /* Feather the rim so the edge is not jagged. */
            p->a = (dist > radius - 1.0f)
                 ? (unsigned char)((radius - dist) * 255.0f)
                 : 255;
        }
    }

    if (s_wheel.id != 0) {
        UnloadTexture(s_wheel);
    }
    s_wheel = LoadTextureFromImage(img);
    UnloadImage(img);
}

/*
 * Hit-test and marker are done outside Clay: the layout supplies the box, the
 * pointer maths happens here.
 */
/*
 * The wheel is drawn as a disc inside a square element, so the hit test has to
 * be the circle rather than the box. A press is only accepted within the disc,
 * and that press then owns the drag: without the ownership flag a press in a
 * corner would be rejected on its first frame but accepted on the next, because
 * IsMouseButtonPressed is only true once.
 */
static bool s_wheel_active;

/*
 * @param hue     Receives the angle under the pointer, 0..360.
 * @param sat     Receives the distance from the centre, 0..1.
 * @param changed Set when either moved. Left alone otherwise, so several
 *                sources can be collected into one flag.
 */
static void ui_wheel_interact(float *hue, float *sat, Clay_BoundingBox box,
                              bool *changed)
{
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_wheel_active = false;
        return;
    }

    Vector2 m = GetMousePosition();
    float cx = box.x + box.width / 2.0f;
    float cy = box.y + box.height / 2.0f;
    float radius = fminf(box.width, box.height) / 2.0f;

    float dx = m.x - cx;
    float dy = m.y - cy;
    float dist = sqrtf(dx * dx + dy * dy);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && dist <= radius) {
        s_wheel_active = true;
    }
    if (!s_wheel_active) {
        return;
    }

    float angle = atan2f(dy, dx) * 180.0f / UI_PI + 90.0f;
    if (angle < 0.0f) {
        angle += 360.0f;
    }

    *hue = angle;
    /* Saturation is clamped at the rim, so a drag may leave the disc and keep
       tracking the hue, which is how a colour wheel is expected to behave. */
    *sat = fminf(dist / radius, 1.0f);
    *changed = true;
}

static void ui_draw_wheel_marker(float hue, float sat, float val,
                                 Clay_BoundingBox box)
{
    float cx = box.x + box.width / 2.0f;
    float cy = box.y + box.height / 2.0f;
    float radius = fminf(box.width, box.height) / 2.0f;
    float angle = (hue - 90.0f) * UI_PI / 180.0f;

    Vector2 at = { cx + sat * radius * cosf(angle),
                   cy + sat * radius * sinf(angle) };

    DrawCircleV(at, 7.0f, WHITE);
    DrawCircleV(at, 5.0f, (Color){ 0, 0, 0, 160 });

    uint8_t r, g, b;
    app_hsv_to_rgb(hue, sat, val, &r, &g, &b);
    DrawCircleV(at, 4.0f, (Color){ r, g, b, 255 });
}

/* -------------------------------------------------------------- slider --- */

/*
 * Interaction only. The element is declared inside the layout; this reads the
 * box Clay recorded last frame, so it must run before Clay_BeginLayout().
 */
static bool ui_slider(Clay_ElementId id, float *value)
{
    Clay_ElementData data = Clay_GetElementData(id);
    if (!data.found || !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        return false;
    }

    Vector2 m = GetMousePosition();
    Clay_BoundingBox b = data.boundingBox;

    bool inside = m.x >= b.x - 6.0f && m.x <= b.x + b.width + 6.0f &&
                  m.y >= b.y - 8.0f && m.y <= b.y + b.height + 8.0f;
    if (!inside) {
        return false;
    }

    float t = (m.x - b.x) / (b.width > 1.0f ? b.width : 1.0f);
    t = fmaxf(0.0f, fminf(1.0f, t));

    if (fabsf(t - *value) <= 0.001f) {
        return false;
    }
    *value = t;
    return true;
}

static void ui_draw_slider_knob(Clay_ElementId id, float value)
{
    Clay_ElementData data = Clay_GetElementData(id);
    if (!data.found) {
        return;
    }

    Clay_BoundingBox b = data.boundingBox;
    float x = b.x + value * b.width;
    float y = b.y + b.height / 2.0f;

    DrawRectangleRounded((Rectangle){ b.x, y - 2.5f, value * b.width, 5.0f },
                         1.0f, 4, (Color){ 0x5b, 0x8c, 0xff, 255 });
    DrawCircle((int)x, (int)y, 7.0f, WHITE);
}

/* ---------------------------------------------------------------- cards -- */

static void ui_card_title(const char *title)
{
    CLAY_AUTO_ID({ .layout = { .padding = { 0, 0, 0, 8 } } }) {
        CLAY_TEXT(dyn(title), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION, .textColor = C_MUTED }));
    }
}

#define UI_CARD(id) CLAY(id, {                                              \
        .layout = {                                                          \
            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },            \
            .padding = CLAY_PADDING_ALL(14),                                 \
            .childGap = 8,                                                   \
            .layoutDirection = CLAY_TOP_TO_BOTTOM,                           \
            /* Anything narrower than the card sits in the middle of it       \
               rather than against its left edge. */                         \
            .childAlignment = { .x = CLAY_ALIGN_X_CENTER },                  \
        },                                                                   \
        .backgroundColor = C_CARD,                                           \
        .cornerRadius = CLAY_CORNER_RADIUS(12),                              \
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },              \
    })

/* As UI_CARD, but filling the width. Only the traffic console uses it: its
   content is arbitrarily wide, so fitting to it would have the panel resize
   itself on every line that arrives. */
#define UI_CARD_WIDE(id) CLAY(id, {                                         \
        .layout = {                                                          \
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },           \
            .padding = CLAY_PADDING_ALL(14),                                 \
            .childGap = 8,                                                   \
            .layoutDirection = CLAY_TOP_TO_BOTTOM,                           \
        },                                                                   \
        .backgroundColor = C_CARD,                                           \
        .cornerRadius = CLAY_CORNER_RADIUS(12),                              \
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },            \
    })

/* As UI_CARD, but never narrower than @p minw. For a card whose contents are
   short enough to fit into less room than they want to be read in. */
#define UI_CARD_MIN(id, minw) CLAY(id, {                                    \
        .layout = {                                                          \
            .sizing = { CLAY_SIZING_FIT(minw), CLAY_SIZING_FIT(0) },         \
            .padding = CLAY_PADDING_ALL(14),                                 \
            .childGap = 8,                                                   \
            .layoutDirection = CLAY_TOP_TO_BOTTOM,                           \
            .childAlignment = { .x = CLAY_ALIGN_X_CENTER },                  \
        },                                                                   \
        .backgroundColor = C_CARD,                                           \
        .cornerRadius = CLAY_CORNER_RADIUS(12),                              \
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },            \
    })

static Clay_Color log_colour(app_log_kind_t kind)
{
    switch (kind) {
    case APP_LOG_TX:    return C_ACCENT;
    case APP_LOG_RX:    return C_RX;
    case APP_LOG_EVENT: return C_OK;
    case APP_LOG_ERROR:
    default:            return C_WARN;
    }
}

/* Pin a clip container to its last row. */
static void ui_scroll_to_start(Clay_ElementId id)
{
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);

    if (sc.found && sc.scrollPosition != NULL) {
        sc.scrollPosition->y = 0.0f;
    }
}

static void ui_scroll_to_end(Clay_ElementId id)
{
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);
    if (!sc.found || sc.scrollPosition == NULL) {
        return;
    }

    float overflow = sc.contentDimensions.height -
                     sc.scrollContainerDimensions.height;
    if (overflow < 0.0f) {
        overflow = 0.0f;
    }

    /* Clay scrolls with a negative offset, so the bottom sits at -overflow. */
    sc.scrollPosition->y = -overflow;
}

/* ----------------------------------------------------------- macro pad --- */

static Clay_Color key_fill(uint32_t rgb, bool hovered)
{
    float lift = hovered ? 34.0f : 0.0f;

    float r = (float)((rgb >> 16) & 0xFF) + lift;
    float g = (float)((rgb >> 8) & 0xFF) + lift;
    float b = (float)(rgb & 0xFF) + lift;

    return (Clay_Color){ fminf(r, 255.0f), fminf(g, 255.0f), fminf(b, 255.0f),
                         255.0f };
}

/* ----------------------------------------------------------- led states -- */

/*
 * Which state colour the wheel is editing, or -1 for the live one.
 *
 * There is one wheel and five things it could be pointed at, so the target is
 * part of the interface rather than of the colour: picking a slot loads it,
 * and everything the wheel does from then on lands in that slot.
 */
static int s_led_target = -1;

/* Point the wheel at a slot, or back at the live colour with -1. */
static void ui_led_target_set(app_t *app, int slot)
{
    s_led_target = slot;

    uint32_t rgb;
    if (slot >= 0 && keys_parse_hex(app->led_hex[slot], &rgb)) {
        app_set_rgb(app, rgb);
    }
}

/*
 * Follow a slot's digits being typed.
 *
 * The wheel writes into the slot, so the slot has to be able to write back, or
 * a colour typed in is not what the wheel then edits. Only the slot being
 * edited moves the wheel; the other three are just text until they are sent.
 */
static void ui_led_hex_apply(app_t *app)
{
    static char seen[APP_LED_SLOT_COUNT][APP_HEX_MAX];

    for (int i = 0; i < APP_LED_SLOT_COUNT; i++) {
        if (strcmp(app->led_hex[i], seen[i]) == 0) {
            continue;
        }
        snprintf(seen[i], APP_HEX_MAX, "%s", app->led_hex[i]);

        /* The wheel writes the slot too, and that comes back through here
           on the next frame; taking it again would round the colour through
           HSV a second time for nothing. */
        uint32_t rgb;
        if (s_led_target == i && keys_parse_hex(app->led_hex[i], &rgb) &&
            rgb != app_rgb(app)) {
            app_set_rgb(app, rgb);
        }
    }
}

/*
 * The four colours the controller shows for itself.
 *
 * They live on the device, not here: the boot and disconnected colours have to
 * be right before any host has said a word. So these are edited as a set and
 * written in one go, rather than following the wheel onto the wire.
 */
static void ui_led_states_card(app_t *app)
{
    /* Clay keeps the pointer rather than the text, so this cannot be local. */
    static char editing[48];

    const app_led_slot_t *chosen = app_led_slot(s_led_target);
    snprintf(editing, sizeof(editing), "wheel: %s",
             chosen ? chosen->label : "live colour");

    UI_CARD_MIN(CLAY_ID("LedStatesCard"), LED_STATES_W) {
        ui_card_title("LED STATES");

        for (int i = 0; i < APP_LED_SLOT_COUNT; i++) {
            const app_led_slot_t *slot = app_led_slot(i);
            if (slot == NULL) {
                continue;
            }

            uint32_t rgb = 0;
            bool known = keys_parse_hex(app->led_hex[i], &rgb);
            bool live = (s_led_target == i);

            CLAY(CLAY_IDI("LedRow", i), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(LED_ROW_H) },
                    .childGap = 8,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                },
            }) {
                /* The swatch is the target selector: clicking it hands the
                   wheel to that slot, clicking it again gives it back. */
                CLAY(CLAY_IDI("LedSwatch", i), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(LED_SWATCH_W),
                                            CLAY_SIZING_FIXED(LED_ROW_H - 4) } },
                    .backgroundColor = known ? COL((rgb >> 16) & 0xFF,
                                                   (rgb >> 8) & 0xFF,
                                                   rgb & 0xFF, 255)
                                             : C_FIELD,
                    .cornerRadius = CLAY_CORNER_RADIUS(7),
                    .border = { .color = live ? C_ACCENT
                                       : (Clay_Hovered() ? C_FG : C_BORDER),
                                .width = { 2, 2, 2, 2 } },
                }) {
                    if (clicked(true)) {
                        ui_led_target_set(app, live ? -1 : i);
                    }
                }

                CLAY_AUTO_ID({
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    },
                }) {
                    CLAY_TEXT(dyn(slot->label), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                        .textColor = C_FG }));

                    CLAY_TEXT(dyn(slot->hint), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION,
                        .textColor = C_MUTED }));
                }

                CLAY_AUTO_ID({ .layout = { .sizing = {
                                   CLAY_SIZING_FIXED(LED_HEX_W),
                                   CLAY_SIZING_FIT(0) } } }) {
                    ui_text_field_sized(CLAY_IDI("LedHex", i), app->led_hex[i],
                                        APP_HEX_MAX, FONT_MONO, NULL,
                                        (float)LED_ROW_H);
                }
            }
        }

        /*
         * One level for all four, which is what the device does with it: it
         * scales whatever colour is being shown rather than any one of them.
         */
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                               CLAY_SIZING_FIT(0) },
                                   .childGap = 8,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(CLAY_STRING("Brightness"), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                .textColor = C_MUTED }));

            CLAY(CLAY_ID("LedBrightness"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                        CLAY_SIZING_FIXED(18) },
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
            }) {
                CLAY_AUTO_ID({
                    .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                            CLAY_SIZING_FIXED(5) } },
                    .backgroundColor = C_LINE,
                    .cornerRadius = CLAY_CORNER_RADIUS(3),
                }) {}
            }

            /* Clay keeps the pointer rather than the text. */
            static char percent[8];

            snprintf(percent, sizeof(percent), "%d%%",
                     (int)lroundf(app->led_brightness * 100.0f));

            CLAY_TEXT(dyn(percent), CLAY_TEXT_CONFIG({
                .fontId = FONT_MONO, .fontSize = FONT_SIZE_SMALL,
                .textColor = C_FG }));
        }

        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                               CLAY_SIZING_FIT(0) },
                                   .childGap = 8,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(dyn(editing), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION,
                .textColor = C_MUTED }));

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

            if (ui_button(CLAY_ID("ReadStates"), "Read", false,
                          app->connected, false)) {
                app_get(app, "config");
            }
            if (ui_button(CLAY_ID("SaveStates"), "Save to controller", true,
                          app->connected, false)) {
                app_set_led_states(app);
            }
        }
    }
}

/* Black on a light key, the usual foreground on a dark one. */
static Clay_Color key_text_colour(uint32_t rgb)
{
    float luma = 0.299f * (float)((rgb >> 16) & 0xFF) +
                 0.587f * (float)((rgb >> 8) & 0xFF) +
                 0.114f * (float)(rgb & 0xFF);

    return (luma > 140.0f) ? COL(0x10, 0x10, 0x10, 255) : C_FG;
}

static void ui_key_editor_open(app_t *app, int id)
{
    keys_binding_t *binding = keys_binding(&app->keys, id, app->keys.profile);
    if (binding == NULL) {
        return;
    }

    s_key_editor = id;
    s_key_record = false;
    s_key_picker = false;
    s_key_wheel = false;
    snprintf(s_key_text, sizeof(s_key_text), "%s", binding->macro.text);
    snprintf(s_key_media, sizeof(s_key_media), "%s", binding->macro.media);
    snprintf(s_key_target, sizeof(s_key_target), "%s", binding->macro.target);
    s_focus = NULL;
    s_menu_open = false;

    keys_format_hex(binding->color, s_key_hex, sizeof(s_key_hex));

    for (int i = 0; i < KEYS_MACRO_MAX; i++) {
        snprintf(s_key_timing[i], sizeof(s_key_timing[i]), "%d",
                 binding->macro.timings[i]);
    }
}

/* Closing is what commits the edit to the file, so a name being typed does not
   rewrite config.json on every keystroke. */
static void ui_key_editor_close(app_t *app)
{
    s_key_editor = -1;
    s_key_record = false;
    s_key_picker = false;
    s_key_wheel = false;
    s_focus = NULL;
    app->config_dirty = true;
}

static void ui_key(app_t *app, int id)
{
    /* keys_binding() rejects the same range, but stating it here is what lets
       the compiler see that the caption below cannot overflow its four bytes:
       through a function call it only knows id is some int. */
    if (id < 0 || id >= KEYS_COUNT) {
        return;
    }

    keys_binding_t *binding = keys_binding(&app->keys, id, app->keys.profile);
    if (binding == NULL) {
        return;
    }

    /* Only the front of a name: the key is sixty pixels wide and Clay has
       nothing to clip text against, so a long one is drawn straight over the
       keys beside it. */
    if (binding->name[0] != 0) {
        snprintf(s_key_caption[id], sizeof(s_key_caption[id]), "%.*s",
                 KEY_CAPTION_CHARS, binding->name);
    } else {
        snprintf(s_key_caption[id], sizeof(s_key_caption[id]), "%d", id + 1);
    }

    const char *label = s_key_caption[id];
    bool editing = (s_key_editor == id);

    CLAY(CLAY_IDI("Key", id), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(KEY_SIZE), CLAY_SIZING_FIXED(KEY_SIZE) },
            .padding = CLAY_PADDING_ALL(5),
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = key_fill(binding->color, Clay_Hovered()),
        .cornerRadius = CLAY_CORNER_RADIUS(KEY_RADIUS),
        /* Always two pixels wide, transparent when idle, so selecting a key
           does not nudge the grid. */
        .border = { .color = editing ? C_ACCENT : C_TRANSPARENT,
                    .width = { 2, 2, 2, 2 } },
    }) {
        if (clicked(true)) {
            app_key_press(app, id);
        }
        if (right_clicked()) {
            ui_key_editor_open(app, id);
        }

        CLAY_TEXT(dyn(label), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
            .textColor = key_text_colour(binding->color) }));
    }
}

/* Footer of the pad: which profile the thirty keys currently mean. */
static void ui_profile_bar(app_t *app)
{
    keys_t *keys = &app->keys;
    bool several = (keys->profile_count > 1);

    CLAY(CLAY_ID("ProfileBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
            .childGap = 6,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
    }) {
        /* Choosing a profile is not an edit to the configuration: it is
           written from the menu, deliberately, like everything else here. */
        if (ui_button(CLAY_ID("ProfilePrev"), "<", false, several, false)) {
            keys->profile = (keys->profile + keys->profile_count - 1) %
                            keys->profile_count;
        }

        if (s_profile_rename >= 0) {
            bool done = false;
            ui_text_field(CLAY_ID("ProfileName"), s_profile_buf,
                          KEYS_PROFILE_NAME_MAX, FONT_BODY, &done);
            if (done) {
                keys_rename_profile(keys, s_profile_rename, s_profile_buf);
                app_log(app, APP_LOG_EVENT, "renamed profile to %s",
                        s_profile_buf);
                s_profile_rename = -1;
                s_focus = NULL;
                app->config_dirty = true;
            }
        } else {
            CLAY(CLAY_ID("ProfileName"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) },
                    .padding = { 10, 10, 6, 6 },
                    .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
                },
                .backgroundColor = C_FIELD,
                .cornerRadius = CLAY_CORNER_RADIUS(8),
            }) {
                /* Right click renames it, the same gesture the faders use. */
                if (right_clicked()) {
                    s_profile_rename = keys->profile;
                    snprintf(s_profile_buf, sizeof(s_profile_buf), "%s",
                             keys->profiles[keys->profile].name);
                    s_focus = s_profile_buf;
                    s_focus_cap = sizeof(s_profile_buf);
                }

                CLAY_TEXT(dyn(keys->profiles[keys->profile].name),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION,
                                             .textColor = C_FG }));
            }
        }

        if (ui_button(CLAY_ID("ProfileNext"), ">", false, several, false)) {
            keys->profile = (keys->profile + 1) % keys->profile_count;
        }

        if (ui_button(CLAY_ID("ProfileAdd"), "+", true,
                      keys->profile_count < KEYS_PROFILES_MAX, false)) {
            int index = keys_add_profile(keys, NULL);
            if (index >= 0) {
                keys->profile = index;
                app->config_dirty = true;
                app_log(app, APP_LOG_EVENT, "added profile %s",
                        keys->profiles[index].name);
            }
        }
    }
}

static void ui_keypad_card(app_t *app)
{
    UI_CARD(CLAY_ID("KeysCard")) {
        ui_card_title("KEYS");

        /* Its own container, so the rows sit KEY_GAP apart rather than at the
           card's wider childGap. */
        CLAY_AUTO_ID({
            .layout = {
                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                .childGap = KEY_GAP,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
        }) {
            for (int row = 0; row < KEYS_ROWS; row++) {
                CLAY(CLAY_IDI("KeyRow", row), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .childGap = KEY_GAP,
                    },
                }) {
                    for (int col = 0; col < KEYS_COLUMNS; col++) {
                        ui_key(app, row * KEYS_COLUMNS + col);
                    }
                }
            }
        }

        ui_profile_bar(app);
    }
}

/* One row of the editor: a caption of fixed width, then whatever follows. */
#define UI_FIELD_ROW() CLAY_AUTO_ID({                                        \
        .layout = {                                                           \
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },            \
            .childGap = 8,                                                    \
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },                   \
        },                                                                    \
    })

static void ui_field_caption(const char *text)
{
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(KEY_LABEL_W),
                                           CLAY_SIZING_FIT(0) } } }) {
        CLAY_TEXT(dyn(text), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL, .textColor = C_MUTED }));
    }
}

static void ui_key_editor_steps(keys_binding_t *binding)
{
    CLAY(CLAY_ID("KeySteps"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(KEY_STEPS_H) },
            .padding = CLAY_PADDING_ALL(6),
            .childGap = 4,
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
        .backgroundColor = C_FIELD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
    }) {
        if (binding->macro.count == 0 && binding->macro.text[0] != 0) {
            CLAY(CLAY_ID("KeyTextRow"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(38) },
                    .childGap = 6,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                },
            }) {
                ui_text_field(CLAY_ID("KeyTextStep"), binding->macro.text,
                              KEYS_TEXT_MAX, FONT_BODY, NULL);

                CLAY_AUTO_ID({
                    .layout = { .sizing = { CLAY_SIZING_FIXED(72),
                                            CLAY_SIZING_FIT(0) } },
                }) {
                    ui_text_field(CLAY_ID("KeyTextMs"), s_key_timing[0],
                                  sizeof(s_key_timing[0]), FONT_MONO, NULL);
                }

                binding->macro.timings[0] =
                    keys_clamp_timing(atoi(s_key_timing[0]));

                CLAY_TEXT(CLAY_STRING("ms"), CLAY_TEXT_CONFIG({
                    .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                    .textColor = C_MUTED }));

                CLAY(CLAY_ID("KeyTextDel"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22) },
                        .childAlignment = { CLAY_ALIGN_X_CENTER,
                                            CLAY_ALIGN_Y_CENTER },
                    },
                    .backgroundColor = Clay_Hovered() ? C_WARN : C_LINE,
                    .cornerRadius = CLAY_CORNER_RADIUS(4),
                }) {
                    if (clicked(true)) {
                        binding->macro.text[0] = 0;
                    }
                    CLAY_TEXT(CLAY_STRING("x"), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                        .textColor = C_FG }));
                }
            }
        }

        for (int i = 0; i < binding->macro.count; i++) {
            CLAY(CLAY_IDI("KeyStepRow", i), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(38) },
                    .childGap = 6,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                },
            }) {
                ui_text_field(CLAY_IDI("KeyCmd", i), binding->macro.cmds[i],
                              KEYS_CMD_MAX, FONT_MONO, NULL);

                /* Narrow box for the wait, which the field then fills. */
                CLAY_AUTO_ID({
                    .layout = { .sizing = { CLAY_SIZING_FIXED(72),
                                            CLAY_SIZING_FIT(0) } },
                }) {
                    ui_text_field(CLAY_IDI("KeyMs", i), s_key_timing[i],
                                  sizeof(s_key_timing[i]), FONT_MONO, NULL);
                }

                CLAY_TEXT(CLAY_STRING("ms"), CLAY_TEXT_CONFIG({
                    .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                    .textColor = C_MUTED }));

                /* Read back every frame, so an edit takes effect as it is
                   typed and an emptied field simply means no wait. */
                binding->macro.timings[i] =
                    keys_clamp_timing(atoi(s_key_timing[i]));

                CLAY(CLAY_IDI("KeyStepDel", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22) },
                        .childAlignment = { CLAY_ALIGN_X_CENTER,
                                            CLAY_ALIGN_Y_CENTER },
                    },
                    .backgroundColor = Clay_Hovered() ? C_WARN : C_LINE,
                    .cornerRadius = CLAY_CORNER_RADIUS(4),
                }) {
                    if (clicked(true)) {
                        s_key_step_remove = i;
                    }
                    CLAY_TEXT(CLAY_STRING("x"), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                        .textColor = C_FG }));
                }
            }
        }

        if (binding->macro.media[0] != 0) {
            /* Not a step list at all: one application, told to start or stop.
               Clay keeps the pointer, so the text cannot be a local. */
            static char shown[96];

            snprintf(shown, sizeof(shown), "play/pause %s",
                     binding->macro.media);

            CLAY_TEXT(dyn(shown), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                .textColor = C_FG }));
        } else if (binding->macro.count == 0 && binding->macro.text[0] == 0) {
            CLAY_TEXT(CLAY_STRING("no steps; press Record and type a shortcut"),
                      CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                                         .textColor = C_MUTED }));
        }
    }
}

/*
 * Floating panel over the whole window. Clay captures the pointer for a
 * floating element by default, so nothing underneath reacts while it is open;
 * only the fader strip needs telling separately, because it hit-tests raw
 * mouse coordinates rather than going through Clay.
 */
static void ui_key_editor(app_t *app)
{
    if (s_key_editor < 0 || s_tab != TAB_MAIN) {
        return;
    }

    keys_binding_t *binding = keys_binding(&app->keys, s_key_editor,
                                           app->keys.profile);
    if (binding == NULL) {
        s_key_editor = -1;
        return;
    }

    /* A half-typed colour simply does not take effect yet. */
    keys_parse_hex(s_key_hex, &binding->color);

    snprintf(s_key_title, sizeof(s_key_title), "KEY %d  -  %s",
             s_key_editor + 1, app->keys.profiles[app->keys.profile].name);

    s_key_warning[0] = 0;
    for (int i = 0; i < binding->macro.count && binding->macro.text[0] == 0; i++) {
        if (!keysend_known(binding->macro.cmds[i])) {
            /* Only as much of the step as identifies it: a text step
               can be the whole width of the field. */
            snprintf(s_key_warning, sizeof(s_key_warning),
                     "step %d: \"%.24s\" is not a key name", i + 1,
                     binding->macro.cmds[i]);
            break;
        }
    }

    CLAY(CLAY_ID("KeyEditor"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(KEY_EDITOR_W), CLAY_SIZING_FIT(0) },
            .padding = CLAY_PADDING_ALL(14),
            .childGap = 8,
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
        .backgroundColor = C_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(12),
        .border = { .color = C_ACCENT, .width = { 1, 1, 1, 1 } },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                              CLAY_ATTACH_POINT_CENTER_CENTER },
            .zIndex = 40,
        },
    }) {
        UI_FIELD_ROW() {
            CLAY_TEXT(dyn(s_key_title), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION, .textColor = C_MUTED }));

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

            if (ui_button(CLAY_ID("KeyClose"), "Close", true, true, false)) {
                ui_key_editor_close(app);
            }
        }

        UI_FIELD_ROW() {
            ui_field_caption("Name");
            ui_text_field(CLAY_ID("KeyName"), binding->name, KEYS_NAME_MAX,
                          FONT_BODY, NULL);
        }

        UI_FIELD_ROW() {
            ui_field_caption("Colour");
            ui_text_field(CLAY_ID("KeyHex"), s_key_hex, sizeof(s_key_hex),
                          FONT_MONO, NULL);

            /* The swatch is the way into the wheel: six hex digits are a poor
               way to choose a colour, and there is nowhere else to put it. */
            CLAY(CLAY_ID("KeySwatch"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(34),
                                        CLAY_SIZING_FIXED(30) } },
                .backgroundColor = key_fill(binding->color, Clay_Hovered()),
                .cornerRadius = CLAY_CORNER_RADIUS(8),
                .border = { .color = s_key_wheel ? C_ACCENT : C_BORDER,
                            .width = { 2, 2, 2, 2 } },
            }) {
                if (clicked(true)) {
                    s_key_wheel = !s_key_wheel;

                    if (s_key_wheel) {
                        app_rgb_to_hsv((uint8_t)((binding->color >> 16) & 0xFF),
                                       (uint8_t)((binding->color >> 8) & 0xFF),
                                       (uint8_t)(binding->color & 0xFF),
                                       &s_key_wheel_h, &s_key_wheel_s,
                                       &s_key_wheel_v);
                        s_focus = NULL;     /* the wheel has it now */
                    }
                }
            }
        }

        UI_FIELD_ROW() {
            ui_field_caption("Macro");

            if (ui_button(CLAY_ID("KeyRecord"), s_key_record ? "Stop" : "Record",
                          s_key_record, true, false)) {
                s_key_record = !s_key_record;
                if (s_key_record) {
                    s_focus = NULL;     /* keystrokes go to the macro now */
                }
            }

            /* Spelling CTRL_LEFT correctly by hand is no fun, so the whole
               table is offered instead. */
            if (ui_button(CLAY_ID("KeyAddStep"), "+ Key", false,
                          binding->macro.count < KEYS_MACRO_MAX, false)) {
                s_key_picker = true;
                s_key_name_count = keysend_names(s_key_names, KEYSEND_NAMES_MAX);

                /* Typing goes straight to the filter: finding one name among
                   a hundred is what this list is really for. */
                s_key_filter[0] = 0;
                s_focus = s_key_filter;
                s_focus_cap = sizeof(s_key_filter);
            }

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

            if (ui_button(CLAY_ID("KeyClear"), "Clear", false,
                          binding->macro.count > 0 ||
                          binding->macro.text[0] != 0 ||
                          binding->macro.media[0] != 0, false)) {
                binding->macro.count = 0;
                binding->macro.text[0] = 0;
                binding->macro.media[0] = 0;
                binding->macro.target[0] = 0;
                s_key_media[0] = 0;
                s_key_target[0] = 0;
            }
        }

        UI_FIELD_ROW() {
            ui_field_caption("Text");

            bool entered = false;
            ui_text_field(CLAY_ID("KeyText"), s_key_text, sizeof(s_key_text),
                          FONT_BODY, &entered);

            /* A macro is one of the three, never two at once, which is how the
               file distinguishes them: taking one drops the others. */
            bool room = (s_key_text[0] != 0);

            if (ui_button(CLAY_ID("KeySetText"), "Use text", false, room, false) ||
                (entered && room)) {
                keys_macro_set_text(&binding->macro, s_key_text);
                snprintf(s_key_timing[0], sizeof(s_key_timing[0]), "%d",
                         binding->macro.timings[0]);
            }
        }

        /*
         * Where the chord goes. Empty is the desktop, which is what a macro
         * has always done; named, the keys are posted to that program even
         * while something else has the focus. It is the way to reach a player
         * that answers its own space bar but tells Windows nothing, which is
         * most of them.
         */
        UI_FIELD_ROW() {
            ui_field_caption("Send to");

            bool aimed = false;
            ui_text_field(CLAY_ID("KeyTarget"), s_key_target,
                          sizeof(s_key_target), FONT_BODY, &aimed);

            if (ui_button(CLAY_ID("KeyTargetPick"), "...", false,
                          filedialog_available(), false)) {
                char path[CONFIG_PATH_MAX];

                if (filedialog_open_program("Choose an application", path,
                                            sizeof(path))) {
                    keys_macro_set_target(&binding->macro, path);
                    snprintf(s_key_target, sizeof(s_key_target), "%s",
                             binding->macro.target);
                }
            }

            if (ui_button(CLAY_ID("KeyTargetSet"), "Aim", false,
                          s_key_target[0] != 0, false) || aimed) {
                keys_macro_set_target(&binding->macro, s_key_target);
                snprintf(s_key_target, sizeof(s_key_target), "%s",
                         binding->macro.target);
            }

            if (ui_button(CLAY_ID("KeyTargetClear"), "Desktop", false,
                          binding->macro.target[0] != 0, false)) {
                keys_macro_set_target(&binding->macro, "");
                s_key_target[0] = 0;
            }
        }

        /*
         * Play/pause for one named application. A media key is global and
         * lands on whichever program played last, so the only way to mean
         * this one rather than that one is to say which.
         */
        UI_FIELD_ROW() {
            ui_field_caption("Media");

            bool entered = false;
            ui_text_field(CLAY_ID("KeyMedia"), s_key_media, sizeof(s_key_media),
                          FONT_BODY, &entered);

            if (ui_button(CLAY_ID("KeyMediaPick"), "...", false,
                          filedialog_available(), false)) {
                char path[CONFIG_PATH_MAX];

                if (filedialog_open_program("Choose an application", path,
                                            sizeof(path))) {
                    /* Only the file name survives: that is what the program
                       can be found by once it is running. */
                    keys_macro_set_media(&binding->macro, path);
                    snprintf(s_key_media, sizeof(s_key_media), "%s",
                             binding->macro.media);
                }
            }

            bool named = (s_key_media[0] != 0);

            if (ui_button(CLAY_ID("KeySetMedia"), "Use app", false, named,
                          false) || (entered && named)) {
                keys_macro_set_media(&binding->macro, s_key_media);
                snprintf(s_key_media, sizeof(s_key_media), "%s",
                         binding->macro.media);
            }
        }

        ui_key_editor_steps(binding);

        if (s_key_warning[0] != 0) {
            CLAY_TEXT(dyn(s_key_warning), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL, .textColor = C_WARN }));
        } else {
            CLAY_TEXT(dyn(binding->macro.media[0]
                          ? "this key starts and stops that application, "
                            "whatever else is playing"
                          : binding->macro.target[0]
                          ? "sent to that application's window; modifiers do "
                            "not carry, single keys do"
                          : "pressed in order, released in reverse; "
                            "the wait comes before each step"),
                      CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                                         .textColor = C_MUTED }));
        }
    }
}

/*
 * The colour wheel for one key.
 *
 * Writes into the same hex field the editor commits from, so there is one path
 * to binding->color rather than two that can disagree. The panel floats over
 * the editor the way the key-name list does; the interaction and the marker
 * are handled with the frame, since both need the box Clay laid out.
 */
static void ui_key_colour_picker(void)
{
    if (!s_key_wheel || s_key_editor < 0) {
        return;
    }

    uint8_t r, g, b;
    app_hsv_to_rgb(s_key_wheel_h, s_key_wheel_s, s_key_wheel_v, &r, &g, &b);

    CLAY(CLAY_ID("KeyColour"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(KEY_COLOUR_W), CLAY_SIZING_FIT(0) },
            .padding = CLAY_PADDING_ALL(14),
            .childGap = 10,
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
        .backgroundColor = C_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(12),
        .border = { .color = C_ACCENT, .width = { 1, 1, 1, 1 } },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                              CLAY_ATTACH_POINT_CENTER_CENTER },
            .zIndex = 50,
        },
    }) {
        UI_FIELD_ROW() {
            CLAY_TEXT(CLAY_STRING("KEY COLOUR"), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION,
                .textColor = C_MUTED }));

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

            if (ui_button(CLAY_ID("KeyColourDone"), "Done", true, true, false)) {
                s_key_wheel = false;
            }
        }

        CLAY_AUTO_ID({ .layout = { .childGap = 12 } }) {
            CLAY(CLAY_ID("KeyWheel"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(150),
                                        CLAY_SIZING_FIXED(150) } },
                .image = { .imageData = &s_wheel },
                .backgroundColor = C_TRANSPARENT,
            }) {}

            CLAY_AUTO_ID({
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 8,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
            }) {
                CLAY_TEXT(CLAY_STRING("Brightness"), CLAY_TEXT_CONFIG({
                    .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                    .textColor = C_MUTED }));

                CLAY(CLAY_ID("KeyBright"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                            CLAY_SIZING_FIXED(18) },
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                }) {
                    CLAY_AUTO_ID({
                        .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                CLAY_SIZING_FIXED(5) } },
                        .backgroundColor = C_LINE,
                        .cornerRadius = CLAY_CORNER_RADIUS(3),
                    }) {}
                }

                /* What the key will look like, rather than what it looks like
                   now: the editor behind this is covered. */
                CLAY_AUTO_ID({
                    .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                            CLAY_SIZING_FIXED(44) } },
                    .backgroundColor = COL(r, g, b, 255),
                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                    .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
                }) {}

                CLAY_TEXT(dyn(s_key_hex), CLAY_TEXT_CONFIG({
                    .fontId = FONT_MONO, .fontSize = FONT_SIZE_BODY,
                    .textColor = C_FG }));
            }
        }
    }
}

/* -------------------------------------------------------- profile apps --- */

static void ui_picker_open(app_t *app, int profile)
{
    if (!foreground_available()) {
        app_log(app, APP_LOG_ERROR, "cannot list running applications: %s",
                foreground_last_error());
        return;
    }

    s_picker = profile;
    s_running_count = foreground_list(s_running, FOREGROUND_MAX);

    if (s_running_count == 0) {
        app_log(app, APP_LOG_ERROR, "no running applications with a window");
    }
}

/*
 * Floating list of what is running, one row per application. Picking one adds
 * it to the profile that opened the picker.
 */
static void ui_app_picker(app_t *app)
{
    if (s_picker < 0 || s_tab != TAB_CONFIG) {
        return;
    }

    if (s_picker >= app->keys.profile_count) {
        s_picker = -1;
        return;
    }

    snprintf(s_picker_title, sizeof(s_picker_title), "RUNNING  -  add to %s",
             app->keys.profiles[s_picker].name);

    CLAY(CLAY_ID("AppPicker"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(PICKER_MIN_W, PICKER_MAX_W),
                        CLAY_SIZING_FIT(0) },
            .padding = CLAY_PADDING_ALL(14),
            .childGap = 8,
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
        .backgroundColor = C_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(12),
        .border = { .color = C_ACCENT, .width = { 1, 1, 1, 1 } },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                              CLAY_ATTACH_POINT_CENTER_CENTER },
            .zIndex = 40,
        },
    }) {
        UI_FIELD_ROW() {
            CLAY_TEXT(dyn(s_picker_title), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION, .textColor = C_MUTED }));

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

            if (ui_button(CLAY_ID("PickerRefresh"), "Refresh", false, true,
                          false)) {
                s_running_count = foreground_list(s_running, FOREGROUND_MAX);
            }
            if (ui_button(CLAY_ID("PickerClose"), "Close", true, true, false)) {
                s_picker = -1;
            }
        }

        CLAY(CLAY_ID("PickerRows"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0),
                            CLAY_SIZING_FIT(0, PICKER_MAX_H) },
                .padding = CLAY_PADDING_ALL(6),
                .childGap = 3,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
            .backgroundColor = C_FIELD,
            .cornerRadius = CLAY_CORNER_RADIUS(8),
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            for (int i = 0; i < s_running_count; i++) {
                CLAY(CLAY_IDI("PickerRow", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(38) },
                        .padding = { 10, 10, 0, 0 },
                        .childGap = 8,
                        .childAlignment = { CLAY_ALIGN_X_CENTER,
                                            CLAY_ALIGN_Y_CENTER },
                    },
                    .backgroundColor = Clay_Hovered() ? C_LINE : C_TRANSPARENT,
                    .cornerRadius = CLAY_CORNER_RADIUS(5),
                }) {
                    if (clicked(true)) {
                        if (keys_add_app(&app->keys, s_picker,
                                         s_running[i].process)) {
                            app->config_dirty = true;
                            app_log(app, APP_LOG_EVENT, "%s selects profile %s",
                                    s_running[i].process,
                                    app->keys.profiles[s_picker].name);
                            s_picker = -1;
                        } else {
                            app_log(app, APP_LOG_ERROR,
                                    "already listed, or the profile is full");
                        }
                    }

                    /* What the program calls itself. The executable is what
                       gets stored and matched on, but nobody thinks of Chrome
                       as chrome.exe. */
                    CLAY_TEXT(dyn(s_running[i].name), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FONT_SIZE_ITEM,
                        .textColor = C_FG }));
                }
            }

            if (s_running_count == 0) {
                CLAY_TEXT(CLAY_STRING("nothing with a window is running"),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY,
                                             .fontSize = FONT_SIZE_BODY,
                                             .textColor = C_MUTED }));
            }
        }
    }
}

/*
 * One profile per row: its name, what selects it, and a way to add more. The
 * active profile is marked, and clicking a name switches to it.
 */
static void ui_profiles_card(app_t *app)
{
    keys_t *keys = &app->keys;

    UI_CARD_MIN(CLAY_ID("ProfilesCard"), PROFILES_MIN_W) {
        ui_card_title("PROFILES");

        for (int p = 0; p < keys->profile_count; p++) {
            bool active = (keys->profile == p);

            CLAY(CLAY_IDI("ProfileRow", p), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .padding = CLAY_PADDING_ALL(8),
                    .childGap = 6,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
                .backgroundColor = C_FIELD,
                .cornerRadius = CLAY_CORNER_RADIUS(8),
                .border = { .color = active ? C_ACCENT : C_TRANSPARENT,
                            .width = { 1, 1, 1, 1 } },
            }) {
                UI_FIELD_ROW() {
                    CLAY(CLAY_IDI("ProfilePick", p), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW(0),
                                        CLAY_SIZING_FIXED(28) },
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                        },
                    }) {
                        if (clicked(!active)) {
                            keys->profile = p;
                        }
                        if (right_clicked()) {
                            s_profile_rename = p;
                            snprintf(s_profile_buf, sizeof(s_profile_buf), "%s",
                                     keys->profiles[p].name);
                            s_focus = s_profile_buf;
                            s_focus_cap = sizeof(s_profile_buf);
                        }

                        CLAY_TEXT(dyn(keys->profiles[p].name),
                                  CLAY_TEXT_CONFIG({
                                      .fontId = FONT_BODY, .fontSize = FONT_SIZE_BODY,
                                      .textColor = active ? C_FG : C_MUTED }));
                    }

                    if (ui_button(CLAY_IDI("ProfileAddApp", p), "+ App", false,
                                  keys->profiles[p].app_count < KEYS_APPS_MAX,
                                  false)) {
                        ui_picker_open(app, p);
                    }

                    /*
                     * Filled while this profile is the fallback, plain
                     * otherwise, and pressing the filled one gives the pad no
                     * fallback at all. Marking any profile clears the rest:
                     * there is only ever one.
                     */
                    bool fallback = keys->profiles[p].is_default;

                    if (ui_button(CLAY_IDI("ProfileDefault", p),
                                  "Set to default", fallback, true, false)) {
                        keys_set_default(keys, fallback ? -1 : p);
                        app->config_dirty = true;
                    }

                    /* Absent on the last profile: the pad always has one. */
                    if (keys->profile_count > 1) {
                        CLAY(CLAY_IDI("ProfileDel", p), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(28),
                                            CLAY_SIZING_FIXED(28) },
                                .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                    CLAY_ALIGN_Y_CENTER },
                            },
                            .backgroundColor = Clay_Hovered() ? C_DANGER_HI
                                                              : C_DANGER,
                            .cornerRadius = CLAY_CORNER_RADIUS(4),
                        }) {
                            if (clicked(true)) {
                                s_profile_remove = p;
                            }
                            CLAY_TEXT(CLAY_STRING("x"), CLAY_TEXT_CONFIG({
                                .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                                .textColor = C_FG }));
                        }
                    }
                }

                if (keys->profiles[p].app_count == 0) {
                    CLAY_TEXT(CLAY_STRING("no applications; this profile is "
                                          "only selected by hand"),
                              CLAY_TEXT_CONFIG({ .fontId = FONT_BODY,
                                                 .fontSize = FONT_SIZE_CAPTION,
                                                 .textColor = C_MUTED }));
                }

                for (int i = 0; i < keys->profiles[p].app_count; i++) {
                    int slot = p * KEYS_APPS_MAX + i;

                    CLAY(CLAY_IDI("ProfileApp", slot), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW(0),
                                        CLAY_SIZING_FIXED(APP_ROW_H) },
                            .childGap = 4,
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                        },
                    }) {
                        CLAY_AUTO_ID({ .layout = { .sizing = {
                                           CLAY_SIZING_GROW(0) } } }) {
                            CLAY_TEXT(dyn(keys->profiles[p].apps[i].name),
                                      CLAY_TEXT_CONFIG({
                                          .fontId = FONT_MONO, .fontSize = FONT_SIZE_SMALL,
                                          .textColor = C_MUTED }));
                        }

                        CLAY(CLAY_IDI("ProfileAppDel", slot), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(13),
                                            CLAY_SIZING_FIXED(13) },
                                .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                    CLAY_ALIGN_Y_CENTER },
                            },
                            .backgroundColor = Clay_Hovered() ? C_WARN : C_LINE,
                            .cornerRadius = CLAY_CORNER_RADIUS(3),
                        }) {
                            if (clicked(true)) {
                                s_profile_app_owner = p;
                                s_profile_app_remove = i;
                            }
                            CLAY_TEXT(CLAY_STRING("x"), CLAY_TEXT_CONFIG({
                                .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                                .textColor = C_FG }));
                        }
                    }
                }
            }
        }

        if (ui_button(CLAY_ID("ProfilesAdd"), "+ Profile", true,
                      keys->profile_count < KEYS_PROFILES_MAX, false)) {
            int index = keys_add_profile(keys, NULL);
            if (index >= 0) {
                keys->profile = index;
                app->config_dirty = true;
                app_log(app, APP_LOG_EVENT, "added profile %s",
                        keys->profiles[index].name);
            }
        }
    }
}

/*
 * Switches profile when the window in front is one a profile claims. Nothing
 * happens when no profile claims it, so a manual choice survives clicking on
 * a program nobody listed -- including this one, which is in front whenever
 * the pad is being edited.
 */
/*
 * Start the halo on any key that has been played since the last frame.
 *
 * Watched rather than set at the click, so a press that arrives from the
 * device one day lights the key without the interface having to know where it
 * came from.
 */
static void ui_key_glow_poll(const app_t *app)
{
    for (int id = 0; id < KEYS_COUNT; id++) {
        if (app->key_press_at[id] != s_key_glow_seen[id]) {
            s_key_glow_seen[id] = app->key_press_at[id];
            s_key_glow[id] = GetTime();
        }
    }
}

/*
 * The halo itself, drawn after Clay has laid the pad out and painted it.
 *
 * Added to what is underneath rather than drawn over it, so the key's own
 * face brightens and the rings past its edge tint the background instead of
 * hiding it. Four of them, each wider and fainter than the last, is enough to
 * read as a glow without a blur to do it properly.
 */
static void ui_draw_key_glow(const app_t *app)
{
    double now = GetTime();

    for (int id = 0; id < KEYS_COUNT; id++) {
        double since = now - s_key_glow[id];

        if (s_key_glow[id] <= 0.0 || since >= KEY_GLOW_SECONDS) {
            continue;
        }

        Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Key", id));
        if (!slot.found) {
            continue;
        }

        const keys_binding_t *binding =
            keys_binding_const(&app->keys, id, app->keys.profile);

        if (binding == NULL) {
            continue;
        }

        /* Brightest at the press and fading from there, squared so it goes
           out quickly rather than lingering as a smear. */
        float life = 1.0f - (float)(since / KEY_GLOW_SECONDS);
        float ease = life * life;

        uint32_t rgb = binding->color;
        Clay_BoundingBox b = slot.boundingBox;

        BeginBlendMode(BLEND_ADDITIVE);

        for (int ring = 0; ring < KEY_GLOW_RINGS; ring++) {
            float grow = KEY_GLOW_SPREAD * (float)ring / (KEY_GLOW_RINGS - 1);
            float alpha = KEY_GLOW_ALPHA * ease / (float)(ring + 1);

            /* The innermost ring lies on the key itself, and at full strength
               it washes the colour towards white -- a red key came out pink.
               Halved, the face brightens without losing whose key it is. */
            if (ring == 0) {
                alpha *= 0.45f;
            }

            Rectangle box = { b.x - grow, b.y - grow,
                              b.width + grow * 2.0f, b.height + grow * 2.0f };

            /* The key's own corner radius, kept in proportion as the ring
               grows, so the halo stays the shape of the key. */
            float roundness = KEY_RADIUS / (fminf(box.width, box.height) / 2.0f);

            DrawRectangleRounded(box, fminf(roundness, 1.0f), 8,
                                 (Color){ (unsigned char)((rgb >> 16) & 0xFF),
                                          (unsigned char)((rgb >> 8) & 0xFF),
                                          (unsigned char)(rgb & 0xFF),
                                          (unsigned char)alpha });
        }

        EndBlendMode();
    }
}

static void ui_foreground_poll(app_t *app)
{
    static double next = 0.0;

    if (!foreground_available() || GetTime() < next) {
        return;
    }
    next = GetTime() + UI_FOREGROUND_POLL;

    char process[FOREGROUND_NAME_MAX];
    if (!foreground_active(process, sizeof(process))) {
        return;
    }

    int profile = keys_profile_for(&app->keys, process);

    /* Nothing claims what is in front, so the profile marked as the fallback
       takes over. With none marked, whatever is selected simply stays. */
    if (profile < 0) {
        profile = keys_default_profile(&app->keys);
    }

    if (profile < 0 || profile == app->keys.profile) {
        return;
    }

    app->keys.profile = profile;
    app_log(app, APP_LOG_EVENT, "%s is in front: profile %s", process,
            app->keys.profiles[profile].name);
}

/*
 * Follows the step list down as it grows. Recording appends below the fold of
 * a fixed-height box, so without this the steps being captured scroll out of
 * sight the moment there are more than five of them.
 */
static void ui_autoscroll_steps(const app_t *app)
{
    static int seen = -1;

    if (s_key_editor < 0) {
        seen = -1;      /* re-arm for whichever key is opened next */
        return;
    }

    const keys_binding_t *binding = keys_binding_const(&app->keys, s_key_editor,
                                                       app->keys.profile);
    if (binding == NULL || binding->macro.count == seen) {
        return;
    }

    seen = binding->macro.count;
    ui_scroll_to_end(CLAY_ID("KeySteps"));
}

/*
 * Whether @p name contains @p needle, whatever the case.
 *
 * An empty needle matches everything, so a filter nobody has typed into shows
 * the whole list.
 */
static bool name_matches(const char *name, const char *needle)
{
    if (needle == NULL || needle[0] == 0) {
        return true;
    }

    for (const char *at = name; *at != 0; at++) {
        size_t i = 0;

        while (needle[i] != 0 && at[i] != 0) {
            char a = at[i], b = needle[i];

            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');

            if (a != b) {
                break;
            }
            i++;
        }

        if (needle[i] == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Every name keysend accepts, to be picked rather than spelled. Floats above
 * the editor, which is itself floating, so it carries the higher z index.
 */
static void ui_key_name_picker(app_t *app)
{
    if (!s_key_picker || s_key_editor < 0) {
        return;
    }

    keys_binding_t *binding = keys_binding(&app->keys, s_key_editor,
                                           app->keys.profile);
    if (binding == NULL) {
        s_key_picker = false;
        return;
    }

    CLAY(CLAY_ID("KeyNames"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(KEY_PICKER_W), CLAY_SIZING_FIT(0) },
            .padding = CLAY_PADDING_ALL(14),
            .childGap = 8,
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
        .backgroundColor = C_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(12),
        .border = { .color = C_ACCENT, .width = { 1, 1, 1, 1 } },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                              CLAY_ATTACH_POINT_CENTER_CENTER },
            .zIndex = 50,
        },
    }) {
        UI_FIELD_ROW() {
            CLAY_TEXT(CLAY_STRING("PICK A KEY"), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION, .textColor = C_MUTED }));

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

            if (ui_button(CLAY_ID("KeyNamesClose"), "Close", true, true, false)) {
                s_key_picker = false;
            }
        }

        /* Typed into rather than scrolled through. */
        UI_FIELD_ROW() {
            ui_field_caption("Find");
            ui_text_field(CLAY_ID("KeyFilter"), s_key_filter,
                          sizeof(s_key_filter), FONT_MONO, NULL);
        }

        CLAY(CLAY_ID("KeyNameRows"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0),
                            CLAY_SIZING_FIXED(KEY_PICKER_H) },
                .padding = CLAY_PADDING_ALL(6),
                .childGap = 2,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
            .backgroundColor = C_FIELD,
            .cornerRadius = CLAY_CORNER_RADIUS(8),
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            for (int i = 0; i < s_key_name_count; i++) {
                /* Indexed by position in the whole list, so the identifiers
                   stay unique and stable as the filter narrows it. */
                if (!name_matches(s_key_names[i], s_key_filter)) {
                    continue;
                }

                CLAY(CLAY_IDI("KeyNameRow", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) },
                        .padding = { 10, 10, 0, 0 },
                        .childAlignment = { CLAY_ALIGN_X_CENTER,
                                            CLAY_ALIGN_Y_CENTER },
                    },
                    .backgroundColor = Clay_Hovered() ? C_LINE : C_TRANSPARENT,
                    .cornerRadius = CLAY_CORNER_RADIUS(5),
                }) {
                    if (clicked(true)) {
                        int at = binding->macro.count;

                        if (keys_macro_add(&binding->macro, s_key_names[i],
                                           KEYS_TIMING_DEFAULT)) {
                            snprintf(s_key_timing[at], sizeof(s_key_timing[at]),
                                     "%d", KEYS_TIMING_DEFAULT);
                        }
                        s_key_picker = false;
                    }

                    CLAY_TEXT(dyn(s_key_names[i]), CLAY_TEXT_CONFIG({
                        .fontId = FONT_MONO, .fontSize = FONT_SIZE_CAPTION,
                        .textColor = C_FG }));
                }
            }
        }
    }
}

/*
 * Appends whatever is typed while recording. raylib keeps this queue separate
 * from the character queue the text fields drain, so the two do not fight.
 */
static void ui_key_record_poll(app_t *app)
{
    if (!s_key_record || s_key_editor < 0 || s_key_picker) {
        return;
    }

    keys_binding_t *binding = keys_binding(&app->keys, s_key_editor,
                                           app->keys.profile);
    if (binding == NULL) {
        return;
    }

    /*
     * Two queues, filled in step by the same press. GetKeyPressed() reports
     * where the key sits, which only reads correctly on a US layout and is
     * fixed at the layout the window opened with. GetCharPressed() reports
     * what the layout actually produced, now, so the character wins wherever
     * there is one and the key marked A records as A rather than as Q.
     *
     * A modifier suppresses the character on most systems, so a chord typed
     * all at once still falls back to positions; recording the keys one at a
     * time gets the layout right.
     */
    int chars[RECORD_CHARS];
    int char_count = 0;

    for (int c = GetCharPressed(); c > 0 && char_count < RECORD_CHARS;
         c = GetCharPressed()) {
        chars[char_count++] = c;
    }

    int taken = 0;

    for (int key = GetKeyPressed(); key > 0; key = GetKeyPressed()) {
        const char *name = NULL;

        /* Only the printable half of the keyboard produces a character;
           modifiers, arrows and function keys keep their position. */
        if (key < 256 && taken < char_count) {
            name = keysend_name_of_char(chars[taken++]);
        }
        if (name == NULL) {
            name = keysend_name_of(key);
        }
        if (name == NULL) {
            continue;
        }

        int step = binding->macro.count;
        if (!keys_macro_add(&binding->macro, name, KEYS_TIMING_DEFAULT)) {
            s_key_record = false;
            app_log(app, APP_LOG_ERROR, "macro is full");
            break;
        }

        snprintf(s_key_timing[step], sizeof(s_key_timing[step]), "%d",
                 KEYS_TIMING_DEFAULT);
    }
}

/* ------------------------------------------------------------- battery --- */

#define BATTERY_W       34.0f
#define BATTERY_H       17.0f
#define BATTERY_NUB_W   3.0f
#define BATTERY_INSET   2.0f

/* Green while there is plenty, amber once it is worth noticing, red when it
   is nearly out. */
static Clay_Color battery_colour(float level)
{
    if (level > 40.0f) {
        return C_OK;
    }
    return (level > 15.0f) ? C_WARN : COL(0xe8, 0x6a, 0x5a, 255);
}

/*
 * Drawn out of three rectangles rather than an image: a shell, the charge
 * inside it, and the nub on the end. Nothing to load, and it takes the theme
 * colours with it.
 */
static void ui_battery(const app_t *app)
{
    static char text[16];

    float level = app->battery;
    if (level < 0.0f) {
        level = 0.0f;
    }
    if (level > 100.0f) {
        level = 100.0f;
    }

    app_battery_text(app, text, sizeof(text));

    Clay_Color tint = battery_colour(level);
    float inner = BATTERY_W - 2.0f * BATTERY_INSET;
    float filled = inner * (level / 100.0f);

    if (filled < 1.0f && level > 0.0f) {
        filled = 1.0f;      /* a sliver, so "nearly empty" is not "empty" */
    }

    CLAY(CLAY_ID("Battery"), {
        .layout = {
            .childGap = 6,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
    }) {
        CLAY_AUTO_ID({
            .layout = { .childGap = 1,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
        }) {
            /* Shell, with the charge inset inside it. */
            CLAY_AUTO_ID({
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(BATTERY_W),
                                CLAY_SIZING_FIXED(BATTERY_H) },
                    .padding = CLAY_PADDING_ALL((uint16_t)BATTERY_INSET),
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                },
                .cornerRadius = CLAY_CORNER_RADIUS(3),
                .border = { .color = tint, .width = { 1, 1, 1, 1 } },
            }) {
                CLAY_AUTO_ID({
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(filled),
                                    CLAY_SIZING_GROW(0) },
                    },
                    .backgroundColor = tint,
                    .cornerRadius = CLAY_CORNER_RADIUS(1),
                }) {}
            }

            CLAY_AUTO_ID({
                .layout = { .sizing = { CLAY_SIZING_FIXED(BATTERY_NUB_W),
                                        CLAY_SIZING_FIXED(BATTERY_H / 2.0f) } },
                .backgroundColor = tint,
                .cornerRadius = CLAY_CORNER_RADIUS(1),
            }) {}
        }

        CLAY_TEXT(dyn(text), CLAY_TEXT_CONFIG({
            .fontId = FONT_MONO, .fontSize = FONT_SIZE_SMALL, .textColor = tint }));
    }
}

/* ---------------------------------------------------------------- tabs --- */

/*
 * Tab strip under the header. The active tab is filled and underlined; the
 * rest stay muted labels until the pointer is over them.
 */
static void ui_tab_bar(void)
{
    CLAY(CLAY_ID("TabBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0),
                        CLAY_SIZING_FIXED(UI_TAB_BAR_HEIGHT) },
            .childGap = 4,
        },
    }) {
        for (int i = 0; i < TAB_COUNT; i++) {
            bool active = (s_tab == i);

            CLAY(CLAY_IDI("Tab", i), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
                .backgroundColor = (active || Clay_Hovered()) ? C_CARD
                                                              : C_TRANSPARENT,
                .cornerRadius = (Clay_CornerRadius){ 8, 8, 0, 0 },
            }) {
                if (clicked(!active)) {
                    s_tab = i;
                    /* These all belong to elements that are about to stop
                       being laid out, so none of them could be finished. */
                    s_rename = -1;
                    s_profile_rename = -1;
                    s_key_record = false;
                    s_key_editor = -1;
                    s_key_wheel = false;
                    s_picker = -1;
                    s_focus = NULL;
                }

                CLAY_AUTO_ID({
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0) },
                        .padding = { 18, 18, 0, 0 },
                        .childAlignment = { CLAY_ALIGN_X_CENTER,
                                            CLAY_ALIGN_Y_CENTER },
                    },
                }) {
                    CLAY_TEXT(dyn(s_tab_names[i]), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FONT_SIZE_BODY,
                        .textColor = active ? C_FG : C_MUTED }));
                }

                CLAY_AUTO_ID({
                    .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                            CLAY_SIZING_FIXED(2) } },
                    .backgroundColor = active ? C_ACCENT : C_TRANSPARENT,
                }) {}
            }
        }
    }
}

/* ---------------------------------------------------------------- menu --- */

#define MENU_WIDTH    230.0f
#define MENU_ITEM_H   40.0f
#define MENU_PAD      6.0f
#define MENU_ITEMS    3

static const char *const s_menu_items[MENU_ITEMS] = {
    "Save config", "Load config", "Reload config",
};

static Rectangle ui_menu_rect(Clay_BoundingBox button)
{
    return (Rectangle){
        button.x,
        button.y + button.height + 6.0f,
        MENU_WIDTH,
        MENU_ITEMS * MENU_ITEM_H + 2.0f * MENU_PAD,
    };
}

static int ui_menu_hit(Rectangle panel)
{
    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, panel)) {
        return -1;
    }

    float local = mouse.y - (panel.y + MENU_PAD);
    if (local < 0.0f) {
        return -1;
    }

    int index = (int)(local / MENU_ITEM_H);
    return (index >= 0 && index < MENU_ITEMS) ? index : -1;
}

/*
 * Drawn with raylib after everything else. Clay renders before the colour
 * wheel marker, the slider knob and the faders, so a Clay-drawn panel ended up
 * underneath them and their pointer handling ignored it.
 */
static void ui_draw_menu(Rectangle panel)
{
    int hovered = ui_menu_hit(panel);

    DrawRectangleRounded(panel, 0.12f, 8, (Color){ 0x2b, 0x30, 0x39, 255 });

    Rectangle inner = { panel.x + 1.0f, panel.y + 1.0f,
                        panel.width - 2.0f, panel.height - 2.0f };
    DrawRectangleRounded(inner, 0.12f, 8, (Color){ 0x1c, 0x1f, 0x25, 255 });

    for (int i = 0; i < MENU_ITEMS; i++) {
        Rectangle row = {
            panel.x + MENU_PAD,
            panel.y + MENU_PAD + (float)i * MENU_ITEM_H,
            panel.width - 2.0f * MENU_PAD,
            MENU_ITEM_H,
        };

        if (i == hovered) {
            DrawRectangleRounded(row, 0.25f, 8, (Color){ 0x5b, 0x8c, 0xff, 255 });
        }

        Font font = s_fonts[FONT_BODY];
        float size = (float)FONT_SIZE_ITEM;
        Vector2 extent = MeasureTextEx(font, s_menu_items[i], size, 0.0f);

        DrawTextEx(font, s_menu_items[i],
                   (Vector2){ row.x + 10.0f,
                              row.y + (row.height - extent.y) / 2.0f },
                   size, 0.0f, (Color){ 0xe7, 0xe9, 0xee, 255 });
    }
}

static void ui_add_app(app_t *app, int slider)
{
    char path[CONFIG_PATH_MAX];

    if (!filedialog_open_program("Choose an application", path, sizeof(path))) {
        if (!filedialog_available()) {
            app_log(app, APP_LOG_ERROR,
                    "no file chooser available (install zenity or kdialog)");
        }
        return;
    }

    if (!config_add_app(&app->sliders[slider], path)) {
        app_log(app, APP_LOG_ERROR, "already listed, or the slider is full");
        return;
    }

    app->config_dirty = true;

    int last = app->sliders[slider].app_count - 1;
    app_log(app, APP_LOG_EVENT, "%s controls %s",
            app->sliders[slider].name, app->sliders[slider].apps[last].name);
    app_apply_volume(app, slider);
}

static void ui_menu_save_config(app_t *app)
{
    char path[512];

    if (!filedialog_save("Save configuration", "config.json", path, sizeof(path))) {
        if (!filedialog_available()) {
            app_log(app, APP_LOG_ERROR,
                    "no file chooser available (install zenity or kdialog)");
        }
        return;
    }
    app_config_save_as(app, path);
}

/* Re-reads whichever file the configuration last came from, so an edit made
   outside the program can be picked up without restarting it. */
static void ui_menu_reload_config(app_t *app)
{
    app_config_reload(app);
}

static void ui_menu_load_config(app_t *app)
{
    char path[512];

    if (!filedialog_open("Load configuration", path, sizeof(path))) {
        if (!filedialog_available()) {
            app_log(app, APP_LOG_ERROR,
                    "no file chooser available (install zenity or kdialog)");
        }
        return;
    }
    app_config_load_from(app, path);
}

/*
 * Rename editor. Typing is fed by ui_pump_text_input() through s_focus; this
 * only deals with finishing or abandoning the edit.
 */
static void ui_update_rename(app_t *app)
{
    if (s_rename < 0) {
        return;
    }

    if (IsKeyPressed(KEY_ENTER)) {
        if (s_rename_buf[0] != (char)0) {
            snprintf(app->sliders[s_rename].name, CONFIG_NAME_MAX, "%s",
                     s_rename_buf);
            app->config_dirty = true;
            app_log(app, APP_LOG_EVENT, "renamed fader %d to %s",
                    s_rename, s_rename_buf);
        }
        s_rename = -1;
        s_focus = NULL;
        return;
    }

    /* Escape abandons the edit. ui_pump_text_input() also drops focus on
       escape, which is harmless here. */
    if (IsKeyPressed(KEY_ESCAPE)) {
        s_rename = -1;
        s_focus = NULL;
    }
}

static void ui_draw_rename(const app_t *app, Clay_BoundingBox box)
{
    (void)app;

    Font font = s_fonts[FONT_BODY];
    float size = (float)FONT_SIZE_ITEM;

    char shown[CONFIG_NAME_MAX + 2];
    snprintf(shown, sizeof(shown), "%s%s", s_rename_buf,
             (fmodf((float)GetTime(), 1.0f) < 0.5f) ? "_" : "");

    Vector2 extent = MeasureTextEx(font, shown, size, 0.0f);
    float width = fmaxf(extent.x + 20.0f, 150.0f);
    float height = 34.0f;

    float x = box.x + box.width / 2.0f - width / 2.0f;
    float y = box.y - height - 8.0f;

    if (x < 6.0f) {
        x = 6.0f;
    }
    if (x + width > (float)GetScreenWidth() - 6.0f) {
        x = (float)GetScreenWidth() - width - 6.0f;
    }
    if (y < 6.0f) {
        y = box.y + 6.0f;
    }

    Rectangle rect = { x, y, width, height };
    DrawRectangleRounded(rect, 0.25f, 8, (Color){ 0x5b, 0x8c, 0xff, 255 });

    Rectangle inner = { x + 1.0f, y + 1.0f, width - 2.0f, height - 2.0f };
    DrawRectangleRounded(inner, 0.25f, 8, (Color){ 0x14, 0x16, 0x1a, 255 });

    DrawTextEx(font, shown,
               (Vector2){ x + 10.0f, y + (height - extent.y) / 2.0f },
               size, 0.0f, (Color){ 0xe7, 0xe9, 0xee, 255 });
}

/* --------------------------------------------------------- notification -- */

#define TOAST_SECONDS   4.0
#define TOAST_FADE      0.8

static unsigned long s_toast_seen;
static double        s_toast_until;

/*
 * Drawn with raylib rather than Clay: it floats above the finished layout and
 * fades, neither of which the layout pass is involved in.
 */
static void ui_draw_toast(const app_t *app)
{
    if (GetTime() >= s_toast_until || app->notice[0] == '\0') {
        return;
    }

    double remaining = s_toast_until - GetTime();
    float alpha = (remaining < TOAST_FADE) ? (float)(remaining / TOAST_FADE) : 1.0f;

    Font font = s_fonts[FONT_BODY];
    const char *title = "Interrupt";
    Vector2 title_size = MeasureTextEx(font, title, (float)FONT_SIZE_BODY, 0.0f);
    Vector2 text_size = MeasureTextEx(font, app->notice,
                                      (float)FONT_SIZE_NOTICE, 0.0f);

    float width = fmaxf(title_size.x, text_size.x) + 28.0f;
    float height = 58.0f;
    float x = (float)GetScreenWidth() - width - 18.0f;
    float y = (float)GetScreenHeight() - height - 18.0f;

    Rectangle box = { x, y, width, height };

    /*
     * The border is an outer rounded rect with the fill inset by a pixel.
     * DrawRectangleRoundedLines is avoided on purpose: raylib 5.5 dropped its
     * lineThick parameter and moved it to DrawRectangleRoundedLinesEx, so the
     * call does not compile against both 5.0 and 5.5.
     */
    DrawRectangleRounded(box, 0.18f, 8,
                         Fade((Color){ 0x5b, 0x8c, 0xff, 255 }, alpha));

    Rectangle inner = { box.x + 1.0f, box.y + 1.0f,
                        box.width - 2.0f, box.height - 2.0f };
    DrawRectangleRounded(inner, 0.18f, 8,
                         Fade((Color){ 0x1c, 0x1f, 0x25, 255 }, alpha));

    DrawTextEx(font, title, (Vector2){ x + 14.0f, y + 9.0f },
               (float)FONT_SIZE_BODY, 0.0f,
               Fade((Color){ 0x9a, 0xa1, 0xb1, 255 }, alpha));
    DrawTextEx(font, app->notice, (Vector2){ x + 14.0f, y + 33.0f },
               (float)FONT_SIZE_NOTICE, 0.0f,
               Fade((Color){ 0xe7, 0xe9, 0xee, 255 }, alpha));
}

/*
 * Keep the newest line in view, but stop doing so once the user scrolls up, so
 * reading back through the traffic is not fought by every arriving packet.
 * Scrolling back to the bottom re-arms it.
 */
static void ui_autoscroll_log(const app_t *app, Clay_ElementId id)
{
    static unsigned long seen_seq = 0;
    static bool stick = true;

    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);
    if (!sc.found || sc.scrollPosition == NULL) {
        return;
    }

    float overflow = sc.contentDimensions.height - sc.scrollContainerDimensions.height;
    if (overflow < 0.0f) {
        overflow = 0.0f;
    }

    /* Clay scrolls with a negative offset, so the bottom sits at -overflow. */
    bool at_bottom = (-sc.scrollPosition->y) >= overflow - 4.0f;

    if (app->log_seq != seen_seq) {
        seen_seq = app->log_seq;
        if (stick) {
            sc.scrollPosition->y = -overflow;
            return;     /* just moved it; judge the position again next frame */
        }
    }

    stick = at_bottom;
}

/* ------------------------------------------------------- log selection --- */

#define LOG_PAD         8.0f
#define LOG_BAR_W       10.0f

/*
 * Characters of a log line that fit across the panel, or 0 before it has been
 * measured. Lines are cut to this rather than left to run off the side, which
 * is the only way they can be read without widening the window.
 */
static int s_log_cols;

static void ui_log_measure(void)
{
    Clay_ElementData panel = Clay_GetElementData(CLAY_ID("LogScroll"));
    if (!panel.found) {
        return;             /* not laid out yet; the whole line goes on one row */
    }

    /* Nothing to work out while the panel is the width it was last frame,
       which is every frame but the ones where the window is being dragged. */
    static float seen_width = -1.0f;

    if (panel.boundingBox.width == seen_width) {
        return;
    }
    seen_width = panel.boundingBox.width;

    /* The log is set in the monospaced face, so one character's advance is
       every character's and the count can be arithmetic rather than a
       measurement per line. */
    float ch = MeasureTextEx(s_fonts[FONT_MONO], "0123456789",
                             (float)FONT_SIZE_SMALL, 0.0f).x / 10.0f;
    if (ch < 1.0f) {
        return;
    }

    /* Short of the scrollbar whether or not it is there: a log that reflows
       the moment it overflows is worse than one a few characters narrower. */
    float room = panel.boundingBox.width - 2.0f * LOG_PAD - LOG_BAR_W - 4.0f;
    int cols = (int)(room / ch);

    s_log_cols = (cols > 8) ? cols : 8;
}

/*
 * How much of @p text from @p at belongs on one row.
 *
 * Cut on the column count rather than on words: a log line is mostly JSON,
 * which has no spaces in it, so wrapping by word would not wrap at all. A cut
 * that would land inside a UTF-8 character is moved back to its start.
 */
/*
 * @param len How long @p text is. Passed in because this is called once per
 *            row of every entry, twice a frame, and measuring the same line
 *            again for each of them is the sort of work that does not show up
 *            until the log is full.
 */
static int log_segment(const char *text, int len, int at, int cols)
{
    int left = len - at;

    if (cols <= 0 || left <= cols) {
        return (left > 0) ? left : 0;
    }

    int take = cols;
    while (take > 1 && ((unsigned char)text[at + take] & 0xC0) == 0x80) {
        take--;     /* 10xxxxxx: the character began earlier */
    }
    return take;
}

/* Rows one entry occupies. An empty one still takes a row of its own. */
static int log_rows(const char *text, int cols)
{
    int len = (int)strlen(text);
    int rows = 0;

    for (int at = 0; at < len; rows++) {
        at += log_segment(text, len, at, cols);
    }
    return (rows > 0) ? rows : 1;
}

/* Inclusive range of selected lines; -1 when nothing is selected. */
static int   s_log_from = -1;
static int   s_log_to   = -1;
static bool  s_log_selecting;

/*
 * Per-bar drag state. Two clip containers carry a bar now -- the traffic log
 * and the macro step list -- and a drag belongs to one of them.
 */
typedef struct {
    bool  dragging;
    float grab;                 /* pointer offset inside the thumb, in pixels */
} ui_bar_t;

static ui_bar_t s_log_bar;
static ui_bar_t s_step_bar;

/*
 * The entry at @p y, measured down from the top of the scrolled content.
 *
 * Entries are no longer all one row high, so the position is walked rather
 * than divided. The log holds a few hundred at most and this runs once a
 * frame, which is cheaper than keeping a table of tops in step with the ring.
 */
static int log_line_at(const app_t *app, float y)
{
    float top = 0.0f;

    for (int i = 0; i < app->log_count; i++) {
        float height = (float)log_rows(app_log_at(app, i)->text, s_log_cols)
                     * LOG_ROW_H + LOG_GAP;

        if (y < top + height) {
            return i;
        }
        top += height;
    }
    return (app->log_count > 0) ? app->log_count - 1 : 0;
}

static bool log_line_selected(int index)
{
    if (s_log_from < 0 || s_log_to < 0) {
        return false;
    }

    int lo = (s_log_from < s_log_to) ? s_log_from : s_log_to;
    int hi = (s_log_from < s_log_to) ? s_log_to : s_log_from;
    return index >= lo && index <= hi;
}

/* Geometry of the scrollbar track, or a zero-width rectangle when the content
   fits and no bar is needed. */
static Rectangle log_bar_track(Clay_BoundingBox box, float overflow)
{
    if (overflow <= 0.0f) {
        return (Rectangle){ 0, 0, 0, 0 };
    }
    return (Rectangle){ box.x + box.width - LOG_BAR_W - 4.0f,
                        box.y + 4.0f,
                        LOG_BAR_W,
                        box.height - 8.0f };
}

static Rectangle log_bar_thumb(Rectangle track, float overflow,
                               float content_h, float view_h, float scroll_y)
{
    float ratio = (content_h > 0.0f) ? view_h / content_h : 1.0f;
    float h = track.height * ratio;

    if (h < 24.0f) {
        h = 24.0f;
    }
    if (h > track.height) {
        h = track.height;
    }

    float progress = (overflow > 0.0f) ? (-scroll_y) / overflow : 0.0f;
    progress = fminf(fmaxf(progress, 0.0f), 1.0f);

    return (Rectangle){ track.x, track.y + progress * (track.height - h),
                        track.width, h };
}

/* Copy the selected lines, oldest first, one per line. */
static void ui_log_copy(const app_t *app)
{
    if (s_log_from < 0 || app->log_count <= 0) {
        return;
    }

    int lo = (s_log_from < s_log_to) ? s_log_from : s_log_to;
    int hi = (s_log_from < s_log_to) ? s_log_to : s_log_from;

    if (lo < 0) {
        lo = 0;
    }
    if (hi >= app->log_count) {
        hi = app->log_count - 1;
    }

    size_t need = 1;
    for (int i = lo; i <= hi; i++) {
        need += strlen(app_log_at(app, i)->text) + 1;
    }

    char *text = malloc(need);
    if (text == NULL) {
        return;
    }
    text[0] = '\0';

    for (int i = lo; i <= hi; i++) {
        strcat(text, app_log_at(app, i)->text);
        if (i < hi) {
            strcat(text, "\n");
        }
    }

    SetClipboardText(text);
    free(text);
}

/*
 * Scrollbar dragging and line selection, both against the box Clay recorded
 * last frame. Runs before Clay_BeginLayout(), so the highlight drawn this
 * frame reflects the pointer as it is now.
 */
static void ui_log_interact(const app_t *app)
{
    if (!app->debug) {
        return;
    }

    Clay_ElementData panel = Clay_GetElementData(CLAY_ID("LogScroll"));
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(CLAY_ID("LogScroll"));

    if (!panel.found || !sc.found || sc.scrollPosition == NULL) {
        return;
    }

    Clay_BoundingBox box = panel.boundingBox;
    float content_h = sc.contentDimensions.height;
    float view_h = sc.scrollContainerDimensions.height;
    float overflow = content_h - view_h;

    if (overflow < 0.0f) {
        overflow = 0.0f;
    }

    Vector2 mouse = GetMousePosition();
    Rectangle track = log_bar_track(box, overflow);
    Rectangle thumb = log_bar_thumb(track, overflow, content_h, view_h,
                                    sc.scrollPosition->y);

    /* --- scrollbar ---------------------------------------------------- */
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_log_bar.dragging = false;
    }

    if (track.width > 0.0f && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(mouse, track)) {
        if (CheckCollisionPointRec(mouse, thumb)) {
            s_log_bar.dragging = true;
            s_log_bar.grab = mouse.y - thumb.y;
        } else {
            /* Clicking the track jumps so the thumb centres on the pointer. */
            float span = track.height - thumb.height;
            float progress = (span > 0.0f)
                           ? (mouse.y - track.y - thumb.height / 2.0f) / span
                           : 0.0f;
            progress = fminf(fmaxf(progress, 0.0f), 1.0f);
            sc.scrollPosition->y = -progress * overflow;
        }
        return;         /* the press belongs to the bar, not to the text */
    }

    if (s_log_bar.dragging) {
        float span = track.height - thumb.height;
        float progress = (span > 0.0f)
                       ? (mouse.y - s_log_bar.grab - track.y) / span
                       : 0.0f;
        progress = fminf(fmaxf(progress, 0.0f), 1.0f);
        sc.scrollPosition->y = -progress * overflow;
        return;
    }

    /* --- selection ----------------------------------------------------- */
    bool inside = CheckCollisionPointRec(mouse, (Rectangle){
        box.x, box.y, box.width, box.height });

    /* Rows are laid out from the padded top, shifted by the scroll. */
    float local = mouse.y - box.y - LOG_PAD - sc.scrollPosition->y;
    int line = log_line_at(app, (local > 0.0f) ? local : 0.0f);

    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (app->log_count > 0) {
            s_log_selecting = true;
            s_log_from = line;
            s_log_to = line;
        } else {
            s_log_from = s_log_to = -1;
        }
    } else if (s_log_selecting && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_log_to = line;
    }

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_log_selecting = false;
    }

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (ctrl && IsKeyPressed(KEY_A) && app->log_count > 0) {
        s_log_from = 0;
        s_log_to = app->log_count - 1;
    }
    if (ctrl && IsKeyPressed(KEY_C)) {
        ui_log_copy(app);
    }
}

/*
 * Frame rate, for judging whether the interface is keeping up with the panel.
 * Tinted against the refresh rate so a drop is visible without reading it.
 */
static void ui_draw_fps(const app_t *app)
{
    if (!app->debug) {
        return;
    }

    int fps = GetFPS();

    char text[32];
    snprintf(text, sizeof(text), "%d FPS", fps);

    const float size = (float)FONT_SIZE_BODY;
    Vector2 measured = MeasureTextEx(s_fonts[FONT_MONO], text, size, 0.0f);
    Rectangle box = { 6.0f, 6.0f, measured.x + 12.0f, measured.y + 8.0f };

    Color tint;
    if (fps >= (s_target_fps * 9) / 10) {
        tint = (Color){ 0x4e, 0xcb, 0x84, 255 };        /* holding the clock */
    } else if (fps >= s_target_fps / 2) {
        tint = (Color){ 0xe8, 0xa3, 0x4a, 255 };        /* slipping */
    } else {
        tint = (Color){ 0xe8, 0x6a, 0x5a, 255 };        /* struggling */
    }

    /* Its own backing, since it floats over whatever is underneath. */
    DrawRectangleRounded(box, 0.35f, 6, (Color){ 0x00, 0x00, 0x00, 170 });
    DrawTextEx(s_fonts[FONT_MONO], text,
               (Vector2){ box.x + 6.0f, box.y + 4.0f }, size, 0.0f, tint);
}

/* Drawn after Clay, so the bar sits over the text rather than under it. */
static void ui_log_draw_scrollbar(const app_t *app)
{
    if (!app->debug) {
        return;
    }

    Clay_ElementData panel = Clay_GetElementData(CLAY_ID("LogScroll"));
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(CLAY_ID("LogScroll"));

    if (!panel.found || !sc.found || sc.scrollPosition == NULL) {
        return;
    }

    float content_h = sc.contentDimensions.height;
    float view_h = sc.scrollContainerDimensions.height;
    float overflow = content_h - view_h;

    if (overflow <= 0.0f) {
        return;         /* everything fits, so no bar */
    }

    Rectangle track = log_bar_track(panel.boundingBox, overflow);
    Rectangle thumb = log_bar_thumb(track, overflow, content_h, view_h,
                                    sc.scrollPosition->y);

    Vector2 mouse = GetMousePosition();
    bool hot = s_log_bar.dragging || CheckCollisionPointRec(mouse, track);

    DrawRectangleRounded(track, 1.0f, 6, (Color){ 0x36, 0x36, 0x36, 255 });
    DrawRectangleRounded(thumb, 1.0f, 6,
                         hot ? (Color){ 0xc0, 0xc0, 0xc0, 255 }
                             : (Color){ 0x96, 0x96, 0x96, 255 });
}

/*
 * Scrollbar for any clip container, driven by the box Clay recorded last
 * frame. The traffic log keeps its own copy of this because its bar has to
 * share the pointer with line selection; nothing else does.
 */
static void ui_bar_interact(Clay_ElementId id, ui_bar_t *bar)
{
    Clay_ElementData panel = Clay_GetElementData(id);
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);

    if (!panel.found || !sc.found || sc.scrollPosition == NULL) {
        bar->dragging = false;
        return;
    }

    float overflow = sc.contentDimensions.height -
                     sc.scrollContainerDimensions.height;
    if (overflow <= 0.0f) {
        bar->dragging = false;
        return;
    }

    Vector2 mouse = GetMousePosition();
    Rectangle track = log_bar_track(panel.boundingBox, overflow);
    Rectangle thumb = log_bar_thumb(track, overflow, sc.contentDimensions.height,
                                    sc.scrollContainerDimensions.height,
                                    sc.scrollPosition->y);

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        bar->dragging = false;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(mouse, track)) {
        if (CheckCollisionPointRec(mouse, thumb)) {
            bar->dragging = true;
            bar->grab = mouse.y - thumb.y;
        } else {
            /* Clicking the track jumps so the thumb centres on the pointer. */
            float span = track.height - thumb.height;
            float progress = (span > 0.0f)
                           ? (mouse.y - track.y - thumb.height / 2.0f) / span
                           : 0.0f;
            sc.scrollPosition->y = -fminf(fmaxf(progress, 0.0f), 1.0f) * overflow;
        }
        return;
    }

    if (bar->dragging) {
        float span = track.height - thumb.height;
        float progress = (span > 0.0f)
                       ? (mouse.y - bar->grab - track.y) / span
                       : 0.0f;
        sc.scrollPosition->y = -fminf(fmaxf(progress, 0.0f), 1.0f) * overflow;
    }
}

static void ui_bar_draw(Clay_ElementId id, const ui_bar_t *bar)
{
    Clay_ElementData panel = Clay_GetElementData(id);
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);

    if (!panel.found || !sc.found || sc.scrollPosition == NULL) {
        return;
    }

    float overflow = sc.contentDimensions.height -
                     sc.scrollContainerDimensions.height;
    if (overflow <= 0.0f) {
        return;         /* everything fits, so no bar */
    }

    Rectangle track = log_bar_track(panel.boundingBox, overflow);
    Rectangle thumb = log_bar_thumb(track, overflow, sc.contentDimensions.height,
                                    sc.scrollContainerDimensions.height,
                                    sc.scrollPosition->y);

    bool hot = bar->dragging ||
               CheckCollisionPointRec(GetMousePosition(), track);

    DrawRectangleRounded(track, 1.0f, 6, (Color){ 0x36, 0x36, 0x36, 255 });
    DrawRectangleRounded(thumb, 1.0f, 6,
                         hot ? (Color){ 0xc0, 0xc0, 0xc0, 255 }
                             : (Color){ 0x96, 0x96, 0x96, 255 });
}

/*
 * Grows the window when the layout needs more room than it has -- which is
 * what turning the traffic console on does. Only ever grows: a window the user
 * has made larger is left alone, and the floor moves with it so dragging it
 * back down stops where the layout is still readable.
 */
static void ui_fit_window(void)
{
    int wanted = UI_MIN_HEIGHT;

    /* Never past the screen, or the title bar ends up off the top. */
    int limit = GetMonitorHeight(GetCurrentMonitor());
    if (limit > 0 && wanted > limit) {
        wanted = limit;
    }

    SetWindowMinSize(UI_WINDOW_MIN_WIDTH, wanted);

    if (GetScreenHeight() < wanted) {
        SetWindowSize(GetScreenWidth(), wanted);
    }
}

/* ----------------------------------------------------------------- run --- */

int ui_run(app_t *app)
{
    uint64_t memorySize = Clay_MinMemorySize();
    void *memory = malloc(memorySize);
    if (memory == NULL) {
        fprintf(stderr, "out of memory setting up the interface\n");
        return 1;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(memorySize, memory);

    /* Opens at the smallest size the layout is designed to hold, and can be
       dragged larger; the faders take whatever height there is. */
    int window_w = UI_WINDOW_MIN_WIDTH;
    int window_h = UI_WINDOW_MIN_HEIGHT;

    Clay_Initialize(arena,
                    (Clay_Dimensions){ (float)window_w, (float)window_h },
                    (Clay_ErrorHandler){ HandleClayErrors, NULL });

#ifndef _WIN32
    /*
     * X11 identifies a window by WM_CLASS, and that is what the panel matches
     * against StartupWMClass in IOMeeter.desktop to pair the window with its
     * launcher and icon. GLFW builds WM_CLASS from RESOURCE_NAME when that is
     * set and from the window title otherwise, so setting it here pins the
     * name even in a session that exports its own.
     */
    setenv("RESOURCE_NAME", "IOMeeter", 1);
#endif

    Clay_Raylib_Initialize(window_w, window_h,
                           "IOMeeter",
                           FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);

    /* The layout stops being readable below its design size, so make that the
       floor rather than letting the cards collapse. */
    ui_fit_window();

    /*
     * Escape must not end the program: it cancels the rename editor, and with
     * the tray running the only real quit is the tray menu.
     */
    SetExitKey(KEY_NULL);

    /* Names the phase that blocked, in a file that survives the process being
       killed. Harmless when nothing hangs: two stores per phase. */
    watchdog_start("iomeeter-hang.log");

    /*
     * Run at the panel's refresh rate, with vsync left on. Presenting on the
     * refresh boundary is what makes motion even; a target above it can only
     * tear, and one below it drops frames.
     */
    s_target_fps = GetMonitorRefreshRate(GetCurrentMonitor());
    if (s_target_fps <= 0) {
        s_target_fps = UI_FALLBACK_FPS;     /* driver did not report one */
    }
    SetTargetFPS(s_target_fps);

    /*
     * Window icon. raylib has no .ico loader, so the PNG beside it is what gets
     * used here; the .ico is still the better source on Windows and tray_init()
     * overrides this with it below.
     *
     * On Linux this is the icon the panel shows for the window, and the only
     * one set at all, since the tray layer is a stub there.
     */
    char icon_path[512];

    if (respath_find("icon.png", icon_path, sizeof(icon_path))) {
        Image icon = LoadImage(icon_path);
        if (icon.data) {
            ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            SetWindowIcon(icon);
            UnloadImage(icon);
        }
    } else {
        fprintf(stderr, "could not find icon.png\n");
    }

    /* On Windows this also owns the tray, and replaces the icon above with the
       multi-resolution .ico through the shell. */
    char tray_icon[512];
#ifdef _WIN32
    const char *tray_icon_file = "icon.ico";
#else
    const char *tray_icon_file = "icon.png";    /* AppIndicator cannot read .ico */
#endif
    if (!respath_find(tray_icon_file, tray_icon, sizeof(tray_icon))) {
        tray_icon[0] = 0;
    }
    tray_init(GetWindowHandle(), tray_icon, "IOMeeter");

    char font_path[512];

    if (respath_find("Roboto-Regular.ttf", font_path, sizeof(font_path))) {
        s_fonts[FONT_BODY] = LoadFontEx(font_path, FONT_SIZE_ATLAS, NULL, 0);
    }
    if (respath_find("RobotoMono-Medium.ttf", font_path, sizeof(font_path))) {
        s_fonts[FONT_MONO] = LoadFontEx(font_path, FONT_SIZE_ATLAS, NULL, 0);
    }
    for (int i = 0; i < FONT_COUNT; i++) {
        if (!s_fonts[i].glyphs) {
            s_fonts[i] = GetFontDefault();
        } else {
            SetTextureFilter(s_fonts[i].texture, TEXTURE_FILTER_BILINEAR);
        }
    }

    Clay_SetMeasureTextFunction(Raylib_MeasureText, s_fonts);
    Clay_Raylib_SetFonts(s_fonts, FONT_COUNT);

    Clay_ElementId wheel_id  = CLAY_ID("ColourWheel");
    Clay_ElementId slider_id = CLAY_ID("Brightness");

    double last_live_send = 0.0;

    /*
     * While the device is driving a slider, the pointer is locked out so the
     * two do not fight over the same value.
     */
    unsigned long slider_extern_seen = 0;
    double slider_locked_until = 0.0;

    /* Both variants are looked for on each sweep; see USB_VARIANTS. */
    double last_connect_scan = GetTime();

    /* Per fader, so only the one the device is actually moving recolours. */
    unsigned long slider_seen_at[APP_FADER_COUNT] = { 0 };
    double slider_device_until[APP_FADER_COUNT] = { 0.0 };

    /* Loading a configuration can flip "debug", which adds or removes the
       traffic console and so changes the height the layout needs. */

    /* Nothing is written on a timer any more, so the file is only touched
       when the window is put away or the menu asks for it. */
    bool minimized_seen = tray_is_minimized();

    for (;;) {
        /*
         * On Windows the close button never gets this far: tray.c swallows
         * WM_CLOSE in the window procedure. Elsewhere it arrives here, and
         * WindowShouldClose() reports it exactly once because raylib clears
         * GLFW's flag as it reads it, so ignoring it simply keeps the loop
         * running. With a tray present that means hiding instead of quitting.
         */
        watchdog_phase("WindowShouldClose");

        if (WindowShouldClose()) {
            if (!tray_available()) {
                break;
            }
            tray_minimize();
        }

        watchdog_phase("tray_poll");
        if (tray_poll()) {      /* "Close IOMeeter" from the tray menu */
            break;
        }

        /* Plug the device in at any time and it is picked up on the next
           sweep, without the Connect button being touched. */
        if (!app->connected && GetTime() - last_connect_scan >= UI_CONNECT_SCAN_SECONDS) {
            last_connect_scan = GetTime();
            watchdog_phase("app_connect (usb scan)");
            app_connect(app);
        }

        /* Somebody launched IOMeeter again: surface instead of ignoring it. */
        watchdog_phase("instance_show_requested");
        if (instance_show_requested()) {
            tray_restore();
            instance_raise(GetWindowHandle());
        }

        if (tray_available() && !tray_is_minimized() && IsWindowMinimized()) {
            tray_minimize();
        }

        /*
         * Going to the tray is what commits the configuration, that being the
         * point the user is done with the window. Watched as a transition
         * rather than hooked onto the calls that hide it: on Windows the close
         * button is swallowed in tray.c's window procedure and minimises from
         * there, so it never passes through this loop at all.
         */
        bool minimized_now = tray_is_minimized();

        if (minimized_now && !minimized_seen) {
            watchdog_phase("app_config_save");
            app_config_save(app);
        }
        minimized_seen = minimized_now;

        /* Hidden: no window to draw into, so idle instead of spinning. */
        if (tray_is_minimized()) {
            app_poll(app);
            if (app->notice_seq != s_toast_seen) {
                s_toast_seen = app->notice_seq;
                tray_notify("Interrupt", app->notice);
            }
            /* Nothing is drawn while hidden, and EndDrawing() is what normally
               pumps input, so drain the queue explicitly. */
            PollInputEvents();
            WaitTime(0.05);
            continue;
        }

        app_poll(app);

        if (app->notice_seq != s_toast_seen) {
            s_toast_seen = app->notice_seq;
            s_toast_until = GetTime() + TOAST_SECONDS;
            /* Hidden in the tray there is no window to draw the toast on, so
               hand it to the shell instead. */
            tray_notify("Interrupt", app->notice);
        }

        ui_foreground_poll(app);
        ui_key_glow_poll(app);
        ui_pump_text_input();
        ui_update_rename(app);
        ui_key_record_poll(app);
        ui_hex_field_apply(app);
        ui_led_hex_apply(app);

        /* One texture, and never two wheels on screen at once: whichever is up
           is the one it is drawn for. */
        ui_rebuild_wheel(s_key_wheel ? s_key_wheel_v : app->val);

        /* ui_pump_text_input() drops focus on escape; that is also how the
           profile editor is abandoned, so it follows the focus. */
        if (s_profile_rename >= 0 && s_focus != s_profile_buf) {
            s_profile_rename = -1;
        }

        Clay_SetLayoutDimensions((Clay_Dimensions){
            (float)GetScreenWidth(), (float)GetScreenHeight() });

        Vector2 mouse = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mouse.x, mouse.y },
                             IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(true,
                                    (Clay_Vector2){ 0, GetMouseWheelMove() * 32 },
                                    GetFrameTime());

        /*
         * The menu is resolved first and, while open, swallows the pointer.
         * The wheel, sliders and faders all hit-test raw mouse coordinates
         * without knowing what is drawn above them, so without this a click on
         * a menu entry also dragged whatever sat underneath it.
         */
        Rectangle menu_panel = { 0 };
        bool menu_blocks = false;

        if (s_menu_open) {
            Clay_ElementData button = Clay_GetElementData(CLAY_ID("MenuButton"));
            if (button.found) {
                menu_panel = ui_menu_rect(button.boundingBox);
                Vector2 mouse = GetMousePosition();
                Clay_BoundingBox b = button.boundingBox;

                bool over_button = mouse.x >= b.x && mouse.x <= b.x + b.width &&
                                   mouse.y >= b.y && mouse.y <= b.y + b.height;
                menu_blocks = CheckCollisionPointRec(mouse, menu_panel) || over_button;

                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int item = ui_menu_hit(menu_panel);
                    if (item == 0) {
                        s_menu_open = false;
                        ui_menu_save_config(app);
                    } else if (item == 1) {
                        s_menu_open = false;
                        ui_menu_load_config(app);
                    } else if (item == 2) {
                        s_menu_open = false;
                        ui_menu_reload_config(app);
                    } else if (!over_button) {
                        s_menu_open = false;   /* clicked away */
                    }
                }
            } else {
                s_menu_open = false;   /* button not laid out yet */
            }
        }

        /* Interaction against the previous frame's boxes, before laying out
           the next one. */
        watchdog_phase("ui_log_interact");
        ui_log_measure();
        ui_log_interact(app);

        if (s_key_editor >= 0 && !s_key_picker) {
            ui_bar_interact(CLAY_ID("KeySteps"), &s_step_bar);
        }
        if (s_key_picker) {
            ui_bar_interact(CLAY_ID("KeyNameRows"), &s_step_bar);

            /* Narrowing the list makes it a different list, and leaving it
               scrolled where the old one was shows an empty box. */
            static char filter_seen[sizeof(s_key_filter)];

            if (strcmp(s_key_filter, filter_seen) != 0) {
                snprintf(filter_seen, sizeof(filter_seen), "%s", s_key_filter);
                ui_scroll_to_start(CLAY_ID("KeyNameRows"));
            }
        }

        float fader_h = (float)ui_fader_height(app->debug);

        /*
         * Clay stops the pointer at a floating element on its own, but the
         * fader strip hit-tests raw mouse coordinates without consulting the
         * layout, so the editor has to be kept off it by hand.
         */
        bool editor_blocks = false;

        if (s_key_editor >= 0 || s_picker >= 0) {
            Clay_ElementId id = (s_key_editor >= 0) ? CLAY_ID("KeyEditor")
                                                    : CLAY_ID("AppPicker");
            /* The name list sits over the editor, so it is what blocks. */
            if (s_key_picker) {
                id = CLAY_ID("KeyNames");
            }
            Clay_ElementData panel = Clay_GetElementData(id);
            Clay_BoundingBox b = panel.boundingBox;

            editor_blocks = panel.found &&
                            CheckCollisionPointRec(GetMousePosition(),
                                (Rectangle){ b.x, b.y, b.width, b.height });
        }

        /* The key's colour wheel floats over the editor and reaches past it,
           so it blocks the faders underneath in its own right. */
        if (s_key_wheel) {
            Clay_ElementData panel = Clay_GetElementData(CLAY_ID("KeyColour"));
            Clay_BoundingBox b = panel.boundingBox;

            editor_blocks = editor_blocks ||
                            (panel.found &&
                             CheckCollisionPointRec(GetMousePosition(),
                                (Rectangle){ b.x, b.y, b.width, b.height }));
        }

        bool colour_changed = false;
        Clay_ElementData wheel_data = Clay_GetElementData(wheel_id);
        if (s_tab == TAB_CONFIG && wheel_data.found && !menu_blocks &&
            !editor_blocks) {
            ui_wheel_interact(&app->hue, &app->sat, wheel_data.boundingBox,
                              &colour_changed);
            if (colour_changed) {
                app_sync_hex(app);
            }
        }

        /* The key's own wheel, which is only ever up on the other tab, so the
           two never compete for the pointer. */
        if (s_key_wheel && s_key_editor >= 0) {
            Clay_ElementData key_wheel = Clay_GetElementData(CLAY_ID("KeyWheel"));
            bool key_colour_changed = false;

            if (key_wheel.found && !menu_blocks) {
                ui_wheel_interact(&s_key_wheel_h, &s_key_wheel_s,
                                  key_wheel.boundingBox, &key_colour_changed);
            }
            if (!menu_blocks && ui_slider(CLAY_ID("KeyBright"), &s_key_wheel_v)) {
                key_colour_changed = true;
            }

            /* Into the hex field, which is what the editor commits from. */
            if (key_colour_changed) {
                uint8_t r, g, b;
                app_hsv_to_rgb(s_key_wheel_h, s_key_wheel_s, s_key_wheel_v,
                               &r, &g, &b);
                keys_format_hex(((uint32_t)r << 16) | ((uint32_t)g << 8) | b,
                                s_key_hex, sizeof(s_key_hex));
            }
        }
        if (app->slider_extern_seq != slider_extern_seen) {
            slider_extern_seen = app->slider_extern_seq;
            slider_locked_until = GetTime() + 0.5;
        }

        /* Held briefly past the last packet so the colour does not flicker
           between updates while the hardware fader is being moved. */
        for (int i = 0; i < APP_FADER_COUNT; i++) {
            if (app->slider_extern_at[i] != slider_seen_at[i]) {
                slider_seen_at[i] = app->slider_extern_at[i];
                slider_device_until[i] = GetTime() + 0.5;
            }
        }
        bool sliders_locked = GetTime() < slider_locked_until;

        bool faders_live = (s_tab == TAB_MAIN) && !menu_blocks &&
                           !sliders_locked && !editor_blocks;

        for (int i = 0; i < APP_FADER_COUNT && faders_live; i++) {
            Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Fader", i));
            if (!slot.found) {
                continue;
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                Vector2 m = GetMousePosition();
                Clay_BoundingBox b = slot.boundingBox;
                if (m.x >= b.x && m.x <= b.x + b.width &&
                    m.y >= b.y && m.y <= b.y + b.height) {
                    s_rename = i;
                    snprintf(s_rename_buf, sizeof(s_rename_buf), "%s",
                             app->sliders[i].name);
                    s_focus = s_rename_buf;         /* route typing here */
                    s_focus_cap = sizeof(s_rename_buf);
                }
            }

            fader_interact(i, slot.boundingBox, &app->sliders[i].value,
                           APP_FADER_MAX, ui_on_fader_change, app);
        }

        if (s_tab == TAB_CONFIG && !menu_blocks && !editor_blocks &&
            ui_slider(slider_id, &app->val)) {
            colour_changed = true;
            app_sync_hex(app);
        }

        /* Not part of the colour: it scales every state the device shows,
           and is sent with them rather than as it moves. */
        if (s_tab == TAB_CONFIG && !menu_blocks && !editor_blocks) {
            ui_slider(CLAY_ID("LedBrightness"), &app->led_brightness);
        }

        /* A state slot that has been picked is where the wheel writes; with
           none picked it drives the live colour, as it always did. */
        if (colour_changed && s_led_target >= 0) {
            snprintf(app->led_hex[s_led_target], APP_HEX_MAX, "%s", app->hex);
        }

        if (colour_changed && app->live_send && app->connected) {
            /* Throttle so a drag does not flood the endpoint. */
            if (GetTime() - last_live_send > 0.06) {
                last_live_send = GetTime();
                app_set_led(app);
            }
        }

        s_text_slot = 0;    /* hand out fresh text storage each frame */
        Clay_BeginLayout();

        CLAY(CLAY_ID("Root"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .padding = { 15, 15, 20, 20 },      /* left, right, top, bottom */
                .childGap = 12,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
            },
            .backgroundColor = C_BG,
        }) {
            /* ---- header ---- */
            CLAY(CLAY_ID("Header"), {
                .layout = {
                    /* Spans the window: the badge and buttons belong at the
                       edges, not gathered in the middle. */
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 10,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                },
            }) {
                /* Hamburger. Three bars rather than a glyph, so it does not
                   depend on the loaded font having one. */
                CLAY(CLAY_ID("MenuButton"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(34), CLAY_SIZING_FIXED(30) },
                        .padding = CLAY_PADDING_ALL(8),
                        .childGap = 4,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                    },
                    .backgroundColor = (s_menu_open || Clay_Hovered())
                                     ? C_LINE : C_CARD,
                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                    .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
                }) {
                    if (clicked(true)) {
                        s_menu_open = !s_menu_open;
                    }
                    for (int bar = 0; bar < 3; bar++) {
                        CLAY(CLAY_IDI("MenuBar", bar), {
                            .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                    CLAY_SIZING_FIXED(2) } },
                            .backgroundColor = C_FG,
                            .cornerRadius = CLAY_CORNER_RADIUS(1),
                        }) {}
                    }
                }

                CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                           .childGap = 2 } }) {
                    CLAY_TEXT(CLAY_STRING("IOMeeter"),
                              CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FONT_SIZE_TITLE,
                                                 .textColor = C_FG }));
                }

                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

                CLAY_AUTO_ID({
                    .layout = { .padding = { 10, 10, 6, 6 } },
                    .cornerRadius = CLAY_CORNER_RADIUS(999),
                    .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
                }) {
                    CLAY_TEXT(dyn(app->connected ? app->device_name : "searching..."),
                              CLAY_TEXT_CONFIG({
                                  .fontId = FONT_MONO, .fontSize = FONT_SIZE_SMALL,
                                  /* Green only when there is something on the
                                     far end: a dongle with no controller
                                     behind it is connected and useless. */
                                  .textColor = (app->connected &&
                                                app->controller_linked)
                                               ? C_OK : C_MUTED }));
                }

                /* Only meaningful once something is attached to report it. */
                if (app->connected) {
                    ui_battery(app);
                }

                if (app->connected) {
                    if (ui_button(CLAY_ID("Disconnect"), "Disconnect", false, true, false)) {
                        app_disconnect(app);
                    }
                } else {
                    if (ui_button(CLAY_ID("Connect"), "Connect", true, true, false)) {
                        /* An explicit press deserves an answer even if the
                           timed sweep has already reported the same failure. */
                        app->connect_failure_logged = false;
                        app_connect(app);
                    }
                }
            }

            ui_tab_bar();

            /* ---- configuration tab ---- */
            if (s_tab == TAB_CONFIG) {
            CLAY(CLAY_ID("ConfigTab"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 12,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
                },
            }) {
                /* Side by side: stacked, the slots push the profiles card off
                   the bottom of the window. */
                CLAY(CLAY_ID("LedCards"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .childGap = 12,
                    },
                }) {
                UI_CARD(CLAY_ID("LedCard")) {
                    ui_card_title("NEOPIXEL");

                    CLAY_AUTO_ID({ .layout = { .childGap = 12 } }) {
                        CLAY(wheel_id, {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(150),
                                                    CLAY_SIZING_FIXED(150) } },
                            .image = { .imageData = &s_wheel },
                            .backgroundColor = C_TRANSPARENT,
                        }) {}

                        CLAY_AUTO_ID({
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                .childGap = 8,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                            },
                        }) {
                            CLAY_TEXT(CLAY_STRING("Brightness"), CLAY_TEXT_CONFIG({
                                .fontId = FONT_BODY, .fontSize = FONT_SIZE_SMALL,
                                .textColor = C_MUTED }));

                            /* Declared here; the value was read further up. */
                            CLAY(slider_id, {
                                .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                        CLAY_SIZING_FIXED(18) },
                                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                            }) {
                                CLAY_AUTO_ID({
                                    .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                            CLAY_SIZING_FIXED(5) } },
                                    .backgroundColor = C_LINE,
                                    .cornerRadius = CLAY_CORNER_RADIUS(3),
                                }) {}
                            }

                            CLAY_AUTO_ID({ .layout = { .childGap = 8,
                                                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                                uint32_t rgb = app_rgb(app);
                                CLAY_AUTO_ID({
                                    .layout = { .sizing = { CLAY_SIZING_FIXED(34),
                                                            CLAY_SIZING_FIXED(30) } },
                                    .backgroundColor = COL((rgb >> 16) & 0xFF,
                                                           (rgb >> 8) & 0xFF,
                                                           rgb & 0xFF, 255),
                                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                                    .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
                                }) {}

                                ui_text_field(CLAY_ID("HexField"), app->hex,
                                              APP_HEX_MAX, FONT_MONO, NULL);
                            }

                            ui_checkbox(CLAY_ID("Live"), "Send while dragging",
                                        &app->live_send);

                            CLAY_AUTO_ID({ .layout = { .childGap = 8 } }) {
                                if (ui_button(CLAY_ID("SetLed"), "Set LED", true,
                                              app->connected, true)) {
                                    app_set_led(app);
                                }
                                if (ui_button(CLAY_ID("GetLed"), "Get", false,
                                              app->connected, false)) {
                                    app_get(app, "led");
                                }
                            }
                        }
                    }
                }

                ui_led_states_card(app);
                }   /* LedCards */

                ui_profiles_card(app);
            }
            }   /* if (s_tab == TAB_CONFIG) */

            /* ---- main tab ---- */
            if (s_tab == TAB_MAIN) {
            CLAY(CLAY_ID("MainTab"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 12,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
                },
            }) {
            /* The strip and the pad sit side by side; anything added to the
               main tab later stacks underneath this row. */
            CLAY(CLAY_ID("MainRow"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .childGap = 12,
                },
            }) {
                /* Faders. Clay only reserves the slots; fader_draw paints
                   the gradient track, knob and rotated label afterwards. */
                UI_CARD(CLAY_ID("FaderCard")) {
                    ui_card_title("FADERS");

                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .childGap = 6,
                        },
                    }) {
                        for (int i = 0; i < APP_FADER_COUNT; i++) {
                            CLAY(CLAY_IDI("FaderCol", i), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_FIXED(FADER_COLUMN_W),
                                                CLAY_SIZING_FIT(0) },
                                    .childGap = 6,
                                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
                                },
                            }) {
                                CLAY(CLAY_IDI("Fader", i), {
                                    .layout = { .sizing = {
                                        CLAY_SIZING_FIXED(FADER_SLOT_WIDTH),
                                        CLAY_SIZING_FIXED(fader_h) } },
                                }) {}

                                if (ui_button(CLAY_IDI("AddApp", i), "+",
                                              false, true, true)) {
                                    ui_add_app(app, i);
                                }

                                CLAY(CLAY_IDI("AppList", i), {
                                    .layout = {
                                        .sizing = { CLAY_SIZING_GROW(0),
                                                    CLAY_SIZING_FIXED(APP_LIST_H) },
                                        .padding = { 4, 4, 2, 2 },
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                    },
                                    .backgroundColor = C_FIELD,
                                    .cornerRadius = CLAY_CORNER_RADIUS(6),
                                    /* Scrolls once the list outgrows the fixed
                                       height, i.e. past five entries. */
                                    .clip = { .vertical = true,
                                              .childOffset = Clay_GetScrollOffset() },
                                }) {
                                    for (int a = 0; a < app->sliders[i].app_count; a++) {
                                        int slot = i * CONFIG_APPS_MAX + a;

                                        CLAY(CLAY_IDI("AppRow", slot), {
                                            .layout = {
                                                .sizing = { CLAY_SIZING_GROW(0),
                                                            CLAY_SIZING_FIXED(APP_ROW_H) },
                                                .childGap = 2,
                                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                                            },
                                        }) {
                                            CLAY_AUTO_ID({
                                                .layout = { .sizing = { CLAY_SIZING_GROW(0) } },
                                            }) {
                                                CLAY_TEXT(dyn(app->sliders[i].apps[a].name),
                                                    CLAY_TEXT_CONFIG({
                                                        .fontId = FONT_BODY,
                                                        .fontSize = FONT_SIZE_APP_LIST,
                                                        .textColor = C_MUTED }));
                                            }

                                            CLAY(CLAY_IDI("AppDel", slot), {
                                                .layout = {
                                                    .sizing = { CLAY_SIZING_FIXED(13),
                                                                CLAY_SIZING_FIXED(13) },
                                                    .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                                        CLAY_ALIGN_Y_CENTER },
                                                },
                                                .backgroundColor = Clay_Hovered()
                                                                 ? C_WARN : C_LINE,
                                                .cornerRadius = CLAY_CORNER_RADIUS(3),
                                            }) {
                                                if (clicked(true)) {
                                                    s_remove_slider = i;
                                                    s_remove_index = a;
                                                }
                                                /* "x" rather than a multiplication
                                                   sign: the fonts are loaded with
                                                   the default ASCII range only. */
                                                CLAY_TEXT(CLAY_STRING("x"),
                                                    CLAY_TEXT_CONFIG({
                                                        .fontId = FONT_BODY,
                                                        .fontSize = FONT_SIZE_SMALL,
                                                        .textColor = C_FG }));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                ui_keypad_card(app);
            }
            }
            }   /* if (s_tab == TAB_MAIN) */

            /* ---- traffic ----
               Declared only when "debug" is set in config.json; leaving it out
               of the layout hands its height back to the cards above. */
            if (app->debug) {
            /*
             * Takes what the cards above have left instead of adding to the
             * height they need. Sizing the window is the user's business, and
             * a panel that scrolls has no reason to ask for more of it.
             */
            CLAY(CLAY_ID("LogCard"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(LOG_MIN_H) },
                    .padding = CLAY_PADDING_ALL(14),
                    .childGap = 8,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
                .backgroundColor = C_CARD,
                .cornerRadius = CLAY_CORNER_RADIUS(12),
                .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
            }) {
                CLAY_AUTO_ID({ .layout = { .childGap = 12,
                                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                    CLAY_TEXT(CLAY_STRING("TRAFFIC"), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FONT_SIZE_CAPTION, .textColor = C_MUTED }));

                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

                    if (ui_button(CLAY_ID("ClearLog"), "Clear", false, true, false)) {
                        app_log_clear(app);
                    }
                }

                CLAY(CLAY_ID("LogScroll"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                .padding = CLAY_PADDING_ALL(8),
                                /* log_line_at() counts on this. */
                                .childGap = LOG_GAP,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM },
                    .backgroundColor = C_FIELD,
                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                    .clip = { .vertical = true,
                              .childOffset = Clay_GetScrollOffset() },
                }) {
                    for (int i = 0; i < app->log_count; i++) {
                        const app_log_entry_t *entry = app_log_at(app, i);
                        int rows = log_rows(entry->text, s_log_cols);
                        int at = 0;

                        /* One block per entry, however many rows it wraps
                           onto, so the highlight covers all of it and the
                           selection still counts in entries. */
                        CLAY_AUTO_ID({
                            .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                    CLAY_SIZING_FIT(0) },
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM },
                            .backgroundColor = log_line_selected(i)
                                             ? C_SELECT : C_TRANSPARENT,
                        }) {
                            int len = (int)strlen(entry->text);

                            for (int r = 0; r < rows; r++) {
                                int take = log_segment(entry->text, len, at,
                                                       s_log_cols);

                                CLAY_AUTO_ID({
                                    .layout = { .sizing = {
                                        CLAY_SIZING_GROW(0),
                                        CLAY_SIZING_FIXED(LOG_ROW_H) } },
                                }) {
                                    /* The row is cut to fit, so Clay is told
                                       not to wrap it again: it breaks on
                                       spaces, and the second line would fall
                                       outside a row of fixed height. */
                                    CLAY_TEXT(slice(entry->text + at, take),
                                              CLAY_TEXT_CONFIG({
                                        .fontId = FONT_MONO,
                                        .fontSize = FONT_SIZE_SMALL,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE,
                                        .textColor = log_colour(entry->kind) }));
                                }
                                at += take;
                            }
                        }
                    }
                }
            }
            }   /* if (app->debug) */

            /* Declared last and floating, so they lay out over everything
               above rather than pushing the cards around. Only one of the two
               can be open: they live on different tabs. */
            ui_key_editor(app);
            ui_key_colour_picker();
            ui_key_name_picker(app);
            ui_app_picker(app);
        }

        if (s_remove_slider >= 0) {
            config_slider_t *slider = &app->sliders[s_remove_slider];
            if (s_remove_index < slider->app_count) {
                app_log(app, APP_LOG_EVENT, "%s no longer controls %s",
                        slider->name, slider->apps[s_remove_index].name);
                config_remove_app(slider, s_remove_index);
                app->config_dirty = true;
            }
            s_remove_slider = -1;
            s_remove_index = -1;
        }

        if (s_profile_remove >= 0) {
            /* Read before the removal shifts the names down. */
            char gone[KEYS_PROFILE_NAME_MAX];
            snprintf(gone, sizeof(gone), "%s",
                     app->keys.profiles[s_profile_remove].name);

            if (keys_remove_profile(&app->keys, s_profile_remove)) {
                app->config_dirty = true;
                app_log(app, APP_LOG_EVENT, "removed profile %s", gone);

                /* Both of these are addressed by index, and every index above
                   the one that went has just moved. */
                s_picker = -1;
                s_profile_rename = -1;
                s_key_editor = -1;
                s_key_record = false;
            }
            s_profile_remove = -1;
        }

        if (s_profile_app_remove >= 0) {
            keys_remove_app(&app->keys, s_profile_app_owner,
                            s_profile_app_remove);
            app->config_dirty = true;
            s_profile_app_remove = -1;
            s_profile_app_owner = -1;
        }

        if (s_key_step_remove >= 0) {
            keys_binding_t *binding = keys_binding(&app->keys, s_key_editor,
                                                   app->keys.profile);
            if (binding != NULL) {
                keys_macro_remove(&binding->macro, s_key_step_remove);

                /* The text buffers are positional, so they close the same gap
                   the macro just did. */
                for (int i = s_key_step_remove; i + 1 < KEYS_MACRO_MAX; i++) {
                    memcpy(s_key_timing[i], s_key_timing[i + 1],
                           sizeof(s_key_timing[i]));
                }
            }
            s_key_step_remove = -1;
        }

        Clay_RenderCommandArray commands = Clay_EndLayout();

        /* Both need this frame's content height, so they run after the
           layout and before anything is drawn. */
        if (app->debug) {
            ui_autoscroll_log(app, CLAY_ID("LogScroll"));
        }
        ui_autoscroll_steps(app);

        /*
         * Clay emits one layout root at a time, sorted by z index, so a
         * floating element's commands are the tail of the array. Everything
         * between the two halves below is painted with raylib rather than
         * through Clay, and drawing all of it after the whole array is what
         * put the fader strip over the macro editor.
         *
         * The boundary is the first command carrying a z index, not the last:
         * Clay leaves the field at zero on borders and on the scissor commands
         * that close a clip, so scanning back from the end stops on the
         * panel's own trailing scissor and finds nothing. Scanning forward is
         * sound because the panel is opaque, and a background is emitted as a
         * rectangle, which does carry the index.
         */
        int32_t floating_at = 0;

        while (floating_at < commands.length &&
               Clay_RenderCommandArray_Get(&commands, floating_at)->zIndex <= 0) {
            floating_at++;
        }

        Clay_RenderCommandArray base = commands;
        base.length = floating_at;

        Clay_RenderCommandArray floating = commands;
        floating.internalArray = commands.internalArray + floating_at;
        floating.length = commands.length - floating_at;

        watchdog_phase("render");
        BeginDrawing();
        ClearBackground((Color){ 0x0f, 0x0f, 0x0f, 255 });
        Clay_Raylib_Render(base);

        /*
         * These paint themselves rather than going through Clay, so they have
         * to be told which tab is up. Clay_GetElementData() keeps answering for
         * an element the previous frame laid out, so "found" alone left the
         * faders drawn over the configuration tab and the wheel marker and
         * brightness knob over the main one.
         */
        if (s_tab == TAB_CONFIG) {
            Clay_ElementData wheel_now = Clay_GetElementData(wheel_id);
            if (wheel_now.found) {
                ui_draw_wheel_marker(app->hue, app->sat, app->val,
                                     wheel_now.boundingBox);
            }
            ui_draw_slider_knob(slider_id, app->val);
            ui_draw_slider_knob(CLAY_ID("LedBrightness"), app->led_brightness);
        }

        if (s_key_wheel && s_key_editor >= 0) {
            Clay_ElementData key_wheel = Clay_GetElementData(CLAY_ID("KeyWheel"));
            if (key_wheel.found) {
                ui_draw_wheel_marker(s_key_wheel_h, s_key_wheel_s,
                                     s_key_wheel_v, key_wheel.boundingBox);
            }
            ui_draw_slider_knob(CLAY_ID("KeyBright"), s_key_wheel_v);
        }

        /* Only on the tab the pad is on: Clay keeps answering for elements
           the previous frame laid out, so "found" alone is not enough. */
        if (s_tab == TAB_MAIN) {
            ui_draw_key_glow(app);
        }

        for (int i = 0; i < APP_FADER_COUNT && s_tab == TAB_MAIN; i++) {
            Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Fader", i));
            if (slot.found) {
                fader_draw(i, slot.boundingBox, app->sliders[i].value,
                           APP_FADER_MAX, app->sliders[i].name,
                           &s_fonts[FONT_BODY],
                           GetTime() < slider_device_until[i]);
            }
        }

        ui_log_draw_scrollbar(app);

        if (s_rename >= 0 && s_tab == TAB_MAIN) {
            Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Fader", s_rename));
            if (slot.found) {
                ui_draw_rename(app, slot.boundingBox);
            }
        }

        ui_draw_toast(app);

        /* Last of the raylib pass, so it covers the wheel marker, knob and
           faders. The floating half of the layout still goes over it. */
        if (s_menu_open && menu_panel.width > 0.0f) {
            ui_draw_menu(menu_panel);
        }

        /* The macro editor and anything else floating, above the lot. */
        Clay_Raylib_Render(floating);

        /* Their scrollbars go on top of that again, being drawn by raylib
           rather than emitted as Clay commands. */
        if (s_key_editor >= 0) {
            ui_bar_draw(CLAY_ID("KeySteps"), &s_step_bar);
        }
        if (s_key_picker) {
            ui_bar_draw(CLAY_ID("KeyNameRows"), &s_step_bar);
        }
        if (s_picker >= 0) {
            ui_bar_draw(CLAY_ID("PickerRows"), &s_step_bar);
        }

        ui_draw_fps(app);

        watchdog_phase("EndDrawing (present)");
        EndDrawing();
    }

    if (s_wheel.id != 0) {
        UnloadTexture(s_wheel);
    }
    watchdog_stop();
    tray_shutdown();
    Clay_Raylib_Close();
    free(memory);
    return 0;
}
