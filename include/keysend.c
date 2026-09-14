#include "keysend.h"

#include "foreground.h"
#include "media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#if !defined(_WIN32) && defined(KEYSEND_HAVE_XTEST)
#include <X11/XKBlib.h>     /* the shift level a character sits at */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#endif

/*
 * One key, under the three names it goes by.
 *
 * The GLFW code is what raylib reports when the user presses the key while
 * recording. The other two are what the platform wants back: a virtual-key
 * code on Windows, an X keysym elsewhere. Both are written as literals rather
 * than pulled from headers, so this table reads the same on either system and
 * needs no X11 header just to be compiled on Windows.
 */
typedef struct {
    const char *name;
    int         glfw;       /*!< 0 when the key cannot be typed to record it */
    int         win;        /*!< virtual-key code                            */
    bool        extended;   /*!< needs KEYEVENTF_EXTENDEDKEY                 */
    unsigned    x11;        /*!< keysym                                      */
} entry_t;

/*
 * The letter and digit rows line up across all three: 'A' is GLFW 65 and
 * VK 0x41, '0' is GLFW 48 and VK 0x30. Only the X keysym differs, and only
 * for letters, which are named in lower case so no shift is implied.
 */
#define LETTER(ch, text) { text, ch, ch, false, (unsigned)(ch) + 32 }
#define DIGIT(ch, text)  { text, ch, ch, false, (unsigned)(ch) }
#define FKEY(n)          { "F" #n, 290 + (n) - 1, 0x70 + (n) - 1, false, \
                           0xFFBEu + (n) - 1 }

