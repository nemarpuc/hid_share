# Changelog

## 0.4.0

- Boards run libmimhid 1.3.3 (protocol 6). A clone keeps the device's endpoint sizes (1 to
  the board's chip's packet size, 64 at full speed and 1024 at high speed) and the board
  answers a Feature report up to its `max_feature_bytes` (1024), so the Apex's 644-byte
  report is answered; a larger one is declared and left silent. Needs the 1.3.3 firmware on the board.

- Clone mode (a board only): the Connection tab can present a USB device plugged into this
  computer (a keyboard, a mouse) as the board's own. Its ids, strings, power values and every
  HID interface (number, polling interval, endpoint sizes, boot protocol, `bcdHID`, report
  descriptor with its Report IDs) are read from sysfs as they are and kept in `[clone]` of the
  device file. The mouse, keyboard and media switches cut the parts that are off out of the
  descriptors the other computer sees, and the input goes into the device's own reports
  (a 6-key array or an NKRO bitmap, the Consumer and mouse collections, a battery level when
  the device has one). Only the looks are copied: vendor interfaces are presented but silent,
  a Feature report above the board's `max_feature_bytes` is declared but cannot be answered,
  an endpoint size is kept as it is and cut to the board's chip's packet size, and touch mode
  is not available. The status gains `clone` and `clone_notes`.
- Touch mode is a beta: `[daemon] beta = true` shows a Beta tab (touch mode, the real size,
  rotation, tap and swipe settings, the descriptor values) in the settings window and
  takes them off the other tabs. The setting only hides the tab; a device with touch on
  keeps it. The wizard no longer asks for the real size (type it in the Beta tab).
- The settings window's tabs have plainer names: Connection, Measure, Layout, Inputs,
  Shortcuts, Pointer.
- The Measure tab says when the screen was probably turned after it was measured (the real
  size fits the travel only when swapped) and offers to swap the travel.
- The calibration wizard measures each edge by single steps: press 100, 10 and 1 toward
  the far edge without passing it; the press of 1 that moved nothing is not counted, so
  the count before it is exact to one count (`ProbeAxis`). There is no undo: a press
  that passed the edge means measuring again. The same in the settings window and in `hid_share calibrate`,
  for every link.
- A board's `[usb]` values have ordinary defaults: the pid.codes test id 1209:0001
  (for in-house testing only), `Generic` and `USB Input Device`. The two ids go together;
  one given alone is not completed and the board is not opened. The device status shows
  `usb_test_id`. `language_id` is new.
- A board's Nodes have their own USB interface values: `[usb.mouse]`, `[usb.keyboard]`,
  `[usb.media]`, `[usb.touch]` (interface number, name, polling interval, endpoint sizes,
  HID release, country code, Report ID). The numbers are checked against
  the board's limits; a change makes the board re-attach once. Nodes with the same
  `interface_number` share one HID interface and then each needs its own `report_id`.
- `[touch_hid]` sets the touchscreen's descriptor values (contacts, contact id and count,
  coordinate width, pressure, width, height, azimuth, scan time) for a board and a phone.
  Left out, the descriptors are what they were.

- The real size of the other screen is typed (`[detected] real_width`, `real_height`).
  With it and the travel the wizard measured in counts (`width`, `height`, kept as they
  were), the gain is calculated: the pixels across both edges over the counts across
  both, so it no longer has to be typed or taken as 1. The wizard's last step has the
  rows, shows the gain and warns when the two axes disagree by more than 1%; a typed
  `gain` still wins, and the Screen tab offers to drop it when it differs from the
  calculated one. With the real size given, the daemon works in real pixels.
- The touchscreen is built from the real size (turned by `rotation`), no longer from the
  measured counts, so a tap lands on the pixel the cursor is on and measuring again no
  longer re-attaches the board. Touch needs `real_width`, `real_height` and `rotation`;
  a file that has touch on without the real size reports them as missing. A **Test tap**
  button (`probe DEVICE tap X Y`) taps a quarter in from the top left corner to check the
  rotation.
- The wizard no longer says to push past the edge: every count pressed is counted, so the
  extra made the screen look bigger than it is.
- The HID tab holds every touch setting of the device, each following the computer unless
  set, the swipe settings included (they were hidden while the wheel swipe was off).
