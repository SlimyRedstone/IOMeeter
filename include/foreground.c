#include "foreground.h"

#include "appname.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
/* QueryFullProcessImageName and the DWM attributes are Vista and later. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <stdlib.h>
#include <windows.h>
#include <dwmapi.h>
#endif

#if !defined(_WIN32) && defined(FOREGROUND_HAVE_X11)
#include <stdlib.h>
#include <unistd.h>         /* readlink, getpid: the process behind a window */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>   /* XClassHint: the window class, not in Xlib.h */
#endif

/* Whether a real backend is being built. The helpers below belong to those
   two, and a platform with neither must not carry them unused. */
#if defined(_WIN32) || defined(FOREGROUND_HAVE_X11)
#define FOREGROUND_REAL 1
#endif

static bool s_available;
static char s_error[160] = "not started";

#ifdef FOREGROUND_REAL

static bool same_process(const char *a, const char *b)
{
    for (; *a != 0 && *b != 0; a++, b++) {
        char ca = *a;
        char cb = *b;

        if (ca >= 65 && ca <= 90) {
            ca = (char)(ca + 32);
        }
        if (cb >= 65 && cb <= 90) {
            cb = (char)(cb + 32);
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

static bool already_listed(const foreground_app_t *out, int count,
                           const char *process)
{
    for (int i = 0; i < count; i++) {
        if (same_process(out[i].process, process)) {
            return true;
        }
    }
    return false;
}

#endif /* FOREGROUND_REAL */

/* ------------------------------------------------------------- windows --- */

#ifdef _WIN32

typedef struct {
    foreground_app_t *out;
    int               count;
    int               max;
    DWORD             self;
} collect_t;

/*
 * "Google Chrome" rather than "chrome.exe", out of the file's own version
 * resource. That is where a program states what it is called, and it is what
 * Explorer and the task manager show.
 */
static bool description_of(const char *path, char *out, size_t size)
{
    DWORD ignored = 0;
    DWORD bytes = GetFileVersionInfoSizeA(path, &ignored);

    if (bytes == 0) {
        return false;       /* no version resource; plenty of programs have none */
    }

    void *block = malloc(bytes);
    if (block == NULL) {
        return false;
    }

    bool ok = false;

    if (GetFileVersionInfoA(path, 0, bytes, block)) {
        /* The strings sit under whichever language the file declares, so the
           translation table says which key to ask for. */
        struct { WORD language; WORD codepage; } *translation = NULL;
        UINT count = 0;

        if (VerQueryValueA(block, "\\VarFileInfo\\Translation",
                           (void **)&translation, &count) &&
            translation != NULL && count >= sizeof(*translation)) {
            char key[64];
            snprintf(key, sizeof(key),
                     "\\StringFileInfo\\%04x%04x\\FileDescription",
                     translation[0].language, translation[0].codepage);

            char *text = NULL;
            UINT length = 0;

            if (VerQueryValueA(block, key, (void **)&text, &length) &&
                text != NULL && text[0] != 0) {
                snprintf(out, size, "%s", text);
                ok = true;
            }
        }
    }

    free(block);
    return ok;
}

/* Executable behind a window, as a basename. @p name receives the friendly
   name when the caller wants one. */
static bool process_of_window(HWND window, char *out, size_t size,
                              DWORD *pid_out, char *name, size_t name_size)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);

    if (pid_out != NULL) {
        *pid_out = pid;
    }
    if (pid == 0) {
        return false;
    }

    /* LIMITED_INFORMATION is enough for the image name and, unlike
       QUERY_INFORMATION, is granted for processes at a higher integrity
       level, so elevated programs still resolve. */
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == NULL) {
        return false;
    }

    char path[MAX_PATH];
    DWORD length = MAX_PATH;
    bool ok = QueryFullProcessImageNameA(handle, 0, path, &length) != 0;

    CloseHandle(handle);

    if (!ok) {
        return false;
    }

    if (name != NULL && !description_of(path, name, name_size)) {
        name[0] = 0;
    }

    snprintf(out, size, "%s", appname_base(path));
    return out[0] != 0;
}

/*
 * A window belongs on the taskbar when it is visible, unowned, not a tool
 * window, not cloaked, and has a caption. Cloaking is the one that matters on
 * Windows 10: the shell keeps invisible windows for suspended store apps and
 * for "Windows Input Experience", and only DWM knows they are not on screen.
 */
