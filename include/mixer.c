#include "mixer.h"

#include <stdio.h>
#include <string.h>

/* Compare basenames without case or extension, so one configuration matches
   "Discord.exe" on Windows and "discord" on Linux. */
static bool name_matches(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    for (;;) {
        char ca = *a;
        char cb = *b;

        bool a_end = (ca == 0 || ca == '.');
        bool b_end = (cb == 0 || cb == '.');
        if (a_end || b_end) {
            return a_end && b_end;
        }

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }

        a++;
        b++;
    }
}

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/*
 * Why the last attempt failed. "Volume control disabled" on its own leaves no
 * way to tell a build without PulseAudio from a session the program cannot
 * reach, which are fixed in completely different ways.
 */
static char s_error[256] = "not initialised";

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
}

const char *mixer_last_error(void)
{
    return s_error;
}

/* ========================================================================== */
#ifdef _WIN32
/* ========================================================================== */

/* COBJMACROS gives the C call form; initguid.h turns the interface headers'
   DEFINE_GUID declarations into actual symbols, and must appear exactly once. */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

static bool s_ready;
static bool s_owns_com;

/*
 * Volume changes arrive continuously while a fader moves, and rebuilding the
 * whole session graph for each one is far too slow to keep a frame budget.
 * The per-application volume interfaces are therefore held open and only
 * rediscovered occasionally.
 */
#define CACHE_TTL_MS 2000

typedef struct {
    char name[MIXER_NAME_MAX];
    ISimpleAudioVolume *volume;
} cached_session_t;

static cached_session_t s_cache[MIXER_MAX_SESSIONS];
static int   s_cache_count;
static DWORD s_cache_stamp;
static bool  s_cache_valid;

static void cache_clear(void);

bool mixer_init(void)
{
    if (s_ready) {
        return true;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        s_owns_com = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        /* RPC_E_CHANGED_MODE only means somebody else already picked a model,
           which is fine; anything else is fatal. */
        char why[96];
        snprintf(why, sizeof(why), "COM initialisation failed (0x%08lx)",
                 (unsigned long)hr);
        set_error(why);
        fprintf(stderr, "mixer: %s\n", why);
        return false;
    }

    s_ready = true;
    return true;
}

void mixer_shutdown(void)
{
    cache_clear();

    if (s_owns_com) {
        CoUninitialize();
        s_owns_com = false;
    }
    s_ready = false;
}

bool mixer_available(void)
{
    return s_ready;
}

/* Executable basename for a session's owning process. */
static bool process_name_of(DWORD pid, char *out, size_t cap)
{
    out[0] = 0;

    if (pid == 0) {
        snprintf(out, cap, "System");
        return true;
    }

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == NULL) {
        return false;
    }

    char path[MAX_PATH];
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameA(proc, 0, path, &len) != 0;
    CloseHandle(proc);

    if (!ok) {
        return false;
    }

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == 0x5C || *p == '/') {
            base = p + 1;
        }
    }
    snprintf(out, cap, "%s", base);
    return true;
}

static void cache_clear(void)
{
    for (int i = 0; i < s_cache_count; i++) {
        if (s_cache[i].volume) {
            ISimpleAudioVolume_Release(s_cache[i].volume);
        }
    }
    s_cache_count = 0;
    s_cache_valid = false;
}

/* Take a reference on every session's volume interface and keep it. */
static void cache_build(void)
{
    cache_clear();

    if (!s_ready) {
        return;
    }

    IMMDeviceEnumerator *devices = NULL;
    IMMDevice *endpoint = NULL;
    IAudioSessionManager2 *manager = NULL;
    IAudioSessionEnumerator *sessions = NULL;

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&devices))) {
        goto done;
    }
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, eRender,
                                                           eConsole, &endpoint))) {
        goto done;
    }
    if (FAILED(IMMDevice_Activate(endpoint, &IID_IAudioSessionManager2, CLSCTX_ALL,
                                  NULL, (void **)&manager))) {
        goto done;
    }
    if (FAILED(IAudioSessionManager2_GetSessionEnumerator(manager, &sessions))) {
        goto done;
    }

    int count = 0;
    if (FAILED(IAudioSessionEnumerator_GetCount(sessions, &count))) {
        goto done;
    }

    for (int i = 0; i < count && s_cache_count < MIXER_MAX_SESSIONS; i++) {
        IAudioSessionControl *control = NULL;
        IAudioSessionControl2 *control2 = NULL;
        ISimpleAudioVolume *volume = NULL;
        DWORD pid = 0;

        if (FAILED(IAudioSessionEnumerator_GetSession(sessions, i, &control))) {
            continue;
        }
        if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                control, &IID_IAudioSessionControl2, (void **)&control2))) {
            IAudioSessionControl2_GetProcessId(control2, &pid);
            IAudioSessionControl2_Release(control2);
        }

        if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                control, &IID_ISimpleAudioVolume, (void **)&volume))) {

            cached_session_t *entry = &s_cache[s_cache_count];
            if (!process_name_of(pid, entry->name, sizeof(entry->name))) {
                snprintf(entry->name, sizeof(entry->name), "pid %lu",
                         (unsigned long)pid);
            }
            entry->volume = volume;     /* reference kept until cache_clear */
            s_cache_count++;
        }

        IAudioSessionControl_Release(control);
    }

    s_cache_stamp = GetTickCount();
    s_cache_valid = true;