static const entry_t s_keys[] = {
    LETTER('A', "A"), LETTER('B', "B"), LETTER('C', "C"), LETTER('D', "D"),
    LETTER('E', "E"), LETTER('F', "F"), LETTER('G', "G"), LETTER('H', "H"),
    LETTER('I', "I"), LETTER('J', "J"), LETTER('K', "K"), LETTER('L', "L"),
    LETTER('M', "M"), LETTER('N', "N"), LETTER('O', "O"), LETTER('P', "P"),
    LETTER('Q', "Q"), LETTER('R', "R"), LETTER('S', "S"), LETTER('T', "T"),
    LETTER('U', "U"), LETTER('V', "V"), LETTER('W', "W"), LETTER('X', "X"),
    LETTER('Y', "Y"), LETTER('Z', "Z"),

    DIGIT('0', "0"), DIGIT('1', "1"), DIGIT('2', "2"), DIGIT('3', "3"),
    DIGIT('4', "4"), DIGIT('5', "5"), DIGIT('6', "6"), DIGIT('7', "7"),
    DIGIT('8', "8"), DIGIT('9', "9"),

    FKEY(1),  FKEY(2),  FKEY(3),  FKEY(4),  FKEY(5),  FKEY(6),
    FKEY(7),  FKEY(8),  FKEY(9),  FKEY(10), FKEY(11), FKEY(12),

    /*
     * F13 upwards exist on no ordinary keyboard, which is what makes them
     * worth having here: nothing else claims them, so a pad key bound to one
     * is a shortcut no program will ever take for a real keystroke. The
     * arithmetic continues unbroken on all three -- VK 0x7C, keysym 0xFFCA
     * and GLFW 302 are F13 -- so the same macro serves.
     */
    FKEY(13), FKEY(14), FKEY(15), FKEY(16), FKEY(17), FKEY(18),
    FKEY(19), FKEY(20), FKEY(21), FKEY(22), FKEY(23), FKEY(24),

    /* Modifiers. The right-hand control and alt, and both super keys, are
       extended scan codes on Windows. */
    { "SHIFT_LEFT",    340, 0xA0, false, 0xFFE1u },
    { "SHIFT_RIGHT",   344, 0xA1, false, 0xFFE2u },
    { "CTRL_LEFT",     341, 0xA2, false, 0xFFE3u },
    { "CTRL_RIGHT",    345, 0xA3, true,  0xFFE4u },
    { "ALT_LEFT",      342, 0xA4, false, 0xFFE9u },
    { "ALT_RIGHT",     346, 0xA5, true,  0xFFEAu },
    { "SUPER_LEFT",    343, 0x5B, true,  0xFFEBu },
    { "SUPER_RIGHT",   347, 0x5C, true,  0xFFECu },
    { "MENU",          348, 0x5D, true,  0xFF67u },

    /*
     * The same four again under the names people actually use. A keyboard
     * sends the left-hand one when you press "the" control key, so that is
     * what these are.
     *
     * No GLFW code: recording a physical press must keep naming the side it
     * came from, and keysend_name_of() answers with whichever entry it meets
     * first. Leaving these out of that search keeps it saying CTRL_LEFT.
     */
    { "CTRL",            0, 0xA2, false, 0xFFE3u },
    { "SHIFT",           0, 0xA0, false, 0xFFE1u },
    { "ALT",             0, 0xA4, false, 0xFFE9u },
    { "WINDOWS",         0, 0x5B, true,  0xFFEBu },

    { "ESCAPE",        256, 0x1B, false, 0xFF1Bu },
    { "ENTER",         257, 0x0D, false, 0xFF0Du },
    { "TAB",           258, 0x09, false, 0xFF09u },
    { "BACKSPACE",     259, 0x08, false, 0xFF08u },
    { "SPACE",          32, 0x20, false, 0x0020u },
    { "INSERT",        260, 0x2D, true,  0xFF63u },
    { "DELETE",        261, 0x2E, true,  0xFFFFu },

    { "RIGHT",         262, 0x27, true,  0xFF53u },
    { "LEFT",          263, 0x25, true,  0xFF51u },
    { "DOWN",          264, 0x28, true,  0xFF54u },
    { "UP",            265, 0x26, true,  0xFF52u },
    { "PAGE_UP",       266, 0x21, true,  0xFF55u },
    { "PAGE_DOWN",     267, 0x22, true,  0xFF56u },
    { "HOME",          268, 0x24, true,  0xFF50u },
    { "END",           269, 0x23, true,  0xFF57u },

    { "CAPS_LOCK",     280, 0x14, false, 0xFFE5u },
    { "SCROLL_LOCK",   281, 0x91, false, 0xFF14u },
    { "NUM_LOCK",      282, 0x90, true,  0xFF7Fu },
    { "PRINT_SCREEN",  283, 0x2C, true,  0xFF61u },
    { "PAUSE",         284, 0x13, false, 0xFF13u },

    /* Punctuation, named by the key rather than by the character it produces,
       because that character depends on the layout. */
    { "MINUS",          45, 0xBD, false, 0x002Du },
    { "EQUAL",          61, 0xBB, false, 0x003Du },
    { "BRACKET_LEFT",   91, 0xDB, false, 0x005Bu },
    { "BRACKET_RIGHT",  93, 0xDD, false, 0x005Du },
    { "BACKSLASH",      92, 0xDC, false, 0x005Cu },
    { "SEMICOLON",      59, 0xBA, false, 0x003Bu },
    { "APOSTROPHE",     39, 0xDE, false, 0x0027u },
    { "GRAVE",          96, 0xC0, false, 0x0060u },
    { "COMMA",          44, 0xBC, false, 0x002Cu },
    { "PERIOD",         46, 0xBE, false, 0x002Eu },
    { "SLASH",          47, 0xBF, false, 0x002Fu },

    { "KP_0",          320, 0x60, false, 0xFFB0u },
    { "KP_1",          321, 0x61, false, 0xFFB1u },
    { "KP_2",          322, 0x62, false, 0xFFB2u },
    { "KP_3",          323, 0x63, false, 0xFFB3u },
    { "KP_4",          324, 0x64, false, 0xFFB4u },
    { "KP_5",          325, 0x65, false, 0xFFB5u },
    { "KP_6",          326, 0x66, false, 0xFFB6u },
    { "KP_7",          327, 0x67, false, 0xFFB7u },
    { "KP_8",          328, 0x68, false, 0xFFB8u },
    { "KP_9",          329, 0x69, false, 0xFFB9u },
    { "KP_DECIMAL",    330, 0x6E, false, 0xFFAEu },
    { "KP_DIVIDE",     331, 0x6F, true,  0xFFAFu },
    { "KP_MULTIPLY",   332, 0x6A, false, 0xFFAAu },
    { "KP_SUBTRACT",   333, 0x6D, false, 0xFFADu },
    { "KP_ADD",        334, 0x6B, false, 0xFFABu },
    { "KP_ENTER",      335, 0x0D, true,  0xFF8Du },

    /*
     * Media keys. No GLFW code exists for these, so they cannot be recorded by
     * pressing them -- the window never sees them -- but they can be typed
     * into a macro, which is the point of having them on a pad.
     */
    { "VOLUME_UP",       0, 0xAF, false, 0x1008FF13u },
    { "VOLUME_DOWN",     0, 0xAE, false, 0x1008FF11u },
    { "VOLUME_MUTE",     0, 0xAD, false, 0x1008FF12u },

    /* One key, and it toggles: the same code starts a stopped player and
       stops a playing one. Named for what it does rather than for half of
       it, with the old spelling still understood below. */
    { "MEDIA_PLAY_PAUSE", 0, 0xB3, false, 0x1008FF14u },
    { "MEDIA_STOP",      0, 0xB2, false, 0x1008FF15u },
    { "MEDIA_NEXT",      0, 0xB0, false, 0x1008FF17u },
    { "MEDIA_PREV",      0, 0xB1, false, 0x1008FF16u },
};

