# Usage

## Start

Build first:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/libaoahid;/path/to/libmimusb"
cmake --build build
build/out/hid_share_daemon &
build/out/hid_share status
```

The daemon scans when it starts, for phones and for boards, but opens no device by
itself: it only lists the ones that have a file, and what it found.

**With a board (MIM).** Flash the board and wire its serial port as
[libmimusb's quick start](https://github.com/nemarpuc/libmimusb/blob/main/docs/QUICKSTART.md)
says (the firmware of the same 0.7 line as the library; a board with another one is listed
as needing a flash), and give your user access to the serial port.
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
     nothing is assumed: every value must be given); or
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
0.1 or newer and its udev rule, so your user can open the phone
([libaoahid PORTING.md](https://github.com/nemarpuc/libaoahid/blob/main/docs/PORTING.md#linux)).
Plug the phone in and press the refresh icon. It appears as `+ ...`; press it (or run
`hid_share connect <serial>`) to give it a file and open it. Its **Setup** tab then
walks through the same steps: the phone takes it, the screen (measured with the
wizard, or typed; [docs/CONFIG.md](CONFIG.md)), and where it sits.

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

## Getting back

| | |
| --- | --- |
| Push the cursor back through the shared edge | returns to the computer |
| The device's `switch` key | to the device, and back when it has the input |
| `hid_share release` | returns to the computer, from a terminal |

Shortcuts are set per device, in its Shortcuts tab (press **Record keys**, then the combination), and none is set to start with:
`switch` takes the input to the device and back, `lock` keeps the input where it is, on the device or on the computer (for a
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

## What has been tested

- **A board (MIM):** hid_share was moved from libmimhid 0.2 to libmimusb 0.7 (the board
  API changed: functions and endpoints are placed separately, and a clone is a HID Spec).
  The move builds with warnings as errors and passes the unit tests; the board path
  has **not been run again** against the simulated board or a real board since, and
  earlier runs of it were against libmimhid. **hid_share has not been run with a real
  board or a real target machine.** Not tested: how any game or program treats the input.
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
  Android version and vendor; see [docs/MATH.md](MATH.md#gain).
