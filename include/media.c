/*
 * popen() is POSIX rather than C99. The project builds with the GNU dialect,
 * where it is visible anyway, but saying so here means this file does not
 * quietly depend on that.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "media.h"

#include "appname.h"

#include <stdio.h>
#include <string.h>

static char s_error[MEDIA_ERROR_MAX];

/*
 * Read by the interface thread and written by the worker. Neither a torn
 * message nor a count read an instant late changes what is reported, and a
 * lock around a line of text that only ever explains a failure would cost
 * more than it is worth.
 */
static unsigned s_failures;

/*
 * Record why a call failed.
 *
 * @param fmt  Carries exactly one %s, which is where @p what goes. It is not
 *             a general format: there is one argument and nothing checks it.
 * @param what The application it was about, or NULL for a message that names
 *             nothing.
 */
static void note(const char *fmt, const char *what)
{
    snprintf(s_error, sizeof(s_error), fmt, what ? what : "");
    s_failures++;
}

unsigned media_failures(void)
{
    return s_failures;
}

const char *media_last_error(void)
{
    return s_error;
}

/* ------------------------------------------------------------- matching --- */

/* The file name at the end of a path, which is all an application is known by
   once it is running. */

/* ========================================================================== */
#ifdef _WIN32
/* ========================================================================== */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <tlhelp32.h>
#include <winstring.h>

#ifndef E_ILLEGAL_METHOD_CALL
#define E_ILLEGAL_METHOD_CALL ((HRESULT)0x8000000EL)
#endif

/*
 * Windows keeps one media session per application -- what the volume flyout
 * lists, and what the transport controls on a headset drive. Each one can be
 * addressed on its own, which is the whole point: WM_APPCOMMAND cannot,
 * because a window that does not handle it passes it to DefWindowProc, which
 * hands it to the shell, which turns it back into the global key that toggles
 * whatever played last.
 *
 * The interfaces are declared here rather than included: MinGW ships no
 * windows.media.control.h. The identifiers and the order of the methods below
 * are copied from the Windows SDK header of the same name, and the order is
 * the ABI -- a method in the wrong place calls the wrong function.
 */

static const GUID IID_SessionManagerStatics = {
    0x2050c4ee, 0x11a0, 0x57de,
    { 0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45 }
};

typedef struct rt_statics rt_statics;
typedef struct rt_manager rt_manager;
typedef struct rt_session rt_session;
typedef struct rt_vector  rt_vector;
typedef struct rt_async   rt_async;

/* Every WinRT interface begins with IInspectable, which begins with
   IUnknown. */
#define RT_HEAD(T)                                                            \
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(T *, REFIID, void **);        \
    ULONG   (STDMETHODCALLTYPE *AddRef)(T *);                                 \
    ULONG   (STDMETHODCALLTYPE *Release)(T *);                                \
    HRESULT (STDMETHODCALLTYPE *GetIids)(T *, ULONG *, IID **);               \
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(T *, HSTRING *);         \
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(T *, TrustLevel *)

struct rt_statics_vtbl {
    RT_HEAD(rt_statics);
    HRESULT (STDMETHODCALLTYPE *RequestAsync)(rt_statics *, rt_async **);
};
struct rt_statics { const struct rt_statics_vtbl *lpVtbl; };

struct rt_manager_vtbl {
    RT_HEAD(rt_manager);
    HRESULT (STDMETHODCALLTYPE *GetCurrentSession)(rt_manager *, rt_session **);
    HRESULT (STDMETHODCALLTYPE *GetSessions)(rt_manager *, rt_vector **);
    /* The four event methods after these are never called. */
};
struct rt_manager { const struct rt_manager_vtbl *lpVtbl; };

struct rt_vector_vtbl {
    RT_HEAD(rt_vector);
    HRESULT (STDMETHODCALLTYPE *GetAt)(rt_vector *, UINT32, rt_session **);
    HRESULT (STDMETHODCALLTYPE *get_Size)(rt_vector *, UINT32 *);
};
struct rt_vector { const struct rt_vector_vtbl *lpVtbl; };

