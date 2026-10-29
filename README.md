# hid_share

Share your computer's mouse and keyboard with another machine. Move the cursor
past the edge of your screen and it continues on the other machine; push it
back and it returns.

There are two ways to reach it, chosen per device:

- **A board (MIM), the main way.** A small microcontroller board (a Raspberry
  Pi Pico or Pico 2) sits between this computer and the other one: this
  computer talks to it over a serial port with
  [libmimhid](https://github.com/nemarpuc/libmimhid), and the board is a real
  USB keyboard and mouse to the other machine. It works with anything that
  takes a USB keyboard and mouse (a PC, a phone with a USB host adapter, a
  machine you cannot install software on), and nothing is installed on it.
  Because it arrives as hardware, the other machine does not see input that
  software injected; how a given game or program treats it has not been tested.
- **A phone (AOA), without any extra hardware.** An Android phone or tablet
  connected by its own USB cable takes the input as an ordinary USB mouse and
  keyboard through
  [Android Open Accessory 2.0](https://source.android.com/docs/core/interaction/accessories/aoa2)
  HID, using [libaoahid](https://github.com/nemarpuc/libaoahid). With USB
  debugging on, it also keeps `adb` working: the phone's ADB interface can be served on
  a local port by a proxy.

Both are in the same window and the same config; a device's file says which it
uses (`link = mim` or `link = aoa`), and nothing is guessed when it does not.

Linux (X11 and Wayland), Windows and macOS.

> **Status:** the phone way is used end to end with a Galaxy Tab S11
> (Android 16) on Arch Linux, KDE Plasma (Wayland). The board way is tested
> against libmimhid's simulated board (a program standing in for the firmware,
> on a pseudo-terminal) and has **not been run with a real board** yet. The
> Windows and macOS backends build in CI but have not been run on real
> hardware yet. See [What has been tested](#what-has-been-tested).

## How it works

```
your mouse/keyboard ─▶ hid_share_daemon ─┬▶ serial ─▶ board (MIM) ─▶ USB ─▶ any machine
                              ▲          └▶ USB (AOA 2.0 HID) ───────────▶ Android
              hid_share (CLI) / hid_share_gui
```

- `hid_share_daemon` runs in the background with no window. It watches one
  span of one screen edge, takes the keyboard and mouse when the cursor pushes
  through it, and sends them to the device.
- The other machine's cursor cannot be read back, so the daemon keeps track of
  where it must be: it pushes the cursor into a corner once (the machine stops
  it there), then counts every movement it sends. [docs/MATH.md](docs/MATH.md)
  has the arithmetic. For a machine that is not Android, turn its pointer
  acceleration off and give or measure the gain (the Measure tab's wizard);
  nothing is read from it.
- When the tracked position reaches the shared edge and you keep pushing, the
  input comes back to the computer at the matching height.

## Quick start (Linux)

Build first:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/libaoahid;/path/to/libmimhid"
cmake --build build
build/out/hid_share_daemon &
build/out/hid_share status
```

The daemon scans when it starts, for phones and for boards, but opens no device by
itself: it only lists the ones that have a file, and what it found.

**With a board (MIM).** Flash the board and wire its serial port as
[libmimhid's quick start](https://github.com/nemarpuc/libmimhid/blob/main/docs/QUICKSTART.md)
says (libmimhid 1.3.3 firmware), and give your user access to the serial port.
Then, in the settings window (`hid_share_gui`):

1. Start the daemon (the button at the bottom left). As it starts, and again when you
   press the **refresh icon** above the device list (or `hid_share rescan`), it looks
   for phones and sends a message to every serial port that could be a board; ports in
   use are passed over. It also measures the fastest serial speed the board's
   adapter carries. The board appears as `+ ...`.
2. Press it (or "Set it up" under Found on the Computer page). The board is added with
   that speed, and its **Setup** tab opens. The
   Setup tab lists what is left, in order, with the reason and the next action at
   each step:
   - what the other computer sees: the name and ids the board shows as a USB
     device (one click copies them from a mouse or touchscreen plugged in here;
     left empty they are the test id 1209:0001, "Generic", "USB Input Device"); or
     present a USB keyboard or mouse plugged in here as the board's own, read whole
     (ids, interfaces, report descriptors) with "Clone a plugged-in device", for your own
     testing only (its looks are copied, not its software);
   - the other computer has taken it: plug the board's USB cable into that
     computer and press **Connect** (it saves what you edited first);
   - the other screen: press **Start Calibration Wizard** and step the cursor to the right
     edge and the bottom edge of that screen, finishing with single counts. A mouse never
     tells the computer how big the other screen is, so it is measured once, in mouse
     counts. With the screen's real size (a beta setting, in the Beta tab) the counts turn
     into the gain, and the touchscreen is built from it;
   - where it sits: the window opens the Layout tab, where you pick the monitor and
     the side, or drag the device into place;
   - try it: the last step names the edge to push the cursor through, and has a
     button that sends the input to the device for five seconds to check that it
     works. It also shows why, when this computer's input could not be set up or the
     device could not be placed.
3. Push the cursor through the edge.

A board that has a file is opened on the port its file names, without searching.
Adding or removing the mouse, keyboard, media keys or touchscreen of an open
board makes it detach from the other machine and attach again once.

**With a phone (AOA).** Install [libaoahid](https://github.com/nemarpuc/libaoahid/releases)
4.0 or newer and its udev rule, so your user can open the phone
([libaoahid PORTING.md](https://github.com/nemarpuc/libaoahid/blob/main/docs/PORTING.md#linux)).
Plug the phone in and press the refresh icon. It appears as `+ ...`; press it (or run
`hid_share connect <serial>`) to give it a file and open it. Its **Setup** tab then
walks through the same steps: the phone takes it, the screen (measured with the
wizard, or typed; [docs/CONFIG.md](docs/CONFIG.md)), and where it sits.

A device's file says how it is reached (`link = aoa` or `link = mim`). A file
from before this setting has none: add `link = aoa` to it by hand.

On GNOME and KDE the desktop asks once for permission to capture input.

`hid_share_gui` does the same from a window, in black and white, laid out
like a device manager: the computer is listed on the left, and once the daemon
runs (the button at the bottom left starts it) the devices too, boards (MIM) and
phones (AOA) under their own headings. The refresh icon at the top right of the
device list searches for phones and boards, and the buttons
under the list connect or disconnect the one that is chosen (for the computer:
disconnect every device, stop the daemon). Apply and Discard changes are at the
bottom right. On the right each setting
is a row, its name at the left and its control (a switch, a drop-down, a field)
at the right edge. A device has the tabs Setup (what is left to do before it works, in order),
Device (name, in use, and for a phone its ADB proxy, for a board its
serial port and speed), Keys (its shortcuts), HID
(which of the mouse, keyboard, media keys and touchscreen it gets), USB (a board only: what it announces),
Placement (drag it around your monitors, or say which side of what it
sits on), Screen, Motion, Tools, Status (every key the daemon reports) and Reports; the
computer has General (with a short "getting started" while there is no device), HID, Motion (with the polling rate), ADB (Android), Tools and Status.
It only edits the config and talks to the daemon; closing it changes nothing.
Nothing starts the daemon but you: run it yourself, or press its button in the
window.

### Getting back

| | |
| --- | --- |
| Push the cursor back through the shared edge | returns to the computer |
| The device's `switch` key | to the device, and back when it has the input |
| `hid_share release` | returns to the computer, from a terminal |

Shortcuts are set per device, in its Shortcuts tab, and none is set to start with:
`switch` takes the input to the device and back, `lock` keeps it there (for a
game that takes the mouse), and each media key can have one.

If the device is unplugged or stops answering, the input returns on its own.

## Delay

Each device has its own link (a USB context for a phone, a serial port for a
board), sender thread and lock, and scanning has its own lock too, so a scan or
a slow device never holds up a report. The daemon measures its own part of the delay for every device and
shows it in the settings window (the Status tab) and in `hid_share status`: from an
input reaching the daemon to its USB report being finished. On the Galaxy Tab
S11 that was 0.34 ms typical and 0.56 ms for 99 reports of 100 (0.9 to 2.5 ms
worst), over 600 reports. The device's own delay, the USB frame and Android's
input stack are not in that number and cannot be removed from here.

## adb keeps working (phones)

Boards have no ADB interface; this is for phones (`link = aoa`). While the daemon holds the phone over USB, `adb` cannot open it itself. Switch
a device's **ADB proxy** on (its Connection tab, or `proxy = true` in its
file) and the daemon serves the phone's ADB interface on `127.0.0.1:<port>`
(6555 for the first device, through the vendored
[aoahid_adb_proxy](https://github.com/nemarpuc/aoahid_adb_proxy)); you then
run `adb connect 127.0.0.1:6555` yourself and the phone shows up in
`adb devices`. The proxy is off until you switch it on, and nothing else in
the daemon uses adb on its own: no `adb connect`, no reading of the phone, no
change of its settings.

The size and pointer settings of a screen are typed or
measured with the wizard (the Setup and Measure tabs); a value that is not set is
said to be missing, never guessed. The only way the daemon touches adb's server is to stop it
(`kill_server`, in `[adb]`): before each scan that was asked for (also the one at start), when it holds
the ADB interface that a proxy is to take, and once more on exit if a proxy was
served.

## Configuration

Everything is kept in a `settings` folder next to the programs, so a copy of
the folder is a portable install (an installed copy that cannot write there
uses the user's own folder; [docs/CONFIG.md](docs/CONFIG.md#files)). The global
`settings/config.ini` holds computer-wide settings and defaults; each
device has its own file, `settings/device/<serial>.ini`, with placement, what
was read from the device, and optional motion overrides.
[docs/CONFIG.md](docs/CONFIG.md) lists every key.

`settings/config.ini`:
```ini
[motion]
sensitivity = 1.0      ; the mouse's movement is sent as it comes, times this
report_rate_hz = max   ; or every | 10..8000

[adb]
kill_server = true     ; stop adb's server before a scan and when it holds a phone a proxy is to take
```

`settings/device/<serial>.ini` for a phone:
```ini
[device]
serial = R52X...
name = Galaxy Tab S11
link = aoa

[placement]
monitor = HDMI-A-2     ; which monitor the device sits next to (or beside = another serial)
side = right           ; left | right | top | bottom
length = 50%           ; how much of that edge it covers: pixels, or a percent of the edge
rotation = 0           ; touch mode only: turn it until taps land right

[detected]
width = 741           ; the travel the wizard measured, in counts
height = 1181
real_width = 1600     ; the real size in pixels; with it the gain follows (leave gain out)
real_height = 2560
accel = off

[adb]
proxy = true           ; serve its ADB interface; off until switched on
```

and for a board (the serial is `mim-` and the end of the port):
```ini
[device]
serial = mim-ttyUSB0
name = Gaming PC
link = mim

[mim]
port = /dev/ttyUSB0    ; or a /dev/serial/by-id/ path, which survives replugging

[usb]                  ; what the board announces; left out: the test id 1209:0001, "Generic", "USB Input Device"
vendor_id = 0x....     ; give both ids or neither
product_id = 0x....
manufacturer = ...
product = ...
```

Several devices have a file each under `settings/device/`, and can be arranged beside
a monitor edge or beside each other so the cursor crosses directly from device
to device.

## Platforms

| Platform | Backend | Crosses at the edge | Notes |
| --- | --- | --- | --- |
| Linux, X11 (any desktop) | `x11` | yes | |
| Linux, Wayland: GNOME, KDE Plasma 6.1+ | `portal` | yes | asks for permission once |
| Linux, Wayland: Sway, Hyprland, other wlroots | `layer_shell` | yes | |
| Linux, anything else | `evdev` | hotkey only | needs read access to `/dev/input` |
| Windows | `windows` | yes | the phone needs a WinUSB driver |
| macOS | `macos` | yes | needs the Accessibility permission |

Details, permissions and limits: [docs/PLATFORMS.md](docs/PLATFORMS.md). How it
shares a screen edge with your desktop's own edge features:
[docs/COEXISTENCE.md](docs/COEXISTENCE.md).

## What has been tested

- **A board (MIM), simulated:** the daemon, the settings window and the CLI
  against libmimhid's simulated board on a pseudo-terminal, on a virtual X
  server with no USB device visible: connect, mouse, buttons, keys, media keys and
  touch reach the board as reports; changing the set of inputs is one re-attach
  and leaving it as it was is none. libmimhid 1.0.1 itself was run against a
  real RP2040 board with its firmware (open, limits, and a 4,000,000 baud link
  through the serial adapter). **hid_share has not been run with a real board or
  a real target machine.** Not tested: how any game or program treats the input.
- **Real device, Arch Linux:** Galaxy Tab S11, Android 16. Corner reference,
  tracking, return, re-entry at a new height and sensitivity were compared against the
  position Android itself reports and matched to within a pixel, with an earlier version
  that also read the screen through adb (that reading is gone: it did not work reliably
  on a second phone). The ADB proxy has been used with it.
- **`x11` backend:** crossing, movement, keys, buttons, return and hotkeys on a
  virtual X server.
- **`portal` backend:** KDE Plasma 6.7 (Wayland), with a physical mouse:
  crossing onto the tablet and back works.
- **`layer_shell` backend:** starts on KDE Plasma 6.7 without protocol errors.
  Not run on a wlroots compositor.
- **The settings window:** drawn on a virtual X server against a fake
  USB device: its rows, a refused value, Apply, and a config file
  that cannot be read. Not run with a real phone, on Wayland, on Windows or on
  macOS.
- **`evdev`, `windows`, `macos` backends:** built only.
- **Other phones:** none yet. How far one count moves the cursor depends on the
  Android version and vendor; see [docs/MATH.md](docs/MATH.md#gain).

## Building

Requires CMake 3.22, a C++20 compiler, libaoahid 2.0 or newer and libmimhid 2.0 or newer. On Linux each
backend is built when its libraries are found:

| Backend | Packages (Debian names) |
| --- | --- |
| `x11` | `libxcb1-dev libxcb-xfixes0-dev libxcb-xinput-dev libxcb-randr0-dev` |
| `portal` | `libdbus-1-dev libei-dev libwayland-dev` |
| `layer_shell` | `libwayland-dev` |
| `evdev` | none |
| the settings window | `libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxkbcommon-dev libwayland-dev` |

Dear ImGui and GLFW are downloaded at configure time for the settings window;
`-DHID_SHARE_BUILD_GUI=OFF` leaves it out.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/libaoahid;/path/to/libmimhid"
cmake --build build
ctest --test-dir build
```

## License

MIT. Vendored: [aoahid_adb_proxy](third_party/licenses/aoahid_adb_proxy-LICENSE.txt)
(MIT), [doctest](tests/third_party/doctest.h) (MIT), and the Wayland protocol
descriptions under `protocols/` (their own notices, in each file).