#define KEY_COUNT ((int)(sizeof(s_keys) / sizeof(s_keys[0])))

/*
 * Names that were written into configurations before the table used the ones
 * above. They are understood but never offered, so a pad set up by an older
 * version keeps playing without the picker listing the same key twice.
 */
static const struct {
    const char *was;
    const char *now;
} s_aliases[] = {
    { "MEDIA_PLAY", "MEDIA_PLAY_PAUSE" },
};

#define ALIAS_COUNT ((int)(sizeof(s_aliases) / sizeof(s_aliases[0])))

/* Held for a moment before the release sweep, because a chord released in the
   same millisecond it was pressed is missed by a fair number of programs. */
#define KEYSEND_HOLD_MS 15

/* Long enough for the program being pasted into to have read the clipboard.
   Putting the old contents back sooner pastes those instead. */
#define KEYSEND_PASTE_MS 120

/* Between characters when they are typed out one at a time. */
#define KEYSEND_TYPE_MS 6

/*
 * Longest the worker sleeps without looking at s_running. A macro may ask for
 * ten seconds a step across sixteen steps, and nothing may hold the program
 * open for that long, so every wait is served in slices this size.
 */
#define KEYSEND_SLICE_MS 20

#define KEYSEND_QUEUE   8

typedef struct {
    char cmds[KEYSEND_STEPS_MAX][KEYSEND_NAME_MAX];
    int  timings[KEYSEND_STEPS_MAX];
    int  count;

    /* Set instead of the steps: an application to start or stop playing,
       which is a macro in every way except that no key is pressed. */
    char media[KEYSEND_APP_MAX];

    /* Where the steps go. Empty is the desktop, which is what every macro
       did before there was anywhere else to send them. */
    char target[KEYSEND_APP_MAX];

    /* Typed in place of the chord above, and only when count is zero. */
    char text[KEYSEND_TEXT_MAX];
} job_t;

static job_t s_queue[KEYSEND_QUEUE];
static int   s_head;                 /* next job to play  */
static int   s_tail;                 /* next free slot    */

static volatile bool s_running;
static volatile bool s_ready;        /* worker finished opening the backend */
static volatile bool s_available;
static char s_error[160] = "not started";

/* ------------------------------------------------------------- locking --- */

#ifdef _WIN32
static CRITICAL_SECTION s_lock;
static HANDLE s_thread;

static void lock_init(void)    { InitializeCriticalSection(&s_lock); }
static void lock_destroy(void) { DeleteCriticalSection(&s_lock); }
static void lock(void)         { EnterCriticalSection(&s_lock); }
static void unlock(void)       { LeaveCriticalSection(&s_lock); }
static void nap(int ms)        { Sleep((DWORD)ms); }
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static bool s_thread_valid;