struct rt_session_vtbl {
    RT_HEAD(rt_session);
    HRESULT (STDMETHODCALLTYPE *get_SourceAppUserModelId)(rt_session *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *TryGetMediaPropertiesAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *GetTimelineProperties)(rt_session *, void **);
    HRESULT (STDMETHODCALLTYPE *GetPlaybackInfo)(rt_session *, void **);
    HRESULT (STDMETHODCALLTYPE *TryPlayAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryPauseAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryStopAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryRecordAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryFastForwardAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryRewindAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TrySkipNextAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TrySkipPreviousAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryChangeChannelUpAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryChangeChannelDownAsync)(rt_session *, rt_async **);
    HRESULT (STDMETHODCALLTYPE *TryTogglePlayPauseAsync)(rt_session *, rt_async **);
};
struct rt_session { const struct rt_session_vtbl *lpVtbl; };

struct rt_async_vtbl {
    RT_HEAD(rt_async);
    HRESULT (STDMETHODCALLTYPE *put_Completed)(rt_async *, void *);
    HRESULT (STDMETHODCALLTYPE *get_Completed)(rt_async *, void **);
    /* The result is written through the pointer, whatever its type. */
    HRESULT (STDMETHODCALLTYPE *GetResults)(rt_async *, void *);
};
struct rt_async { const struct rt_async_vtbl *lpVtbl; };

#define RT_RELEASE(p) do { if ((p) != NULL) { (p)->lpVtbl->Release(p); } } while (0)

/* Longest a call is given to come back, and how often it is asked. */
#define RT_TIMEOUT_MS 2000
#define RT_POLL_MS    5

/*
 * Wait for an asynchronous call and take its result.
 *
 * Nothing is registered for completion: an operation that has not finished
 * answers GetResults with E_ILLEGAL_METHOD_CALL, which is all this needs to
 * know. Polling it means no callback object to implement in C and no
 * apartment to pump -- this runs in the multi-threaded one, where WinRT
 * completes on a pool thread of its own.
 */
static HRESULT rt_await(rt_async *op, void *out)
{
    for (int waited = 0; waited < RT_TIMEOUT_MS; waited += RT_POLL_MS) {
        HRESULT hr = op->lpVtbl->GetResults(op, out);

        if (hr != E_ILLEGAL_METHOD_CALL) {
            return hr;
        }
        Sleep(RT_POLL_MS);
    }
    return E_ILLEGAL_METHOD_CALL;
}

/*
 * Bring WinRT up on this thread.
 *
 * Only ever called from the one worker that plays macros, so the flag needs no
 * guarding. It is never taken down again: the thread lives as long as the
 * program, and tearing the apartment down between key presses would mean
 * building it again for the next one.
 */
static bool rt_ready(void)
{
    static bool started;
    static bool ok;

    if (started) {
        return ok;
    }
    started = true;

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);

    /* S_FALSE says somebody already did it on this thread, which is fine.
       RPC_E_CHANGED_MODE says they did it differently, which is not. */
    ok = SUCCEEDED(hr) || hr == S_FALSE;
    return ok;
}

/* The UTF-8 of a WinRT string, which is UTF-16. */
static void from_hstring(HSTRING text, char *out, size_t size)
{
    UINT32 length = 0;
    PCWSTR wide = WindowsGetStringRawBuffer(text, &length);

    out[0] = 0;

    if (wide != NULL && length > 0) {
        /* Counted rather than NUL-terminated on the way in, so it comes out
           the same way: the terminator has to be put on by hand, at the end
           of what was actually written. */
        int wrote = WideCharToMultiByte(CP_UTF8, 0, wide, (int)length, out,
                                        (int)size - 1, NULL, NULL);

        out[(wrote > 0) ? (size_t)wrote : 0] = 0;
    }
}

/*
 * Whether a session belongs to the application asked for.
 *
 * A session is labelled with the application's user model id, which for an
 * ordinary program is usually its executable but is not promised to be:
 * Chrome answers "Chrome", a packaged application answers something far
 * longer. So an exact match on the executable is tried first, and failing
 * that the name is looked for inside the label.
 */
static bool session_is(const char *label, const char *app)
{
    if (appname_same(label, app)) {
        return true;
    }

    const char *want_text = appname_base(app);
    size_t want = appname_length(want_text);

    if (want == 0) {
        return false;
    }

    for (const char *at = label; *at != 0; at++) {
        size_t i = 0;

        while (i < want && at[i] != 0) {
            char x = at[i], y = want_text[i];

            if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');

            if (x != y) {
                break;
            }
            i++;
        }

        if (i == want) {
            return true;
        }
    }
    return false;
}

bool media_available(void)
{
    return true;
}

/*
 * Whether @p app is on the machine at all.
 *
 * Asked only when no session matched, to tell two very different situations
 * apart: a program that is not started, and one that is playing but publishes
 * nothing Windows can address. VLC is the usual example of the second -- it
 * takes the media keys through a hook of its own rather than registering a
 * session, so it never appears in the list and no amount of naming it will
 * help.
 */