- The device page has fewer tabs: Setup (only while something is left to set, or while it
  is the open tab), Device (a board's USB values are under it), Screen, Placement, HID,
  Keys, Motion, Tools and Status (with the Reports). Long explanations are folded.

- Rotation is gone from the mouse: the wizard measures the screen as it is shown, so
  the screen's rotation is no longer asked for or needed, and `turn_input` is read and
  dropped. Only touch mode keeps `rotation`, with a note and a **Turn touch 90 degrees**
  button in the HID tab, because touch positions do not follow the screen's rotation.
- Rotation is gone from the mouse: the wizard measures the screen as it is shown, so
  the screen's rotation is no longer asked for or needed, and `turn_input` is read and
  dropped. Only touch mode keeps `rotation`, with a note and a **Turn touch 90 degrees**
  button in the HID tab, because touch positions do not follow the screen's rotation.
- Renamed from aoahid_share to hid_share: the include directory, namespace,
  programs, environment variables and repository.
- A MIM board (libmimhid) is now the main way to reach a target machine, of any
  kind; an Android phone over AOA (libaoahid) still works. A device's file says
  `link = aoa|mim`; a file without it is not opened and its status asks for the
  line (add `link = aoa` to files from earlier versions).
- Per board, new `[mim]` (`port`, `baud`) and `[usb]` sections. They have no defaults;
  boards differ in speed, so `baud` is each board's own, and adding a board from the
  search writes the fastest rate the search measured. The wait for a board's answer is
  a constant (500 ms), not a setting; an old `[mim] timeout_ms` line is read and dropped.
  See `docs/CONFIG.md`.
- One search: the refresh icon (`hid_share rescan`) looks for phones and boards. It
  asks every serial port whether a board answers; so does the daemon's first scan as
  it starts, and no other (not the ones after a failure, `reload` or `connect`). Ports in
  use are passed over. The separate Find buttons and `find_boards` are gone.
- A **Setup** tab, first for every device, lists what is left, in order, with the reason
  and the next action at each step: for a board, found, what the other computer sees,
  taken by the other computer, the other screen, where it sits; for a phone the same
  without the second. A device that is not fully set up opens on it when it is chosen in
  the list, and so does a device that was just added. One click copies a board's name and
  ids from a mouse plugged in here; one button saves what was edited and connects; the
  calibration wizard starts from the screen step, saves and applies, and the window moves on to the Placement tab. The Computer page
  lists what the last search found with a "Set it up" button for each, and says how to
  start while there is no device.
- Setup ends with **Try it**: it says which edge of which monitor to push the cursor
  through, has a row for the optional switch key (there is none unless it is set), and a
  button that sends the input to the device for five seconds and takes it back by itself
  (a backend that only crosses at the screen edge says so instead). Above the steps it
  shows when this computer's input could not be set up (the daemon's `capture_error`) or
  the desktop is still asking for permission, and under "Where it sits" the daemon's
  reason when the device is not placed.
- **The diagonal is gone, and Placement takes ratios.** `length` and `start` accept a percent
  of the edge the device sits on (`50%`, `25%`) as well as pixels; a ratio fits any monitor
  or device, and is what dragging or resizing a device in the picture now writes. Without a
  `length`, a device covers the whole edge. The physical size of the screen
  (`diagonal_inch`) and of a monitor (`monitor_diagonal_inch`) no longer size anything: old
  lines are read and dropped, the daemon no longer refuses a device for a monitor that
  reports no size, and the status key `density` is gone. The settings window and the
  wizard no longer ask for a diagonal, and `hid_share calibrate` no longer sets one.
- The Screen tab is reorganised: measure it (the wizard), what was measured, about the
  other screen (rotation, diagonal, pointer acceleration), and the rest under Advanced.
  It says what the width and height are: how far the cursor can travel, in mouse counts
  when measured (gain 1), not the real resolution.
- A board that was asked for but cannot be opened (it lacks what it cannot be opened
  without, or opening failed) now shows `[!]` in the list, as a phone does.
- **Fill from ADB is gone** (the `fill` command, the button, `detected.read_at`, the
  status keys `fill`, `reading` and `files_version`): reading a phone through adb did
  not work reliably. Screen size and gain are measured with the calibration wizard or
  typed. The ADB proxy stays; adb is run only to stop its server. A `read_at` line in
  an old file is read and dropped.
