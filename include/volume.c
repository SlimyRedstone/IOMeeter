#include "volume.h"

#include <stdio.h>
#include <string.h>

#include "mixer.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* One pending position per fader, overwritten rather than queued. */
typedef struct {
    char  names[VOLUME_APPS][VOLUME_NAME_MAX];
    int   count;
    float gain;
    bool  pending;
    bool  matched;
} slot_t;

static slot_t s_slots[VOLUME_SLIDERS];

static volatile bool s_running;
static volatile bool s_ready;        /* worker finished mixer_init()      */
static volatile bool s_available;    /* what mixer_init() reported        */

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

/* -------------------------------------------------------------- worker --- */

/*
 * Owns the mixer for its whole life: connects, applies, disconnects. COM
 * apartments belong to a thread, so none of that may happen anywhere else.
 */
static void worker_loop(void)
{
    s_available = mixer_init();
    s_ready = true;

    while (s_running) {
        slot_t work;
        int which = -1;

        lock();
        for (int i = 0; i < VOLUME_SLIDERS; i++) {
            if (s_slots[i].pending) {
                work = s_slots[i];
                s_slots[i].pending = false;
                which = i;
                break;
            }
        }
        unlock();

        if (which < 0) {
            nap(5);         /* nothing to do; stay cheap */
            continue;
        }

        if (!s_available) {
            continue;
        }

        /* Outside the lock: this is the call that can take seconds, and the
           interface must never wait behind it. */
        int matched = 0;
        for (int i = 0; i < work.count; i++) {
            if (mixer_set_volume(work.names[i], work.gain)) {
                matched++;
            }
        }

        lock();
        s_slots[which].matched = (work.count == 0) || (matched > 0);
        unlock();
    }

    mixer_shutdown();
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

/* --------------------------------------------------------------- api ----- */

bool volume_start(void)
{
    if (s_running) {
        return s_available;
    }

    lock_init();

    for (int i = 0; i < VOLUME_SLIDERS; i++) {
        s_slots[i].matched = true;      /* nothing applied yet, so no warning */
    }

    s_running = true;
    s_ready = false;
    s_available = false;

#ifdef _WIN32
    s_thread = CreateThread(NULL, 0, worker_entry, NULL, 0, NULL);
    bool started = (s_thread != NULL);
#else
    bool started = (pthread_create(&s_thread, NULL, worker_entry, NULL) == 0);
    s_thread_valid = started;
#endif

    if (!started) {
        s_running = false;
        lock_destroy();
        return false;
    }

    /*
     * Only the connection is waited for, so the caller can report whether it
     * worked. Bounded, because a mixer that never answers must not stop the
     * program from starting.
     */
    for (int waited = 0; !s_ready && waited < 5000; waited += 10) {
        nap(10);
    }
    return s_available;
}

void volume_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;

    /* Cleared before the wait: it is what stops anything else calling in here
       while the worker is on its way out. */
    s_available = false;

#ifdef _WIN32
    /*
     * A mixer call is exactly the thing that can take longer than this, which
     * is why it lives on a thread at all. If the wait does time out, the
     * handle and the critical section are left in place on purpose: deleting a
     * lock the worker is still using would turn a slow exit into a crash.
     */
    if (WaitForSingleObject(s_thread, 5000) == WAIT_OBJECT_0) {
        CloseHandle(s_thread);
        s_thread = NULL;
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

bool volume_available(void)
{
    return s_available;
}

const char *volume_last_error(void)
{
    /* Written by the worker during mixer_init(), which volume_start() waits
       for, so by the time anyone asks it is settled. */
    return mixer_last_error();
}

void volume_set(int slider, const char *const *names, int count, float gain)
{
    if (slider < 0 || slider >= VOLUME_SLIDERS || !s_running) {
        return;
    }
    if (count > VOLUME_APPS) {
        count = VOLUME_APPS;
    }
    if (count < 0) {
        count = 0;
    }

    lock();

    for (int i = 0; i < count; i++) {
        snprintf(s_slots[slider].names[i], VOLUME_NAME_MAX, "%s",
                 names[i] ? names[i] : "");
    }
    s_slots[slider].count = count;
    s_slots[slider].gain = gain;
    s_slots[slider].pending = true;

    unlock();
}

bool volume_matched(int slider)
{
    /* The lock only exists between volume_start() and volume_stop(); taking it
       outside that is reading a critical section that has been deleted. */
    if (slider < 0 || slider >= VOLUME_SLIDERS || !s_running) {
        return true;
    }

    lock();
    bool matched = s_slots[slider].matched;
    unlock();
    return matched;
}