done:
    if (sessions) IAudioSessionEnumerator_Release(sessions);
    if (manager)  IAudioSessionManager2_Release(manager);
    if (endpoint) IMMDevice_Release(endpoint);
    if (devices)  IMMDeviceEnumerator_Release(devices);
}

static void cache_ensure(bool force)
{
    if (force || !s_cache_valid) {
        cache_build();
    }
}

/*
 * Whether a failed lookup has earned a rebuild.
 *
 * Sessions appear and disappear, so a miss may mean the cache is out of date
 * rather than that the application is silent. Finding out costs a full
 * enumeration, so it is done at most once per CACHE_TTL_MS: a fader moving
 * against an idle application then costs one rebuild every couple of seconds
 * instead of one per frame. Rebuilding on a timer instead would put that cost
 * into a frame even when every lookup is hitting.
 */
static bool miss_rebuild_due(void)
{
    DWORD now = GetTickCount();

    if (s_cache_valid && (now - s_cache_stamp) < CACHE_TTL_MS) {
        return false;
    }
    return true;
}

bool mixer_set_volume(const char *process, float volume)
{
    if (!s_ready) {
        return false;
    }

    float level = clamp01(volume);

    for (int attempt = 0; attempt < 2; attempt++) {
        cache_ensure(false);

        int hits = 0;
        bool stale = false;

        for (int i = 0; i < s_cache_count; i++) {
            if (!name_matches(s_cache[i].name, process)) {
                continue;
            }
            if (FAILED(ISimpleAudioVolume_SetMasterVolume(s_cache[i].volume,
                                                          level, NULL))) {
                stale = true;   /* the session died under us */
                break;
            }
            hits++;
        }

        if (stale) {
            /* A dead handle makes the rest of the cache suspect too, so it is
               rebuilt and the write retried once. */
            cache_clear();
            continue;
        }

        if (hits > 0) {
            return true;
        }

        /*
         * Nothing matched, which is the normal state for an application that
         * is not playing anything: it has no session at all.
         *
         * This used to force a rebuild and then clear the cache, so a fader
         * assigned to an idle application cost two full session enumerations
         * per call and destroyed the cache every other application depended
         * on. Moving one fader was then tens of milliseconds a frame.
         */
        if (attempt == 0 && miss_rebuild_due()) {
            cache_clear();
            continue;
        }
        return false;
    }
    return false;
}

/* ========================================================================== */
#elif defined(MIXER_HAVE_PULSE)
/* ========================================================================== */

#include <pulse/pulseaudio.h>

#include <stdlib.h>
#include <unistd.h>

/*
 * A threaded mainloop is used so the caller never has to pump anything: every
 * entry point locks it, issues an operation and waits for that operation to
 * finish. That keeps the API synchronous, which is what a per-frame UI wants.
 */
static pa_threaded_mainloop *s_loop;
static pa_context *s_ctx;
static bool s_ready;

/* Scratch shared with the introspection callbacks while the loop is locked. */
static int   s_hits;
static const char *s_match;
static float s_apply;
static int   s_mute;
static float s_found;

static void context_state_cb(pa_context *c, void *userdata)
{
    (void)userdata;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        pa_threaded_mainloop_signal(s_loop, 0);
        break;
    default:
        break;
    }
}

/* Wait for an operation, then release it. */
static void wait_for(pa_operation *op)
{
    if (op == NULL) {
        return;
    }
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(s_loop);
    }
    pa_operation_unref(op);
}

/* Which executable a stream belongs to. */
static const char *binary_of(const pa_sink_input_info *info)
{
    const char *name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    if (name == NULL) {
        name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    }
    return name ? name : "unknown";
}

