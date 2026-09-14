# host/c audit — 2026-09-09

Bug hunt across the host application. 13,317 lines, 23 translation units.
Six defects found and fixed, four of them capable of losing user data or
hanging the program. Every fix is verified by a test that fails against the
old code.

---

## Method

Five passes, cheapest first.

1. **Inventory.** Sorted every unit by size to aim the reading. The three
   largest (`ui.c` 3400, `audio_endpoint.c` 1055, `keys.c` 810) got line-level
   reading; the rest were read whole.
2. **Targeted reading** of the highest-risk categories, in this order:
   concurrency and shutdown, resource lifetime, parsing and buffers, then UI
   state. Rare bugs cluster in the first two, so they went first.
3. **Grep sweeps** for classic patterns: `strcpy`/`strcat`/`sprintf`, every
   `malloc` against its NULL check, every `fopen` against its `fclose`, COM
   `Release` balance, PulseAudio lock/unlock balance, array indexing from
   external input.
4. **Extended warning build.** `-Wshadow -Wpointer-arith -Wnull-dereference
   -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wjump-misses-init
   -Wwrite-strings -Wredundant-decls -Wold-style-definition` on top of
   `-Wall -Wextra`, at `-O0` and `-O2` (optimisation enables the range
   analysis that finds truncation and uninitialised reads).
5. **Invariant fuzzing.** No sanitizer exists for this MinGW toolchain
   (`-lasan` and `-lubsan` are both absent), so instead of waiting for a crash
   the harness asserts the invariants the rest of the program relies on —
   bounds, counts, NUL termination — after every round. 140,000 rounds across
   the framer, the JSON loader, the model operations and the key table.

### A note on the fuzzer

Its first run reported a clean pass that meant nothing. `unsigned long` is
32 bits on this toolchain, so the generator's `s_seed >> 33` was undefined and
returned zero every time: every "random" draw picked element 0, and the framer
was fed nothing but `{`. It framed **0 messages** and still reported success.

The tell was that counter, not the result. After widening the seed to
`unsigned long long` the same harness framed 3,046 messages. Any fuzzer that
reports a pass without also reporting coverage is worth distrusting.

---

## Findings

Severity is about consequence, not likelihood.

| # | Severity | Where | What |
|---|---|---|---|
| 1 | **Critical** | `config.c` | The whole configuration is silently lost once the file passes 8 KB |
| 2 | **High** | `usbdev.c`, `app.c` | Received USB bytes are discarded on a timeout |
| 3 | **High** | `keysend.c` | Shutdown blocks on a macro's timings; modifier keys can be left held down system-wide |
| 4 | Medium | `volume.c` | A lock is destroyed while the worker may still be inside it |
| 5 | Medium | `config.c` | A failed save destroys the previous file |
| 6 | Low | `app.c`, `config.c`, `fader.c` | Fader positions from the device and from disk are not range-checked |

---

### 1 — The configuration is lost once it exceeds 8 KB

**`config.c`, `read_file()`** — critical, and reachable with an ordinary setup.

```c
#define FILE_MAX 8192
char *text = malloc(FILE_MAX);
size_t len = fread(text, 1, FILE_MAX - 1, f);
```

The file is read through a fixed 8,191-byte window. Anything longer arrives at
`cJSON_Parse` truncated mid-token, which fails. `config_load` then returns
false — and by that point `config_defaults()` has already overwritten the
caller's state. `app_config_load` treats that as "no configuration yet",
marks the state dirty, and the next save writes the defaults over the real
file.

The failure is silent, and it destroys the original.

**Why it is reachable.** The file has grown with the feature set. Thirty keys
across eight profiles, each carrying a name, a colour and a macro, plus four
faders with their application lists. Measured with a deliberately modest
setup — 4 faders × 6 applications, 3 profiles, 15 keys bound:

```
compact (debug=false)  8502 bytes   over the limit
pretty  (debug=true)  11585 bytes   over the limit
reload compact: FAILED -- configuration lost
reload pretty : FAILED -- configuration lost
```

`debug=true` makes it markedly worse: indented output roughly doubles the
size, so switching on the traffic console can be what pushes a working
configuration over the edge.

**Fix.** Size the allocation from the file rather than from a constant.
`FILE_MAX` stays as a sanity ceiling against reading something that is not a
configuration at all, raised to 4 MB.

```c
fseek(f, 0, SEEK_END);
long size = ftell(f);
if (size < 0 || size > FILE_MAX || fseek(f, 0, SEEK_SET) != 0) { ... }
char *text = malloc((size_t)size + 1);
size_t len = fread(text, 1, (size_t)size, f);
text[len] = '\0';
```

**Verified.** A 19,011-byte configuration now round-trips with its profiles,
bindings and application lists intact.

---

### 2 — Received USB bytes are thrown away on a timeout

**`usbdev.c`, `usbdev_recv()`** — high.

```c
*out_len = (rc == 0) ? transferred : 0;
```