static bool is_desktop_window(HWND window)
{
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL) {
        return false;
    }

    LONG_PTR extended = GetWindowLongPtrA(window, GWL_EXSTYLE);
    if ((extended & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    int cloaked = 0;
    if (DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                              sizeof(cloaked)) == S_OK && cloaked != 0) {
        return false;
    }

    return GetWindowTextLengthA(window) > 0;
}

static BOOL CALLBACK on_window(HWND window, LPARAM param)
{
    collect_t *state = (collect_t *)param;

    if (state->count >= state->max || !is_desktop_window(window)) {
        return TRUE;
    }

    char process[FOREGROUND_NAME_MAX];
    char name[FOREGROUND_TITLE_MAX];
    DWORD pid = 0;

    if (!process_of_window(window, process, sizeof(process), &pid,
                           name, sizeof(name))) {
        return TRUE;
    }
    if (pid == state->self || already_listed(state->out, state->count, process)) {
        return TRUE;
    }

    foreground_app_t *app = &state->out[state->count];
    snprintf(app->process, sizeof(app->process), "%s", process);
    GetWindowTextA(window, app->title, FOREGROUND_TITLE_MAX);

    /* Falling back through the window caption to the executable, so a program
       with no version resource is still called something. */
    if (name[0] != 0) {
        snprintf(app->name, sizeof(app->name), "%s", name);
    } else if (app->title[0] != 0) {
        snprintf(app->name, sizeof(app->name), "%s", app->title);
    } else {
        snprintf(app->name, sizeof(app->name), "%.*s",
                 (int)appname_length(process), process);
    }

    state->count++;
    return TRUE;
}

static bool backend_init(void)
{
    return true;        /* the window list needs nothing opened */
}

static void backend_shutdown(void) {}

/* Carries the search through EnumWindows, which takes one pointer. */
typedef struct {
    const char *want;
    HWND        best;       /*!< a window on the taskbar */
    HWND        any;        /*!< any window at all, as a fallback */
} find_t;

static BOOL CALLBACK on_find(HWND window, LPARAM param)
{
    find_t *find = (find_t *)param;
    char process[FOREGROUND_NAME_MAX];

    if (!IsWindowVisible(window) ||
        !process_of_window(window, process, sizeof(process), NULL, NULL, 0) ||
        !appname_same(process, find->want)) {
        return TRUE;
    }

    if (is_desktop_window(window)) {
        find->best = window;
        return FALSE;       /* the main window; nothing better to find */
    }
    if (find->any == NULL) {
        find->any = window;
    }
    return TRUE;
}

static uintptr_t backend_window_for(const char *app)
{
    find_t find = { app, NULL, NULL };

    EnumWindows(on_find, (LPARAM)&find);
    return (uintptr_t)(find.best != NULL ? find.best : find.any);
}

static int backend_list(foreground_app_t *out, int max)
{
    collect_t state = { out, 0, max, GetCurrentProcessId() };

    EnumWindows(on_window, (LPARAM)&state);
    return state.count;
}

static bool backend_active(char *out, size_t size)
{
    HWND window = GetForegroundWindow();

    if (window == NULL) {
        return false;
    }

    DWORD pid = 0;
    if (!process_of_window(window, out, size, &pid, NULL, 0)) {
        return false;
    }

    if (pid == GetCurrentProcessId()) {
        out[0] = 0;
        return false;       /* our own window: nothing else is in front */
    }

    return true;
}

/* ----------------------------------------------------------------- x11 --- */

#elif defined(FOREGROUND_HAVE_X11)

static Display *s_display;
static Window   s_root;

static Atom s_client_list;
static Atom s_active_window;
static Atom s_window_pid;
static Atom s_window_type;
static Atom s_type_normal;

/*
 * Read one property. The caller frees with XFree(). Returns NULL when the
 * property is absent, which is normal: not every window sets every hint.
 */
static unsigned char *property_of(Window window, Atom property, Atom type,
                                  unsigned long *count)
{
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long items = 0;
    unsigned long remaining = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(s_display, window, property, 0, 1024, False, type,
                           &actual_type, &actual_format, &items, &remaining,
                           &data) != Success) {
        return NULL;
    }

    if (data != NULL && items == 0) {
        XFree(data);
        return NULL;
    }

    if (count != NULL) {
        *count = items;
    }
    return data;
}