static void sink_input_cb(pa_context *c, const pa_sink_input_info *info,
                          int eol, void *userdata)
{
    (void)userdata;

    if (eol) {
        pa_threaded_mainloop_signal(s_loop, 0);
        return;
    }

    const char *binary = binary_of(info);
    float level = (float)pa_sw_volume_to_linear(pa_cvolume_avg(&info->volume));

    if (s_match == NULL || !name_matches(binary, s_match)) {
        return;
    }

    if (s_hits == 0) {
        s_found = level;
    }
    s_hits++;

    if (s_apply >= 0.0f) {
        pa_cvolume cv;
        pa_cvolume_set(&cv, info->volume.channels,
                       pa_sw_volume_from_linear((double)s_apply));
        pa_operation_unref(
            pa_context_set_sink_input_volume(c, info->index, &cv, NULL, NULL));
    }

    if (s_mute >= 0) {
        pa_operation_unref(
            pa_context_set_sink_input_mute(c, info->index, s_mute, NULL, NULL));
    }
}

/*
 * Both failure paths want the same detail: what libpulse itself said, plus
 * the two conditions that usually explain it on a working desktop.
 */
static void set_connect_error(const char *what)
{
    const char *why = s_ctx ? pa_strerror(pa_context_errno(s_ctx)) : "no context";
    char detail[256];

    if (geteuid() == 0) {
        snprintf(detail, sizeof(detail),
                 "%s (%s); root cannot reach the desktop session's audio "
                 "server, so use the udev rule instead of sudo", what, why);
    } else if (getenv("XDG_RUNTIME_DIR") == NULL) {
        snprintf(detail, sizeof(detail),
                 "%s (%s); XDG_RUNTIME_DIR is unset, so the session socket "
                 "cannot be located", what, why);
    } else {
        snprintf(detail, sizeof(detail),
                 "%s (%s); check that \"pactl info\" works as this user",
                 what, why);
    }

    set_error(detail);
    fprintf(stderr, "mixer: %s\n", s_error);
}

bool mixer_init(void)
{
    if (s_ready) {
        return true;
    }

    s_loop = pa_threaded_mainloop_new();
    if (s_loop == NULL) {
        set_error("could not create the PulseAudio main loop");
        return false;
    }

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(s_loop);
    s_ctx = pa_context_new(api, "IOMeeter");
    if (s_ctx == NULL) {
        set_error("could not create the PulseAudio context");
        pa_threaded_mainloop_free(s_loop);
        s_loop = NULL;
        return false;
    }

    pa_context_set_state_callback(s_ctx, context_state_cb, NULL);

    if (pa_context_connect(s_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0 ||
        pa_threaded_mainloop_start(s_loop) < 0) {
        set_connect_error("could not reach the PulseAudio socket");
        mixer_shutdown();
        return false;
    }

    pa_threaded_mainloop_lock(s_loop);
    for (;;) {
        pa_context_state_t state = pa_context_get_state(s_ctx);
        if (state == PA_CONTEXT_READY) {
            s_ready = true;
            break;
        }
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            break;
        }
        pa_threaded_mainloop_wait(s_loop);
    }
    pa_threaded_mainloop_unlock(s_loop);

    if (!s_ready) {
        set_connect_error("no PulseAudio or PipeWire server reachable");
        mixer_shutdown();
    }
    return s_ready;
}

void mixer_shutdown(void)
{
    if (s_loop) {
        pa_threaded_mainloop_stop(s_loop);
    }
    if (s_ctx) {
        pa_context_disconnect(s_ctx);
        pa_context_unref(s_ctx);
        s_ctx = NULL;
    }
    if (s_loop) {
        pa_threaded_mainloop_free(s_loop);
        s_loop = NULL;
    }
    s_ready = false;
}

bool mixer_available(void)
{
    return s_ready;
}

/* Shared entry: fills the scratch, runs one introspection pass, returns hits. */
static int run_pass(const char *match, float apply, int mute, float *found)
{
    if (!s_ready) {
        return 0;
    }

    pa_threaded_mainloop_lock(s_loop);

    s_hits = 0;
    s_match = match;
    s_apply = apply;
    s_mute = mute;
    s_found = -1.0f;

    wait_for(pa_context_get_sink_input_info_list(s_ctx, sink_input_cb, NULL));

    int hits = s_hits;
    if (found) {
        *found = s_found;
    }
    s_match = NULL;

    pa_threaded_mainloop_unlock(s_loop);
    return hits;
}

bool mixer_set_volume(const char *process, float volume)
{
    return run_pass(process, clamp01(volume), -1, NULL) > 0;
}

/* ========================================================================== */
#else
/* ========================================================================== */

/*
 * Built without libpulse. Everything reports unavailable rather than failing,
 * so the interface still runs; only volume control is missing.
 */

bool mixer_init(void)
{
    set_error("built without PulseAudio support; install libpulse-dev, "
              "then ./build.sh clean");
    fprintf(stderr, "mixer: %s\n", s_error);
    return false;
}

void mixer_shutdown(void) {}
bool mixer_available(void) { return false; }

bool mixer_set_volume(const char *process, float volume)
{
    (void)process;
    (void)volume;
    return false;
}

#endif