- The output side is split into a hub (queue, pacing, statistics) and one port
  per target; the AOA code is unchanged in behaviour.
- The settings window groups devices by link and has a USB tab for boards (the four
  values a board cannot be opened without, and the rest under Advanced).
- The USB tab of a board can copy everything a plugged-in mouse or touchscreen
  announces (ids, strings, serial number, release, current, power attributes;
  Linux). New `[usb]` keys: `bcd_device`, `max_power_ma`, `self_powered`,
  `remote_wakeup`, `configuration`.
- Builds need libmimhid 1.0 or newer as well as libaoahid; CI and release
  builds fetch libmimhid 1.0.1 and libaoahid 1.0.1.

## 0.3.11

- Interactive display calibration wizard in GUI and CLI (`hid_share calibrate`)
  to calibrate screen boundaries and cursor gain without ADB.
- Reorganized Screen tab in the settings window: simpler resolution and gain
  view, collapsed ADB details, and clear cursor idle / entry mode selection
  (parked, aligned, or corner).

## 0.3.10

- The daemon no longer exits or goes deaf when the desktop ends the capture
  (the compositor or X server restarts, or the portal session is closed). It
  keeps the devices open and starts a capture again every 15 seconds, as at
  startup; `status` says why meanwhile. See `docs/PLATFORMS.md`.

## 0.3.9

- Built against libaoahid 1.0.0 as re-released on 2026-10-11. This program's
  code is unchanged, but its options structures grew there (each profile can
  carry a battery report), so a 0.3.8 build cannot open its HIDs with the new
  library, nor this build with the old one: put the matching library next to
  the programs.

## 0.3.8

- Builds against libaoahid 1.0.0 as it is released. Only names changed: the
  media key call is `aoahid_toggle_usage` (it was `aoahid_toggle`), and the
  options fields are `contact_id`, `linux_hid_report_data_policy_bits`, and
  `linux_hid_report_size_policy_bits`. Behavior is the same.

## 0.3.7

- Builds against libaoahid 1.0.0: the `find_package` minimum is 1.0.0 and CI
  and the release build use the 1.0.0 release assets. libaoahid restarted its
  version numbering at 1.0.0 and withdrew the earlier release tags. The only
  code changes are the names of two calls, `aoahid_touchscreen_contact` (was
  `aoahid_touch`) and `aoahid_keyboard_key` (was `aoahid_kbd`), and a media key
  release that now names its key instead of 0.

## 0.3.6

- Volume up and volume down join the media keys: `volume_up` and
  `volume_down` in a device's `[keys]`, in `hid_share media`, and as
  buttons in the device's Tools tab.

## 0.3.5

- Shortcuts belong to each device. A device's file has a `[keys]` section:
  `switch` (to the device, and back to the computer when it has the input),
  `lock`, `resync` and the five media keys. All start empty.
- `lock` keeps the input on the device whatever edge the cursor reaches, for
  a game that takes the mouse. The same key, a `switch` key, `release`,
  `pause` or closing the device ends it. `status` has `locked` per device.
- A device's media keys go to that device, wherever the input is; several
  devices can each have their own.
- Gone: `toggle_hotkey`, `panic_hotkey`, `pause_hotkey`, `resync_hotkey`, the
  `[media]` hotkeys and `target` in `config.ini`, and `[device] hotkey`. They
  are still read, and dropped; set the keys again in each device's Keys tab.
  `hid_share pause`, `resume` and `release` are unchanged.
- `status`: `protocol` is 3; `media_target` is gone.
- Settings window: a Keys tab per device; the computer's page has an HID tab
  and no hotkeys; Tools, then Status and Reports, are the last tabs; Status
  lists what is wrong first; the media-key buttons are in each device's Tools.

## 0.3.4

- CI and release builds link libaoahid 4.2.0 (was 4.1.0). The `find_package`
  minimum (4.0.0) is unchanged.
- The vendored aoahid_adb_proxy is 3.2.1 (only its header's version macros
  differ from 3.2.0).

## 0.3.3