static bool process_running(const char *app)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    bool found = false;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            char name[MAX_PATH];

            int wrote = WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1,
                                            name, (int)sizeof(name), NULL, NULL);
            found = (wrote > 0) && appname_same(name, app);
        } while (!found && Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

/*
 * Find the session manager, which is the list of everything playing.
 *
 * @return NULL with the reason already noted.
 */
static rt_manager *open_manager(void)
{
    static const WCHAR CLASS_NAME[] =
        L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";

    if (!rt_ready()) {
        note("%sthe Windows runtime would not start on this thread", NULL);
        return NULL;
    }

    HSTRING name = NULL;
    if (FAILED(WindowsCreateString(CLASS_NAME,
                                   (UINT32)(sizeof(CLASS_NAME) / sizeof(WCHAR) - 1),
                                   &name))) {
        note("%sout of memory", NULL);
        return NULL;
    }

    rt_statics *statics = NULL;
    HRESULT hr = RoGetActivationFactory(name, &IID_SessionManagerStatics,
                                        (void **)&statics);
    WindowsDeleteString(name);

    if (FAILED(hr) || statics == NULL) {
        note("%sthis version of Windows has no media session list", NULL);
        return NULL;
    }

    rt_async *op = NULL;
    hr = statics->lpVtbl->RequestAsync(statics, &op);
    RT_RELEASE(statics);

    if (FAILED(hr) || op == NULL) {
        note("%sthe media session list would not open", NULL);
        return NULL;
    }

    rt_manager *manager = NULL;
    hr = rt_await(op, &manager);
    RT_RELEASE(op);

    if (FAILED(hr) || manager == NULL) {
        note("%sthe media session list did not answer", NULL);
        return NULL;
    }
    return manager;
}

/*
 * The session manager, opened once and kept.
 *
 * Asking the runtime for it means a factory lookup and an asynchronous call,
 * and doing that for every key press costs both time and memory that only
 * comes back when the program ends. The manager itself does not go stale --
 * the list of sessions is read from it fresh each time -- so it is built on
 * the first press and reused after that.
 *
 * Only the worker that plays macros ever gets here, so it needs no guarding.
 */
static rt_manager *s_manager;

static void manager_forget(void)
{
    RT_RELEASE(s_manager);
    s_manager = NULL;
}

bool media_toggle(const char *app)
{
    s_error[0] = 0;

    if (app == NULL || app[0] == 0) {
        note("%sno application was named", NULL);
        return false;
    }

    if (s_manager == NULL) {
        s_manager = open_manager();
    }
    if (s_manager == NULL) {
        return false;       /* open_manager() said why */
    }

    rt_vector *sessions = NULL;
    HRESULT hr = s_manager->lpVtbl->GetSessions(s_manager, &sessions);

    if (FAILED(hr) || sessions == NULL) {
        /* Whatever went wrong, the kept manager is the first suspect: drop it
           so the next press starts again rather than failing for good. */
        manager_forget();
        note("%sthe media sessions could not be listed", NULL);
        return false;
    }

    UINT32 count = 0;
    sessions->lpVtbl->get_Size(sessions, &count);

    /* What is there, for the message when none of it is what was asked for:
       an application's session is not always named after its executable, and
       the only way to see the difference is to be shown both. */
    char found[120];
    size_t at = 0;
    bool done = false;

    found[0] = 0;

    for (UINT32 i = 0; i < count && !done; i++) {
        rt_session *session = NULL;

        if (FAILED(sessions->lpVtbl->GetAt(sessions, i, &session)) ||
            session == NULL) {
            continue;
        }

        HSTRING id = NULL;
        char label[MEDIA_NAME_MAX * 2];

        label[0] = 0;
        if (SUCCEEDED(session->lpVtbl->get_SourceAppUserModelId(session, &id))) {
            from_hstring(id, label, sizeof(label));
            WindowsDeleteString(id);
        }

        if (label[0] != 0 && at + strlen(label) + 3 < sizeof(found)) {
            at += (size_t)snprintf(found + at, sizeof(found) - at, "%s%s",
                                   (at > 0) ? ", " : "", label);
        }

        if (session_is(label, app)) {
            rt_async *op = NULL;

            if (SUCCEEDED(session->lpVtbl->TryTogglePlayPauseAsync(session, &op)) &&
                op != NULL) {
                boolean taken = 0;
                HRESULT result = rt_await(op, &taken);

                RT_RELEASE(op);
                done = SUCCEEDED(result) && taken;

                if (!done) {
                    note("%s has a media session but would not take the "
                         "command", app);
                }
            } else {
                note("%s would not take the command", app);
            }
        }

        RT_RELEASE(session);
    }

    RT_RELEASE(sessions);

    if (!done && s_error[0] == 0) {
        /*
         * Three different situations, and the difference is the whole of what
         * the user needs: the program is not started; it is started but tells
         * Windows nothing, so nobody can address it; or it simply is not the
         * one that is playing.
         */
        if (process_running(app)) {
            snprintf(s_error, sizeof(s_error),
                     "%.40s is running but publishes no media session, so it "
                     "cannot be addressed on its own", app);
            s_failures++;
        } else if (found[0] != 0) {
            snprintf(s_error, sizeof(s_error),
                     "%.40s is not running; what is playing: %.110s",
                     app, found);
            s_failures++;
        } else {
            note("%snothing on this machine is playing", NULL);
        }
    }
    return done;
}

/* ========================================================================== */
#else
/* ========================================================================== */

#include <ctype.h>
#include <stdlib.h>

/*
 * MPRIS names every player org.mpris.MediaPlayer2.<something>, where the
 * something is usually the executable but not always: Chromium publishes
 * "chromium.instance1234" and Firefox "firefox.instance_1_2". So the bus is
 * asked what is there, and the part before the first dot is what gets matched.
 */
#define MPRIS_PREFIX "org.mpris.MediaPlayer2."

/* Runs @p command and hands back what it printed. */
static bool run(const char *command, char *out, size_t size)
{
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        return false;
    }

    size_t at = 0;
    int c;

    while ((c = fgetc(pipe)) != EOF && at + 1 < size) {
        out[at++] = (char)c;
    }
    out[at] = 0;

    /* A non-zero status means dbus-send itself refused, which is worth
       knowing: the player may simply not be there. */
    return pclose(pipe) == 0;
}