/* Executable behind a pid, from the link the kernel keeps for it. */
static bool process_of_pid(unsigned long pid, char *out, size_t size)
{
    char link[64];
    char path[512];

    snprintf(link, sizeof(link), "/proc/%lu/exe", pid);

    ssize_t length = readlink(link, path, sizeof(path) - 1);
    if (length <= 0) {
        /* A sandboxed process may hide its link but still publish a name. */
        snprintf(link, sizeof(link), "/proc/%lu/comm", pid);

        FILE *f = fopen(link, "rb");
        if (f == NULL) {
            return false;
        }

        if (fgets(path, (int)sizeof(path), f) == NULL) {
            fclose(f);
            return false;
        }
        fclose(f);

        path[strcspn(path, "\n")] = 0;
        snprintf(out, size, "%s", appname_base(path));
        return out[0] != 0;
    }

    path[length] = 0;
    snprintf(out, size, "%s", appname_base(path));
    return out[0] != 0;
}

static bool process_of_window(Window window, char *out, size_t size)
{
    unsigned long count = 0;
    unsigned char *data = property_of(window, s_window_pid, XA_CARDINAL, &count);

    if (data == NULL) {
        return false;
    }

    unsigned long pid = *(unsigned long *)(void *)data;
    XFree(data);

    return (pid != 0) && process_of_pid(pid, out, size);
}

/*
 * A window belongs on the taskbar when the manager lists it and it either
 * declares itself normal or declares nothing at all. Docks, panels and menus
 * all say what they are, so filtering on the type is enough here; the client
 * list has already left out everything unmapped.
 */
/*
 * The class a toolkit sets on its windows, which is the closest X has to the
 * name a program calls itself: "Google-chrome", "Firefox", "Steam". Better
 * than the caption, which follows whatever document is open.
 */
static bool class_of(Window window, char *out, size_t size)
{
    XClassHint hint = { NULL, NULL };

    if (!XGetClassHint(s_display, window, &hint)) {
        return false;
    }

    bool ok = false;

    if (hint.res_class != NULL && hint.res_class[0] != 0) {
        snprintf(out, size, "%s", hint.res_class);
        ok = true;
    }

    if (hint.res_name != NULL) {
        XFree(hint.res_name);
    }
    if (hint.res_class != NULL) {
        XFree(hint.res_class);
    }
    return ok;
}

static bool is_desktop_window(Window window)
{
    unsigned long count = 0;
    unsigned char *data = property_of(window, s_window_type, XA_ATOM, &count);

    if (data == NULL) {
        return true;        /* no hint: treat as an ordinary window */
    }

    Atom *types = (Atom *)(void *)data;
    bool normal = false;

    for (unsigned long i = 0; i < count; i++) {
        if (types[i] == s_type_normal) {
            normal = true;
            break;
        }
    }

    XFree(data);
    return normal;
}

static bool backend_init(void)
{
    s_display = XOpenDisplay(NULL);
    if (s_display == NULL) {
        snprintf(s_error, sizeof(s_error),
                 "no X display; the running-application list needs X11 or "
                 "XWayland");
        return false;
    }

    s_root = DefaultRootWindow(s_display);

    s_client_list   = XInternAtom(s_display, "_NET_CLIENT_LIST", False);
    s_active_window = XInternAtom(s_display, "_NET_ACTIVE_WINDOW", False);
    s_window_pid    = XInternAtom(s_display, "_NET_WM_PID", False);
    s_window_type   = XInternAtom(s_display, "_NET_WM_WINDOW_TYPE", False);
    s_type_normal   = XInternAtom(s_display, "_NET_WM_WINDOW_TYPE_NORMAL", False);

    /* Every desktop in use publishes this; without it there is no list to
       read, and saying so beats returning nothing and looking broken. */
    unsigned long count = 0;
    unsigned char *probe = property_of(s_root, s_client_list, XA_WINDOW, &count);

    if (probe == NULL) {
        XCloseDisplay(s_display);
        s_display = NULL;
        snprintf(s_error, sizeof(s_error),
                 "the window manager does not publish _NET_CLIENT_LIST");
        return false;
    }
    XFree(probe);

    return true;
}

static void backend_shutdown(void)
{
    if (s_display != NULL) {
        XCloseDisplay(s_display);
        s_display = NULL;
    }
}

