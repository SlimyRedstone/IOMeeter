# How the client is put together

Every library in this folder is ours; `../libs/` holds the vendored ones (Clay,
its raylib renderer, cJSON). `main.c` only wires these together — it holds
initialisation and declarations, and no behaviour of its own.

Two things are worth explaining at length rather than in a line each, and they
are at the bottom: how a USB packet actually leaves the program, and how one
application's volume is changed without moving the system volume. The rest is a
sentence apiece.

## The libraries

### Talking to the device

| File | What it does |
| --- | --- |
| `usbdev` | libusb-1.0 wrapper: finds the device, claims the vendor interface, and moves bytes over the two bulk endpoints. Knows the controller (`303A:6901`) and falls back to the dongle (`303A:6902`). |
| `proto` | Turns the IN endpoint's byte stream back into whole JSON documents. The endpoint is a stream, not a message queue, so replies arrive glued together or split across packets; this is what re-frames them. Built on cJSON, and it hands the parsed document straight on rather than parsing twice. |
| `devcfg` | Moves the desktop configuration to and from the controller, a slice at a time. The document is far larger than one packet, so it has a small state machine of its own: announce the size, append until it is all there, commit. Explained below. |
| `jsoncmd` | Builds the JSON a command line asks for. The shorthands exist because `cmd.exe` mangles quotes, so `iometer led ff0000` has to work without any. |

### State and what is kept

| File | What it does |
| --- | --- |
| `app` | The state the interface and the transport share: the connection, the current colour, the fader positions, the log, the battery reading. Everything else reads it; nothing else owns it. |
| `config` | `config.json`: the fader strip, the applications each fader mixes, and the `debug` flag. Scans the JSON by hand rather than linking a parser into every build. Writes to a temporary file and renames, so a crash mid-save cannot leave a half-written configuration. |
| `keys` | The macro pad half of the same file: thirty keys, each with a name, colour and macro **per profile**, plus the profiles themselves and the applications that select them. |
| `fonts` | Type sizes for the whole interface. Definitions only — nothing here allocates, loads or measures. |
| `appname` | What it means for two names to be the same program. Four places compare what somebody typed against what is running — a fader, a profile, a media key and a macro's target — and they have to agree, so the rule lives here once: ignore the path, ignore the case, ignore an executable suffix, but keep a version (`python3.11` is not `python3`). |

### Sound

| File | What it does |
| --- | --- |
| `mixer` | Per-application volume, the same thing the Windows mixer does. One API, two implementations: Core Audio sessions on Windows, PulseAudio sink inputs elsewhere. Explained below. |
| `media` | Starting and stopping **one** application's playback. A media key is global and lands on whichever program played last, so this addresses the application instead: its media session on Windows, its MPRIS interface on Linux. Explained below. |
| `volume` | Applies fader positions **off the interface's thread**. A mixer call can block for longer than a frame, and doing it inline froze the window. The worker owns its COM apartment from start to finish, because apartments are per-thread. |

### Input and applications

| File | What it does |
| --- | --- |
| `keysend` | Synthesised keyboard input for the macro pad: `SendInput` on Windows, XTEST on X11. A macro is a chord of named keys, a phrase typed out, or either of those **aimed at one application** rather than at the desktop. Waits in short slices so shutting down never has to sit through a macro's own timings, and always runs its release sweep — a modifier left held down is held for the whole desktop. |
| `foreground` | Which desktop applications are running, which one is in front, and which window belongs to a named one — the last of those is how a macro reaches an application that is not in front. Windows that are cloaked or have no title are skipped, so background hosts like `COMsurrogate` never appear. Names come from the executable's version resource, so the list reads "Windows Terminal" rather than `WindowsTerminal.exe`. |

### The interface

| File | What it does |
| --- | --- |
| `ui` | The whole Clay + raylib front end: header, tab bar, fader strip, macro pad, profile manager, colour wheel, traffic console. |
| `fader` | The vertical fader widget: pill track, circular knob, and the smoothing applied to a moving value. |
| `window` | Showing and hiding the window. Separate because `raylib.h` and `windows.h` cannot share a translation unit — both define `Rectangle`, among others. |
| `tray` | Window icon and system tray. Kept apart for the same reason, and because minimising to the tray is what triggers a configuration save. |
| `filedialog` | The native file chooser, used to pick the executables a fader mixes. |
| `console` | The traffic console. The program is linked as a GUI application on Windows, so it starts with no console at all; this opens one on demand when `debug` is set. |

