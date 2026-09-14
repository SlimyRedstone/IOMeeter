# IOMeeter

An open source alternative to Voicemeeter for Windows and Linux, driven by a
physical control surface rather than a window full of sliders.

IOMeeter is the desktop client. It talks to an ESP32-S3 controller over USB,
mixes the volume of individual applications, and turns the controller's pad
into macros. The window is there when you want it and lives in the tray when
you do not.

## What it does

* **Per-application volume, up to 4 channels.** Each fader owns a list of
  executables, and moving it changes the volume of those programs only. The
  system volume is never touched.
* **Runs on Windows and Linux.** Core Audio sessions on Windows, PulseAudio
  sink inputs elsewhere (PipeWire is served through its PulseAudio
  compatibility layer).
* **A 30 key macro pad.** Every key carries a name, a colour and a macro: a
  chord of keys, a phrase typed out, or the playback of one named application.
* **Profiles.** The whole pad re-binds at once, and a profile can select
  itself whenever one of its applications is the window in front.
* **Per-application playback.** A media key is global and lands on whichever
  program played last, so IOMeeter addresses the application instead: its
  media session on Windows, its MPRIS interface on Linux.
* **Tray resident.** Closing the window hides it and saves the configuration;
  a second launch raises the copy already running instead of starting another.

## Hardware

Either device works on its own, and the client prefers the surface when both
are attached.

| Device | Repository | What it is |
| --- | --- | --- |
| Surface | [IOMeeter-Surface](https://github.com/SlimyRedstone/IOMeeter-Surface) | The control surface: four faders and a 6 by 5 macro pad |
| Dongle | [IOMeeter-Dongle](https://github.com/SlimyRedstone/IOMeeter-Dongle) | The receiver, for using the surface wirelessly |

Both expose a vendor-specific USB interface with two bulk endpoints alongside
their CDC serial function, so a terminal can stay open on the COM port while
the client is running. On Windows the firmware's MS OS 2.0 descriptors bind
WinUSB automatically, so no driver has to be installed by hand.

The configuration is also stored on the controller itself, so the fader names,
the pad and the profiles follow the hardware rather than the machine it was
set up on.

## Building

The client is C99 and builds with CMake. `libusb-1.0` and `raylib` are
required; everything else is optional and only costs a feature.

### Windows

libusb and raylib come from [vcpkg](https://github.com/microsoft/vcpkg):

```
vcpkg install libusb:x64-mingw-dynamic raylib:x64-mingw-dynamic
```

Then, from the project directory:

```
build.bat          configure if needed, compile, then run
build.bat build    compile without running
build.bat clean    wipe the build directory and reconfigure
```

`install.bat` puts a copy in `%APPDATA%\IOMeeter` with a Start menu entry.
Neither script needs administrator rights.

### Linux

```
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev \
                 libpulse-dev libayatana-appindicator3-dev libxtst-dev \
                 libx11-dev libraylib-dev
```

`libraylib-dev` exists from Debian 12 and Ubuntu 23.04 onward; on anything
older, build raylib from source. `./install.sh` sorts all of this out and
reports what it could not find.

```
./build.sh         configure if needed, compile, then run
./install.sh       prerequisites, build, install to ~/.IOMeeter
```

Talking to the device needs permission for the USB node. Install the udev rule
once, then unplug and replug the device:

```
sudo ./install.sh --udev
```

Do not run IOMeeter with `sudo` instead. That opens the device but cuts the
process off from the desktop session's sound server, so the mixer reports
itself unavailable and no volume is controlled.

### Optional dependencies on Linux

| Package | Without it |
| --- | --- |
| `libpulse-dev` | No per-application volume |
| `libxtst-dev` | Macro keys cannot type |
| `libx11-dev` | Profiles do not switch on their own |
| `libayatana-appindicator3-dev` | No tray icon, and the close button quits |

## Configuration

Everything lives in `config.json` beside the executable, written indented so it
can be opened and edited by hand:

```json
{
  "debug": false,
  "sliders": [
    { "id": 0, "value": 3994, "name": "Main",
      "apps": [{ "path": "C:\\Program Files\\VideoLAN\\VLC\\vlc.exe",
                 "name": "vlc.exe" }] }
  ],
  "keys": [
    { "id": 0, "macro": [{ "profile": 0, "color": "ff00ff",
                           "timings": [20, 20, 20],
                           "cmds": ["CTRL_LEFT", "SHIFT_LEFT", "M"] }] }
  ],
  "profile": 0
}
```

* Fader values run from 0 to 4095, bottom to top.
* `"debug"` opens the traffic console, which shows every JSON document
  crossing the wire in both directions.
* A missing or malformed file is not an error. Whatever cannot be read falls
  back to the built-in defaults, so the interface always starts.

Faders and applications are normally set up in the Configuration tab rather
than by hand, and the file is saved when the window is hidden to the tray.

## Built with

* [raylib](https://www.raylib.com/) for the window, input and drawing
* [Clay](https://github.com/nicbarker/clay) for the layout, with its raylib
  renderer
* [cJSON](https://github.com/DaveGamble/cJSON) for the wire protocol
* [libusb](https://libusb.info/) for the vendor interface

Clay, its renderer and cJSON are vendored in [libs/](libs/). Everything in
[include/](include/) is ours, and [include/HOW.md](include/HOW.md) explains how
the pieces fit together, including how a packet leaves the program and how one
application's volume is changed without moving the system volume.

## License

GNU General Public License v3.0. See [LICENSE](LICENSE).