static uintptr_t backend_window_for(const char *app)
{
    unsigned long count = 0;
    unsigned char *data = property_of(s_root, s_client_list, XA_WINDOW, &count);

    if (data == NULL) {
        return 0;
    }

    Window *windows = (Window *)(void *)data;
    Window best = 0;
    Window any = 0;

    for (unsigned long i = 0; i < count && best == 0; i++) {
        char process[FOREGROUND_NAME_MAX];

        if (!process_of_window(windows[i], process, sizeof(process)) ||
            !appname_same(process, app)) {
            continue;
        }

        if (is_desktop_window(windows[i])) {
            best = windows[i];
        } else if (any == 0) {
            any = windows[i];
        }
    }

    XFree(data);
    return (uintptr_t)(best != 0 ? best : any);
}

static int backend_list(foreground_app_t *out, int max)
{
    unsigned long count = 0;
    unsigned char *data = property_of(s_root, s_client_list, XA_WINDOW, &count);

    if (data == NULL) {
        return 0;
    }

    Window *windows = (Window *)(void *)data;
    char self[FOREGROUND_NAME_MAX] = { 0 };
    int written = 0;

    process_of_pid((unsigned long)getpid(), self, sizeof(self));

    for (unsigned long i = 0; i < count && written < max; i++) {
        if (!is_desktop_window(windows[i])) {
            continue;
        }

        char process[FOREGROUND_NAME_MAX];
        if (!process_of_window(windows[i], process, sizeof(process))) {
            continue;
        }
        if (same_process(process, self) ||
            already_listed(out, written, process)) {
            continue;
        }

        foreground_app_t *app = &out[written];
        snprintf(app->process, sizeof(app->process), "%s", process);
        app->title[0] = 0;
        app->name[0] = 0;

        char *title = NULL;
        if (XFetchName(s_display, windows[i], &title) != 0 && title != NULL) {
            snprintf(app->title, sizeof(app->title), "%s", title);
            XFree(title);
        }

        /* Falling back through the caption to the executable, so a window
           that sets no class is still called something. */
        if (!class_of(windows[i], app->name, sizeof(app->name))) {
            if (app->title[0] != 0) {
                snprintf(app->name, sizeof(app->name), "%s", app->title);
            } else {
                snprintf(app->name, sizeof(app->name), "%.*s",
                         (int)appname_length(process), process);
            }
        }

        written++;
    }

    XFree(data);
    return written;
}

static bool backend_active(char *out, size_t size)
{
    unsigned long count = 0;
    unsigned char *data = property_of(s_root, s_active_window, XA_WINDOW, &count);

    if (data == NULL) {
        return false;
    }

    Window window = *(Window *)(void *)data;
    XFree(data);

    if (window == 0 || !process_of_window(window, out, size)) {
        return false;
    }

    char self[FOREGROUND_NAME_MAX] = { 0 };

    if (process_of_pid((unsigned long)getpid(), self, sizeof(self)) &&
        same_process(out, self)) {
        out[0] = 0;
        return false;       /* our own window: nothing else is in front */
    }

    return true;
}

/* ---------------------------------------------------------------- stub --- */

#else

static bool backend_init(void)
{
    snprintf(s_error, sizeof(s_error),
             "built without X11; install libx11-dev and rebuild");
    return false;
}

static void backend_shutdown(void) {}

static uintptr_t backend_window_for(const char *app)
{
    (void)app;
    return 0;
}

static int backend_list(foreground_app_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

static bool backend_active(char *out, size_t size)
{
    (void)out;
    (void)size;
    return false;
}

#endif

/* ----------------------------------------------------------------- api --- */

bool foreground_init(void)
{
    if (s_available) {
        return true;
    }

    s_available = backend_init();
    if (s_available) {
        snprintf(s_error, sizeof(s_error), "ready");
    }
    return s_available;
}

void foreground_shutdown(void)
{
    if (!s_available) {
        return;
    }

    backend_shutdown();
    s_available = false;
}

bool foreground_available(void)
{
    return s_available;
}

const char *foreground_last_error(void)
{
    return s_error;
}

int foreground_list(foreground_app_t *out, int max)
{
    if (!s_available || out == NULL || max <= 0) {
        return 0;
    }
    return backend_list(out, max);
}

uintptr_t foreground_window_for(const char *app)
{
    if (!s_available || app == NULL || app[0] == 0) {
        return 0;
    }
    return backend_window_for(app);
}

bool foreground_active(char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return false;
    }

    out[0] = 0;

    if (!s_available) {
        return false;
    }
    return backend_active(out, size);
}