### Process plumbing

| File | What it does |
| --- | --- |
| `instance` | One running copy at a time. A second launch hands its arguments to the first and exits. |
| `watchdog` | Where the interface stopped responding. The main loop marks the phase it is in; a separate thread notices when a phase overruns and says which one. |
| `respath` | Finds the files that ship with the program. Opening `resources/<name>` only worked when the working directory happened to be right. |
| `cli` | Console mode: everything the client does without opening a window. |

---

## How a packet is sent

The device exposes a **vendor-specific interface** with two bulk endpoints
alongside its CDC serial function. The CDC half is left to the operating
system, which is why a terminal can stay open on the COM port while the client
is running.

**Getting hold of it.** `usbdev_open()` walks the bus with
`libusb_get_device_list`, prefers the controller and falls back to the dongle,
then looks through the configuration descriptor for an interface of class
`0xFF` carrying one bulk IN and one bulk OUT. On Windows that interface needs
WinUSB bound to it; the firmware's MS OS 2.0 descriptors arrange that
automatically, so nothing has to be installed by hand.

**Going out.** A command is a single JSON object — `{"set":{"led":"FF0000"}}`
— handed to `usbdev_send()`, which is one `libusb_bulk_transfer` on the OUT
endpoint. There is no length prefix and no framing of our own on the way out.
Documents larger than the 64-byte endpoint simply span several packets, and the
firmware puts them back together: it appends each packet to a buffer and tries
to parse it, and a buffer that parses is a complete document. A packet shorter
than the endpoint size ends the USB transfer, so if the buffer still does not
parse at that point it never will, and the device reports the error instead of
waiting for bytes that will not come.

**Coming back.** The IN endpoint is a **byte stream**, not a queue of messages.
Two replies can arrive in one transfer, and one reply can be split across two,
so `app_poll()` pushes whatever arrives into `proto_framer_push()`, which
accumulates until a whole JSON document is present and only then calls back.

Two details in the receive path are easy to get wrong, and both were:

- libusb can return **`LIBUSB_ERROR_TIMEOUT` with data already in the buffer**.
  The poll timeout is 1 ms, so this happens constantly. The bytes must be taken
  before the return code is looked at, or the front of a message is thrown away
  and everything behind it stalls.
- The transfer length is only meaningful when the transfer moved something, so
  `*out_len` follows `transferred`, not the return code.

**Long documents.** The desktop client's own configuration is far larger than
one JSON buffer, so it moves in slices under the `host` command:

```
{"set":{"host":{"begin":8502}}}     open a transfer
{"set":{"host":{"text":"..."}}}     append, repeated
{"set":{"host":{"commit":true}}}    replace the stored document
{"get":"host"}                      -> {"host":{"size":8502}}
{"get":{"host":{"off":1024}}}       -> {"host":{"off":...,"text":...,"eof":...}}
```

The device writes the slices to a temporary file and renames it on commit, so
an interrupted transfer leaves the previous configuration in place, and a
commit that arrives with fewer bytes than were announced is refused outright.
Offsets are counted in **bytes**, not characters, and no slice ever ends inside
a UTF-8 sequence in either direction — one application path with an accent in
it is enough to matter.

---

## How one application's volume is changed

The point of the strip is that pulling fader 2 down turns Discord down and
leaves everything else alone. Neither platform lets you "set the volume of a
process" directly; both expose the same idea under different names, and
`mixer.c` presents one API over the two.

### Windows: Core Audio sessions

Every process that plays audio gets an **audio session** on the output device,
and each session has its own volume, independent of the device's master
volume — that is exactly what the Windows volume mixer shows.

The walk is:

1. `CoCreateInstance(CLSID_MMDeviceEnumerator)` for the enumerator.
2. `GetDefaultAudioEndpoint(eRender, eConsole)` for the output the user hears.
3. `IMMDevice::Activate(IID_IAudioSessionManager2)`, then
   `GetSessionEnumerator()` for every session on that endpoint.