- `scroll_overlap` starts as `restart`: every wheel notch is a swipe of its own
  from the cursor. With `add` (the old start) the finger went further from the
  cursor with every notch, and the view drifted. The setting is labelled "Next
  notch during a swipe" and its help says what each value does.

## 0.3.2

- `[touch] swipe` turns the wheel swipe on and off; it is off by default and
  the settings window shows the swipe settings only while it is on. The swipe
  settings start from values to begin with: 100 device pixels per notch in both
  directions (a starting value, not measured against a mouse wheel), 4 steps
  in 8 ms, put down 1 ms before the first step, lifted 200 ms after the last, a
  notch during a swipe added to it.

## 0.3.0

- Touchscreen mode (`[touch]`, global and per device): registers a touchscreen
  next to the mouse and keyboard. The tap button (`tap_button`, left by
  default) becomes a tap at the tracked cursor position and moving while it is
  held drags. Needs the device's `width`, `height` and `rotation`; the
  position is converted for every rotation and mount (docs/MATH.md).
- The wheel can swipe a second finger (`scroll`, `scroll_pan` in device pixels
  per notch). Every notch is a swipe: put down where the cursor is, moved in
  `scroll_steps` steps (the first `scroll_start_ms` after the finger is down,
  the last `scroll_total_ms` after the first), and lifted `scroll_release_ms`
  after the last step. Android takes a swipe split in steps for a scroll more
  readily than a jump. A notch that comes while a swipe goes on is added to
  what is left of it, or lifts that finger and starts again
  (`scroll_overlap = add | restart`).
- Android drops a touch when a mouse report arrives while it is down, so no
  mouse movement is sent while a finger is down: it is summed and sent in one
  report after the finger's lift. While the tap button is held the cursor
  therefore stays where it was and jumps to the drop point (docs/CONFIG.md).
- The daemon scans for devices when it starts (stopping the adb server first,
  as for any scan that was asked for). A device that has a file and shows up
  in a scan is listed but no longer opened by itself; it is opened with
  `connect` or the Connect button. A device that was connected that way and
  drops out still comes back by itself.
- New `reports SERIAL|NAME` command and a Reports tab on a device's page: the
  calls handed to the device (mouse, keys, media keys, touch contacts), one
  line each, grouped by report. Recorded only while it is being watched.
- The migration of the 0.2 file layout was removed.

## 0.2.1

- The daemon stops a running adb server before every scan that was asked for (a
  reload, `rescan`, `connect`), so a phone the server had claimed is found
  again. It is switched off with `kill_server = false` under `[adb]`.

## 0.2.0

The settings window was rebuilt.

- Black on white, laid out like a device manager: the computer and the devices
  on the left, and under the list the buttons that connect or disconnect the
  one chosen (for the computer: disconnect all, stop the daemon). On the right
  every setting is a row with its name at the left and its control at the right
  edge: real switches for on and off, drop-downs, fields. A device has the tabs
  Device, Placement, Screen, Motion, Status and Tools; the computer has General,
  Motion (the polling rate is here), ADB, Status and Tools.
- Every key of the config files and every key of `hid_share status` is shown
  with its unit, default and meaning (as a tip on the name), and every setting
  can be edited except the serial number and the date a device was read, which
  mean nothing when typed. A value the parsers refuse stays on screen with their
  reason and holds Apply back.
- The devices are listed only while the daemon runs; before that the button at
  the bottom left starts it.
- A device that is open can be disconnected even when "In use" has been switched
  off.
- The polling rate (`report_rate_hz`) can be set for each device (its Motion
  tab, or `[motion]` in its file); a device without one follows the computer's.
- `x11`: while the input is on a device the PC's cursor is held where it was
  taken. It was hidden but kept moving over the screen (and its edges and hot
  corners) before.
- Removed background automatic USB device scanning; devices are scanned only
  when asked (`rescan`), with a refresh button placed at the top right of the
  device list.
- Rescan immediately detects and drops physically unplugged devices, and
  automatically connects and applies configuration for previously configured
  devices.
- The device list now displays only currently connected devices by default;
  disconnected configured devices are tucked away under a "Disconnected" group.
- Fix: "Fill from ADB" can now be used on unconfigured devices as long as their
  ADB proxy is running, without requiring the layout to already be ready.