static void lock_init(void)    {}
static void lock_destroy(void) {}
static void lock(void)         { pthread_mutex_lock(&s_lock); }
static void unlock(void)       { pthread_mutex_unlock(&s_lock); }

static void nap(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* --------------------------------------------------------------- table --- */

/* Case-insensitive, so a hand-typed "ctrl_left" is accepted. */
static bool same_name(const char *a, const char *b)
{
    for (; *a != 0 && *b != 0; a++, b++) {
        char ca = *a;
        char cb = *b;

        if (ca >= 97 && ca <= 122) {
            ca = (char)(ca - 32);
        }
        if (cb >= 97 && cb <= 122) {
            cb = (char)(cb - 32);
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

/*
 * Macros that went nowhere, and the reason for the last one.
 *
 * Written on the worker and read by the interface. A message half replaced is
 * still readable and a count read a frame late still moves, so neither is
 * worth a lock.
 */
static unsigned s_undelivered;
static char     s_undelivered_why[160];

unsigned keysend_failures(void)
{
    return s_undelivered;
}

const char *keysend_failure(void)
{
    return s_undelivered_why;
}

static void undelivered(const char *app, const char *why)
{
    snprintf(s_undelivered_why, sizeof(s_undelivered_why), "%.40s %s", app, why);
    s_undelivered++;
}

static const entry_t *lookup(const char *name)
{
    if (name == NULL || name[0] == 0) {
        return NULL;
    }

    for (int i = 0; i < KEY_COUNT; i++) {
        if (same_name(s_keys[i].name, name)) {
            return &s_keys[i];
        }
    }

    for (int i = 0; i < ALIAS_COUNT; i++) {
        if (same_name(s_aliases[i].was, name)) {
            return lookup(s_aliases[i].now);
        }
    }
    return NULL;
}

bool keysend_known(const char *name)
{
    return lookup(name) != NULL;
}

const char *keysend_name_of(int glfw_key)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        if (s_keys[i].glfw == glfw_key && s_keys[i].glfw != 0) {
            return s_keys[i].name;
        }
    }
    return NULL;
}

const char *keysend_name_of_char(int codepoint)
{
    /* Every printable entry stores its unshifted character as the X keysym,
       so one comparison covers letters, digits and punctuation alike. */
    if (codepoint >= 65 && codepoint <= 90) {
        codepoint += 32;            /* fold a shifted letter back down */
    }
    if (codepoint <= 0 || codepoint > 126) {
        return NULL;
    }

    for (int i = 0; i < KEY_COUNT; i++) {
        /* Media keys have no position and no character; their keysyms are
           well outside ASCII, but the guard is cheap and states the intent. */
        if (s_keys[i].glfw != 0 && s_keys[i].x11 == (unsigned)codepoint) {
            return s_keys[i].name;
        }
    }
    return NULL;
}

int keysend_names(const char **out, int max)
{
    int count = 0;

    for (int i = 0; i < KEY_COUNT && count < max; i++) {
        out[count++] = s_keys[i].name;
    }
    return count;
}

/* ------------------------------------------------------------- backend --- */

#ifdef _WIN32

static bool backend_open(void)
{
    return true;        /* SendInput needs nothing opened */
}

static void backend_close(void) {}

static void backend_key(const entry_t *e, bool down)
{
    INPUT in;
    memset(&in, 0, sizeof(in));

    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)e->win;
    in.ki.dwFlags = (DWORD)((down ? 0 : KEYEVENTF_KEYUP) |
                            (e->extended ? KEYEVENTF_EXTENDEDKEY : 0));

    SendInput(1, &in, sizeof(in));
}

/*
 * The same key, posted to one window instead of typed at the desktop.
 *
 * A posted key carries its own scan code and flags because that is what the
 * receiving program reads out of the message. What it cannot carry is the
 * state of the modifier keys: those are read from the keyboard itself, so a
 * targeted Ctrl+C is a C with no Ctrl as far as most programs are concerned.
 * Single keys arrive whole, which is what this is for.
 */
static void backend_key_to(uintptr_t window, const entry_t *e, bool down)
{
    HWND hwnd = (HWND)window;
    UINT scan = MapVirtualKeyW((UINT)e->win, MAPVK_VK_TO_VSC);

    LPARAM low = 1 | ((LPARAM)scan << 16) |
                 (e->extended ? ((LPARAM)1 << 24) : 0);

    if (down) {
        PostMessageW(hwnd, WM_KEYDOWN, (WPARAM)e->win, low);
    } else {
        /* The two top bits say the key was down and is now going up. */
        PostMessageW(hwnd, WM_KEYUP, (WPARAM)e->win, low | ((LPARAM)3 << 30));
    }
}

/* Wide copy of @p text in a block the clipboard can take ownership of. */
static HGLOBAL wide_block(const char *text)
{
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (count <= 0) {
        return NULL;
    }

    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)count * sizeof(WCHAR));
    if (block == NULL) {
        return NULL;
    }

    WCHAR *raw = (WCHAR *)GlobalLock(block);
    if (raw == NULL) {
        GlobalFree(block);
        return NULL;
    }

    MultiByteToWideChar(CP_UTF8, 0, text, -1, raw, count);
    GlobalUnlock(block);
    return block;
}