libusb splits a transfer into chunks to suit the operating system, and the
deadline can pass after some of them have already landed. Its documentation is
explicit: *"libusb is careful not to lose any data that may have been
transferred; do not assume that timeout conditions indicate a complete lack of
I/O."*

`app_poll` compounds it — the timeout is checked *before* the data is used:

```c
if (rc == LIBUSB_ERROR_TIMEOUT) {
    return;                      /* nothing waiting */
}
...
if (len > 0) { proto_framer_push(...); }   /* never reached */
```

**Why it matters here.** `POLL_TIMEOUT_MS` is 1. A one-millisecond deadline on
a bulk endpoint expires mid-transfer routinely, not rarely. Each time, the
front of a message is discarded; the framer then holds an incomplete document
waiting for a remainder that was thrown away, and blocks every later message
behind it until the 2 KB buffer fills and resets. The visible symptom is
occasional dropped or delayed device traffic — exactly the kind of flakiness
that looks like a hardware problem.

**Fix.** Report what arrived whatever the result, and consume it before acting
on the code.

```c
*out_len = (transferred > 0) ? transferred : 0;
```

```c
if (len > 0) {
    proto_framer_push(&a->framer, buf, len, app_on_message, a);
}
if (rc == LIBUSB_ERROR_TIMEOUT) {
    return;
}
```

The header now documents that bytes can arrive alongside a timeout.

---

### 3 — Shutdown hangs on a macro, and can leave keys held down

**`keysend.c`, `play()` and `keysend_stop()`** — high.

`play()` sleeps for each step's timing in one uninterruptible call:

```c
if (job->timings[i] > 0) {
    nap(job->timings[i]);
}
```

Timings were raised to 10,000 ms across up to 16 steps, so one macro can ask
for **160 seconds** of waiting. Three consequences, all of them real:

- **POSIX:** `keysend_stop()` calls `pthread_join`, which waits for the whole
  macro. Closing the window hangs the program for up to two and a half
  minutes.
- **Windows:** the 3-second `WaitForSingleObject` times out, and the code then
  runs `CloseHandle` and `DeleteCriticalSection` unconditionally — deleting a
  lock the worker is about to enter. Undefined behaviour at exit.
- **Both, and the nastiest:** a chord is pressed step by step and released only
  at the end. If the process exits mid-chord, `Ctrl` (or `Alt`, or `Shift`)
  stays physically down **for the whole desktop**, with the program that
  pressed it gone. The user has to press and release the key by hand to
  recover.

**Fix.** Wait in 20 ms slices that check the shutdown flag, and always run the
release sweep:

```c
static bool nap_interruptible(int ms)
{
    while (ms > 0) {
        if (!s_running) return false;
        int slice = (ms > KEYSEND_SLICE_MS) ? KEYSEND_SLICE_MS : ms;
        nap(slice);
        ms -= slice;
    }
    return s_running;
}
```

```c
if (!nap_interruptible(job->timings[i])) {
    break;              /* release what is already down, below */
}
```

`keysend_stop()` now only closes the handle and destroys the lock when the
thread actually joined. If it somehow did not, both are leaked deliberately —
leaking a critical section at exit beats deleting one that is still in use.

**Verified.** A 16-step macro asking for 128 seconds, queued and then
interrupted: `keysend_stop()` returns in **0.00 s**. The test uses key names
deliberately absent from the table, so the timings run in full but no
keystroke is ever injected into the focused window.

---

### 4 — The mixer lock is destroyed while the worker may hold it

**`volume.c`, `volume_stop()`** — medium, same shape as #3.

```c
WaitForSingleObject(s_thread, 5000);
CloseHandle(s_thread);
...
lock_destroy();
```

A mixer call taking longer than five seconds is precisely the case this file
exists to isolate — the reason the mixer runs on a worker at all. When the wait
times out, the lock is deleted underneath a live thread.

A second, quieter path: `volume_stop()` never cleared `s_available`, so
`volume_available()` kept returning true afterwards. `app_apply_volume` checks
that flag and would go on to call `volume_matched()`, which locks a critical
section that no longer exists.

**Fix.** Clear `s_available` first, so nothing else calls in during the wind
down; destroy the lock only on a successful join; and make `volume_matched()`
refuse to lock unless the worker is running.

---

### 5 — A failed save destroys the previous configuration

**`config.c`, `config_save()`** — medium.

`fopen(path, "wb")` truncates the real file before a single byte is written. A
full disk, a removed drive, or the process being killed mid-write leaves the
only copy truncated — and a truncated file then fails to parse, which lands
straight back in finding #1.

This got sharper when saving became deliberate rather than continuous: the file
is now written when the window goes to the tray or from the menu, so a lost
save is a lost session's work rather than one second's.

**Fix.** Write to `<path>.tmp`, check both `ferror` and the result of `fclose`,
and only then move it into place. On failure the temporary is removed and the
original is untouched.

`rename()` will not replace an existing file on every platform, so the old file
is removed first. That leaves a brief window with no file at the target name —
but the data is safe in the temporary throughout, which the previous code could
not say at any point.

---

### 6 — Fader positions are not range-checked

**`app.c`, `config.c`, `fader.c`** — low, but it persists.