4. For each session, `IAudioSessionControl2::GetProcessId()` says which process
   owns it. That process id is resolved to an executable name and matched
   against the names on the fader's list.
5. On a match, `ISimpleAudioVolume::SetMasterVolume()` sets **that session's**
   level, from 0.0 to 1.0. `SetMute()` handles muting.

`ISimpleAudioVolume` is per-session. The system volume lives behind
`IAudioEndpointVolume` on the endpoint itself, and is never touched.

Two consequences fall out of this design. A session only exists while the
process is actually playing something, so an application that has been silent
for a while disappears from the enumeration and reappears later — the fader has
to re-find it rather than hold a pointer. And one application can own several
sessions (browsers routinely do, one per tab or renderer), so every match is
applied, not just the first.

### Elsewhere: PulseAudio sink inputs

The same idea, different vocabulary. A **sink input** is one stream feeding an
output sink, and each carries its own volume. `mixer.c` runs a threaded main
loop, lists the sink inputs, and reads
`PA_PROP_APPLICATION_PROCESS_BINARY` from each one's property list to learn
which executable it belongs to. A match gets `pa_context_set_sink_input_volume`.
The sink's own volume — the system volume — is left alone.

### Why it is on its own thread

Both of these can block. Core Audio talks to the audio service, PulseAudio
talks to its daemon, and either can take longer than a frame; doing it inline
froze the window while a fader was moving. `volume.c` therefore owns a worker
that takes fader positions and applies them, and the interface never waits on
it.

The worker also owns its **COM apartment** from start to finish. COM
initialisation is per-thread, so a thread that did not call `CoInitializeEx`
itself cannot use interfaces another thread created — which is why the whole
init → calls → shutdown sequence lives inside the worker rather than being
split across it and `main`.

---

## How one application's playback is toggled

A fader changes how loud something is. A pad key can also start and stop it,
and that turns out to be a different problem entirely.

**Why the obvious way does not work.** `VK_MEDIA_PLAY_PAUSE` is global.
Windows hands it to whichever application owns the system media session —
normally the last one that played — so a key that sends it cannot say whether
it means the browser or the music player. Sending `WM_APPCOMMAND` to a chosen
window does not fix it either: a window that does not handle the message passes
it to `DefWindowProc`, which hands it to the shell, which turns it straight
back into the global key. It appears to work, and it hits the wrong
application.

**Windows: the session list.** Every media application registers a session
with the system — the list behind the volume flyout's transport controls. It
can be enumerated, each session labelled with the application it belongs to,
and each one told to toggle **on its own**:

1. `RoGetActivationFactory` for
   `Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager`.
2. `RequestAsync()` for the manager, then `GetSessions()` for the list.
3. `get_SourceAppUserModelId()` on each, matched against the name on the key.
4. `TryTogglePlayPauseAsync()` on the one that matched.

MinGW ships no `windows.media.control.h`, so the three interfaces are declared
by hand in `media.c`. The identifiers and **the order of the methods** are
copied from the Windows SDK header of the same name: the order is the ABI, and
a method declared in the wrong place calls the wrong function.

The manager is built once and kept. Asking the runtime for it costs a factory
lookup and an asynchronous call, and doing that per key press left about six
kilobytes behind each time that only came back when the program ended.

**What the session list cannot reach.** An application that handles the media
keys with a hook of its own, rather than by registering a session, never
appears. VLC 3 is the common example — it is also why it never shows in the
volume flyout. For those, a macro can be **aimed** instead: a chord carrying a
`target` is posted to that application's window with `PostMessage` rather than
synthesised onto the desktop, so `SPACE` sent to `vlc.exe` pauses VLC while
something else has the focus. The limit is modifiers: a program reads the state
of Ctrl and Shift from the keyboard itself, not from the message it was handed,
so a targeted chord carries only as far as the program bothers to look. Single
keys arrive whole, which is what this is for.

**Linux: MPRIS.** The desktops solved this years ago. Every player publishes
`org.mpris.MediaPlayer2.<name>` on the session bus with `PlayPause`, `Next` and
`Previous` on it. `media.c` asks the bus what is there, matches the name, and
calls the method — through `dbus-send`, so nothing new has to be linked in.
VLC does publish MPRIS, so the application that needs aiming on Windows needs
nothing special here.