/*
 * Hands @p block to the clipboard, which then owns it: on success it must not
 * be freed here.
 */
static bool clipboard_put(HGLOBAL block)
{
    if (!OpenClipboard(NULL)) {
        return false;
    }

    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, block) != NULL;
    CloseClipboard();
    return ok;
}

/* A copy of whatever text the clipboard holds, for putting back afterwards. */
static HGLOBAL clipboard_take(void)
{
    if (!OpenClipboard(NULL)) {
        return NULL;
    }

    HANDLE held = GetClipboardData(CF_UNICODETEXT);
    HGLOBAL copy = NULL;

    if (held != NULL) {
        const WCHAR *raw = (const WCHAR *)GlobalLock(held);

        if (raw != NULL) {
            SIZE_T bytes = (wcslen(raw) + 1) * sizeof(WCHAR);
            copy = GlobalAlloc(GMEM_MOVEABLE, bytes);

            if (copy != NULL) {
                void *dest = GlobalLock(copy);

                if (dest != NULL) {
                    memcpy(dest, raw, bytes);
                    GlobalUnlock(copy);
                } else {
                    GlobalFree(copy);
                    copy = NULL;
                }
            }
            GlobalUnlock(held);
        }
    }

    CloseClipboard();
    return copy;
}

/*
 * Pastes rather than types: a keystroke per character depends on the target
 * layout matching ours, and is slow enough to interleave with real typing.
 * Whatever the clipboard held is put back afterwards, though only text can be
 * saved -- an image is lost, there being nothing to copy it into without
 * knowing its format.
 */
static void backend_text(const char *text)
{
    HGLOBAL saved = clipboard_take();
    HGLOBAL ours = wide_block(text);

    if (ours == NULL || !clipboard_put(ours)) {
        if (ours != NULL) {
            GlobalFree(ours);
        }
        if (saved != NULL) {
            GlobalFree(saved);
        }
        return;
    }

    const entry_t *ctrl = lookup("CTRL_LEFT");
    const entry_t *v = lookup("V");

    if (ctrl != NULL && v != NULL) {
        backend_key(ctrl, true);
        backend_key(v, true);
        nap(KEYSEND_HOLD_MS);
        backend_key(v, false);
        backend_key(ctrl, false);
    }

    nap(KEYSEND_PASTE_MS);

    if (saved != NULL && !clipboard_put(saved)) {
        GlobalFree(saved);
    }
}

#elif defined(KEYSEND_HAVE_XTEST)

static Display *s_display;