The inbound handler checks the slider *id* and then trusts the value:

```c
if (cJSON_IsNumber(id) && id->valueint >= 0 && id->valueint < APP_FADER_COUNT) {
    a->sliders[id->valueint].value = value->valueint;   /* unbounded */
}
```

`slider_to_gain()` clamps, so the volume itself is safe. Nothing else does.
`fader_draw` computes `t = value / max` without limit, so a value of 999999
puts the knob thousands of pixels off the window and feeds that figure into the
easing state, where it persists across frames. The bad value is then written
into config.json and reloaded on every subsequent start.

**Fix.** Clamp at all three points where a value can enter: from the device, from
the file, and defensively in `fader_draw` — the easing keeps whatever it is
given, so one bad frame would otherwise stick.

**Verified.** `999999` reads back as 4095, `-4000` as 0.

---

## Checked and found sound

Worth recording so the same ground is not covered twice.

- **`proto.c` framing.** 40,000 fuzz rounds of malformed and glued JSON,
  embedded NULs, braces inside strings, escaped quotes, and bursts larger than
  the buffer. The buffer never overran, every message was NUL-terminated as the
  contract promises, and a full buffer recovers rather than wedging.
- **`keys.c` model and loader.** 80,000 rounds of random operation sequences
  and malformed documents against invariants: profile count in range, active
  profile in range, at most one default, app counts bounded, macro counts
  bounded, timings inside the accepted range, every fixed buffer terminated.
  All held.
- **Memory safety sweep.** No `strcpy`, `sprintf` or `gets` anywhere. The two
  `strcat` sites (`jsoncmd.c`, `ui.c` log copy) are correctly pre-sized. Every
  `malloc` is NULL-checked; every `fopen` has its `fclose` on all paths.
- **COM lifetime in `mixer.c`.** `control`, `control2` and `volume` are
  declared and cleared *inside* the enumeration loop, so the double-release
  that shape usually hides is not present. Release calls balance on every path
  including the `goto done` ones.
- **`ui.c` focus handling.** `s_focus` and `s_focus_cap` are set together at
  every one of the sixteen assignment sites; the input pump's bound
  (`len + 1 < cap`) is correct. No path leaves the pointer dangling — removals
  shift within a live structure rather than freeing it.
- **`fader.c` indexing.** `smoothed()` bounds-checks the id; `fader_interact`
  indexes nothing.
- **`watchdog.c`.** Sleeps in 500 ms slices, so its 2-second join is safe. Not
  affected by the problem in #3 and #4.
- **`instance.c`.** Both the Windows named-event and the POSIX socket paths
  handle the stale-lock and simultaneous-launch cases correctly.

## Left alone, worth knowing

- **`audio_endpoint.c` is dead code.** 1,055 lines, compiled into the binary,
  referenced from nowhere — no call site in any source file or in
  `CMakeLists.txt` beyond the build entry itself. Its PulseAudio lock/unlock
  counts are also asymmetric (3 locks, 6 unlocks), which is a latent trap for
  whoever wires it up. Not removed: deleting a thousand lines is a decision to
  take deliberately, not as a side effect of a bug hunt. Either delete it or
  fix the balance before using it.
- **`mixer.c:164` truncation warning.** `snprintf(out, cap, "%s", base)` cuts a
  path basename into a 64-byte buffer. The truncation is intentional and the
  code is correct; `-Wformat-truncation` cannot tell deliberate from accidental.
  Appears only at `-O2`. One line (`"%.*s"` with the cap) silences it.
- **`config.c` `remove()` before `rename()`.** See #5 — a brief window where the
  target name does not exist. `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`
  would close it on Windows at the cost of a platform branch in an otherwise
  portable file.

---

## Verification

Everything below passes against the fixed tree.

| Check | Result |
|---|---|
| 23 units, `-Wall -Wextra`, `-O0` and `-O2` | clean (`mixer.c:164` excepted, pre-existing) |
| Extended warning set | clean, vendored `clay.h`/`cJSON.c` aside |
| POSIX branches via stub headers — `keysend` (XTEST and stub), `foreground` (X11 and stub), `volume` | clean |
| `test_profiles` — profiles, matching, round trip | pass |
| `test_frame` — framer, 13 checks | pass |
| `test_macro2` — timings, text macros, fallback profile | pass |
| `test_suffix` — executable-suffix handling | pass |
| Fuzzer — 140,000 rounds, four surfaces | all invariants held |
| `verify_fixes` — one test per finding | 17/17 |

Not built or flashed, per the standing rule; all checks are compile and unit
level.

### Reproducing

Harnesses are in the session scratchpad, not the repository:
`fuzz.c`, `verify_fixes.c`, `measure_config.c`, `test_frame.c`,
`test_macro2.c`, `test_profiles.c`, `test_suffix.c`.

```sh
gcc -O2 -Wall -Wextra -std=c11 -Iinclude -Ilibs -o vf.exe verify_fixes.c \
    include/config.c include/keys.c include/keysend.c libs/cJSON.c && ./vf.exe
```