/*
 * The bus name of the player belonging to @p app.
 *
 * @return false when no player on the bus answers to that name.
 */
static bool player_for(const char *app, char *out, size_t size)
{
    static char names[16384];

    if (!run("dbus-send --session --dest=org.freedesktop.DBus --type=method_call"
             " --print-reply /org/freedesktop/DBus"
             " org.freedesktop.DBus.ListNames 2>/dev/null",
             names, sizeof(names))) {
        return false;
    }

    /* Every name comes back as a line reading: string "org.mpris..." */
    for (char *at = strstr(names, MPRIS_PREFIX); at != NULL;
         at = strstr(at + 1, MPRIS_PREFIX)) {

        char full[256];
        size_t len = 0;

        while (len + 1 < sizeof(full) && at[len] != 0 &&
               at[len] != '"' && at[len] != '\n') {
            full[len] = at[len];
            len++;
        }
        full[len] = 0;

        /* "chromium.instance1234" is chromium; the suffix is the process. */
        char player[128];
        snprintf(player, sizeof(player), "%s", full + strlen(MPRIS_PREFIX));

        char *dot = strchr(player, '.');
        if (dot != NULL) {
            *dot = 0;
        }

        if (appname_same(player, app)) {
            snprintf(out, size, "%s", full);
            return true;
        }
    }
    return false;
}

bool media_available(void)
{
    /* Nothing to open, but without a session bus there is nobody to ask. */
    return getenv("DBUS_SESSION_BUS_ADDRESS") != NULL ||
           getenv("XDG_RUNTIME_DIR") != NULL;
}

bool media_toggle(const char *app)
{
    s_error[0] = 0;

    if (app == NULL || app[0] == 0) {
        note("%sno application was named", NULL);
        return false;
    }

    if (!media_available()) {
        note("%sthere is no session bus to ask", NULL);
        return false;
    }

    char player[256];
    if (!player_for(app, player, sizeof(player))) {
        note("%s is not playing anything the desktop knows about", app);
        return false;
    }

    char command[512];
    snprintf(command, sizeof(command),
             "dbus-send --session --dest=%s --type=method_call"
             " /org/mpris/MediaPlayer2"
             " org.mpris.MediaPlayer2.Player.PlayPause 2>/dev/null",
             player);

    char reply[256];
    if (!run(command, reply, sizeof(reply))) {
        note("%s would not take the command", app);
        return false;
    }
    return true;
}

#endif /* _WIN32 */