static bool backend_open(void)
{
    s_display = XOpenDisplay(NULL);
    if (s_display == NULL) {
        snprintf(s_error, sizeof(s_error),
                 "no X display; macros need an X11 or XWayland session");
        return false;
    }

    int event = 0, error = 0, major = 0, minor = 0;
    if (!XTestQueryExtension(s_display, &event, &error, &major, &minor)) {
        XCloseDisplay(s_display);
        s_display = NULL;
        snprintf(s_error, sizeof(s_error),
                 "the X server has no XTEST extension");
        return false;
    }

    return true;
}

static void backend_close(void)
{
    if (s_display != NULL) {
        XCloseDisplay(s_display);
        s_display = NULL;
    }
}

static void backend_key(const entry_t *e, bool down)
{
    KeyCode code = XKeysymToKeycode(s_display, (KeySym)e->x11);
    if (code == 0) {
        return;         /* the current layout cannot produce this key */
    }

    XTestFakeKeyEvent(s_display, code, down ? True : False, CurrentTime);
    XFlush(s_display);
}

/*
 * The same key, sent to one window rather than to whatever has the focus.
 *
 * XTEST cannot aim: it drives the server's idea of the keyboard, so it always
 * lands on the focused window. Aiming means handing the client a synthetic
 * event, which it is free to ignore -- and Qt and GTK both do, unless the
 * program asked for them. Sent anyway, because the programs that do accept
 * them are reached no other way, and because the alternative is to pretend the
 * option does not exist on this platform.
 */
static void backend_key_to(uintptr_t window, const entry_t *e, bool down)
{
    KeyCode code = XKeysymToKeycode(s_display, (KeySym)e->x11);
    if (code == 0 || window == 0) {
        return;
    }

    XKeyEvent event;
    memset(&event, 0, sizeof(event));

    event.display = s_display;
    event.window = (Window)window;
    event.root = DefaultRootWindow(s_display);
    event.subwindow = None;
    event.time = CurrentTime;
    event.same_screen = True;
    event.keycode = code;
    event.state = 0;
    event.type = down ? KeyPress : KeyRelease;

    XSendEvent(s_display, (Window)window, True,
               down ? KeyPressMask : KeyReleaseMask, (XEvent *)&event);
    XFlush(s_display);
}

/*
 * Typed a character at a time rather than pasted. Owning the X clipboard means
 * staying available to answer requests for it, which a worker thread with no
 * event loop cannot do; typing needs nothing of the sort, and leaves whatever
 * the user had copied alone.
 *
 * Printable ASCII is its own keysym, so the only question per character is
 * whether it sits at the shifted level of its key.
 */
static void backend_text(const char *text)
{
    const entry_t *shift = lookup("SHIFT_LEFT");
    KeyCode shift_code = (shift != NULL)
                       ? XKeysymToKeycode(s_display, (KeySym)shift->x11)
                       : 0;

    for (const char *p = text; *p != 0; p++) {
        KeySym symbol = (KeySym)(unsigned char)*p;
        KeyCode code = XKeysymToKeycode(s_display, symbol);

        if (code == 0) {
            continue;   /* not on this layout at all */
        }

        bool shifted = (XkbKeycodeToKeysym(s_display, code, 0, 0) != symbol) &&
                       (shift_code != 0);

        if (shifted) {
            XTestFakeKeyEvent(s_display, shift_code, True, CurrentTime);
        }

        XTestFakeKeyEvent(s_display, code, True, CurrentTime);
        XTestFakeKeyEvent(s_display, code, False, CurrentTime);

        if (shifted) {
            XTestFakeKeyEvent(s_display, shift_code, False, CurrentTime);
        }

        XFlush(s_display);
        nap(KEYSEND_TYPE_MS);
    }
}

#else

static bool backend_open(void)
{
    snprintf(s_error, sizeof(s_error),
             "built without XTEST; install libxtst-dev and rebuild");
    return false;
}

static void backend_close(void) {}

static void backend_key(const entry_t *e, bool down)
{
    (void)e;
    (void)down;
}

static void backend_key_to(uintptr_t window, const entry_t *e, bool down)
{
    (void)window;
    (void)e;
    (void)down;
}

static void backend_text(const char *text)
{
    (void)text;
}

#endif

/* -------------------------------------------------------------- worker --- */

/*
 * Waits @p ms, in slices, giving up early when the program is shutting down.
 *
 * @return false when the wait was cut short, which means stop.
 */
static bool nap_interruptible(int ms)
{
    while (ms > 0) {
        if (!s_running) {
            return false;
        }

        int slice = (ms > KEYSEND_SLICE_MS) ? KEYSEND_SLICE_MS : ms;
        nap(slice);
        ms -= slice;
    }
    return s_running;
}

/*
 * Presses every step in order, waiting its timing first, then releases them in
 * the reverse order. A step naming a key this platform does not have is
 * skipped rather than abandoning the macro, so one bad line does not leave a
 * modifier held down.
 *
 * An interrupted macro still runs its release sweep. Leaving a modifier down
 * is not a local matter: the key stays pressed for the whole desktop, and the
 * program that did it is gone.
 */
static void play(const job_t *job)
{
    const entry_t *held[KEYSEND_STEPS_MAX];
    int count = 0;

    /*
     * Nothing to press. It rides this queue because it is what a key press
     * does, and because the search it performs can take long enough to drop a
     * frame if it were done where the press was noticed.
     */
    if (job->media[0] != 0) {
        media_toggle(job->media);
        return;
    }

    /* A phrase is the whole macro when there is one: nothing to hold, and
       nothing to release afterwards. */
    if (job->count == 0) {
        if (job->text[0] != 0) {
            if (!nap_interruptible(job->timings[0])) {
                return;     /* nothing pressed yet, so nothing to undo */
            }
            backend_text(job->text);
        }
        return;
    }

    /*
     * Aimed at one program, the window is found once: asking the window system
     * per step would be slower and could answer differently halfway through a
     * chord.
     */
    uintptr_t window = 0;

    if (job->target[0] != 0) {
        window = foreground_window_for(job->target);

        if (window == 0) {
            undelivered(job->target, "has no window to send the macro to");
            return;
        }
    }

    for (int i = 0; i < job->count; i++) {
        if (!nap_interruptible(job->timings[i])) {
            break;          /* release what is already down, below */
        }

        const entry_t *e = lookup(job->cmds[i]);
        if (e == NULL) {
            continue;
        }

        if (window != 0) {
            backend_key_to(window, e, true);
        } else {
            backend_key(e, true);
        }
        held[count++] = e;
    }

    if (count > 0) {
        nap(KEYSEND_HOLD_MS);   /* short and fixed; worth finishing */
    }

    for (int i = count - 1; i >= 0; i--) {
        if (window != 0) {
            backend_key_to(window, held[i], false);
        } else {
            backend_key(held[i], false);
        }
    }
}

/*
 * Owns the backend for its whole life. The X display is not safe to share
 * between threads without XInitThreads(), and opening it here keeps every call
 * on the one thread that made it.
 */
static void worker_loop(void)
{
    s_available = backend_open();
    s_ready = true;

    if (s_available) {
        snprintf(s_error, sizeof(s_error), "ready");
    }

    while (s_running) {
        job_t work;
        bool have = false;

        lock();
        if (s_head != s_tail) {
            work = s_queue[s_head];
            s_head = (s_head + 1) % KEYSEND_QUEUE;
            have = true;
        }
        unlock();

        if (!have) {
            nap(8);
            continue;
        }

        if (s_available) {
            play(&work);
        }
    }

    backend_close();
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID arg)
{
    (void)arg;
    worker_loop();
    return 0;
}
#else
static void *worker_entry(void *arg)
{
    (void)arg;
    worker_loop();
    return NULL;
}
#endif

/* ----------------------------------------------------------------- api --- */

bool keysend_start(void)
{
    if (s_running) {
        return s_available;
    }

    lock_init();

    s_head = 0;
    s_tail = 0;
    s_ready = false;
    s_available = false;
    s_running = true;

#ifdef _WIN32
    s_thread = CreateThread(NULL, 0, worker_entry, NULL, 0, NULL);
    if (s_thread == NULL) {
        s_running = false;
        lock_destroy();
        snprintf(s_error, sizeof(s_error), "could not start the macro thread");
        return false;
    }
#else
    if (pthread_create(&s_thread, NULL, worker_entry, NULL) != 0) {
        s_running = false;
        lock_destroy();
        snprintf(s_error, sizeof(s_error), "could not start the macro thread");
        return false;
    }
    s_thread_valid = true;
#endif

    /* Wait for the backend so the caller can report the outcome once, at
       startup, rather than on the first key press. */
    for (int waited = 0; !s_ready && waited < 2000; waited += 10) {
        nap(10);
    }

    return s_available;
}

void keysend_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;
    s_available = false;

#ifdef _WIN32
    if (s_thread != NULL) {
        /*
         * The worker checks s_running every slice, so this is generous. Should
         * it somehow still be running -- stuck inside a platform call -- the
         * handle and the lock are deliberately left alone: deleting a critical
         * section another thread is about to enter is worse than leaking one
         * at exit.
         */
        if (WaitForSingleObject(s_thread, 3000) == WAIT_OBJECT_0) {
            CloseHandle(s_thread);
            s_thread = NULL;
            lock_destroy();
        }
    } else {
        lock_destroy();
    }
#else
    if (s_thread_valid) {
        pthread_join(s_thread, NULL);
        s_thread_valid = false;
    }
    lock_destroy();
#endif
}

bool keysend_available(void)
{
    return s_available;
}

const char *keysend_last_error(void)
{
    return s_error;
}

/* Puts a filled job on the queue. One slot stays free to mark "empty". */
static bool enqueue(const job_t *job)
{
    bool queued = false;

    lock();
    int next = (s_tail + 1) % KEYSEND_QUEUE;

    if (next != s_head) {
        s_queue[s_tail] = *job;
        s_tail = next;
        queued = true;
    }
    unlock();

    return queued;
}

bool keysend_play_text(const char *text, int delay_ms)
{
    if (!s_running || !s_available || text == NULL || text[0] == 0) {
        return false;
    }

    job_t job;
    memset(&job, 0, sizeof(job));

    job.count = 0;
    job.timings[0] = (delay_ms > 0) ? delay_ms : 0;
    snprintf(job.text, KEYSEND_TEXT_MAX, "%s", text);

    return enqueue(&job);
}

bool keysend_play_media(const char *app)
{
    if (!s_running || app == NULL || app[0] == 0) {
        return false;
    }

    /* Deliberately not gated on s_available: that says whether keystrokes can
       be synthesised, and this sends none. A desktop with no XTEST can still
       tell a player to pause. */
    job_t job;
    memset(&job, 0, sizeof(job));
    snprintf(job.media, KEYSEND_APP_MAX, "%s", app);

    return enqueue(&job);
}

/* The common half of the two below: everything except where it goes. */
static bool queue_chord(const char *app, const char *const *cmds,
                        const int *timings, int count)
{
    if (!s_running || cmds == NULL || count <= 0) {
        return false;
    }

    if (count > KEYSEND_STEPS_MAX) {
        count = KEYSEND_STEPS_MAX;
    }

    job_t job;
    memset(&job, 0, sizeof(job));
    job.count = count;

    if (app != NULL) {
        snprintf(job.target, KEYSEND_APP_MAX, "%s", app);
    }

    for (int i = 0; i < count; i++) {
        snprintf(job.cmds[i], KEYSEND_NAME_MAX, "%s", cmds[i] ? cmds[i] : "");
        job.timings[i] = timings ? timings[i] : 0;
    }

    return enqueue(&job);
}

bool keysend_play(const char *const *cmds, const int *timings, int count)
{
    /* Synthesising a keystroke is what needs a backend; posting one to a
       window does not, which is why only this half asks. */
    if (!s_available) {
        return false;
    }
    return queue_chord(NULL, cmds, timings, count);
}

bool keysend_play_to(const char *app, const char *const *cmds,
                     const int *timings, int count)
{
    if (app == NULL || app[0] == 0) {
        return false;
    }
    return queue_chord(app, cmds, timings, count);
}
