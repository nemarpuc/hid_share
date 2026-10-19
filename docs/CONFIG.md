# Configuration and commands

## Files

Everything is kept in one `settings` folder next to the programs
(`hid_share`, `hid_share_daemon`, `hid_share_gui`), so a copy of the
folder is a portable install:

```text
settings/
  config.ini          computer-wide settings and defaults
  state.ini           what the daemon remembers (the portal's restore token, ...)
  daemon.log          the daemon's log, started over on each run
  device/
    <serial>.ini      one Android device each: placement, what was read from it
```

Where that folder cannot be used, the same four things are kept in the
user's own folder instead: `$XDG_CONFIG_HOME/hid_share` (else
`~/.config/hid_share`) on Linux, `%APPDATA%\hid_share` on Windows,
`~/Library/Application Support/hid_share` on macOS. That is the case when the
programs' folder cannot be written to (an installed copy under `/usr/bin` or
`Program Files`), when the programs are inside an application bundle, and, on
Linux and macOS, when `settings` is not this user's own or can be written to by
others (the settings name the adb program to run, so a folder others can change
is not trusted). `HID_SHARE_SETTINGS_DIR` names another folder for all of it.

`hid_share paths` prints them. Each configured Android device has a file of
its own under `device/<serial>.ini`, created when you connect it.

The daemon writes `daemon.log`, started over on each run: which backend it captures with, each device connecting or dropping,
and every error. If no backend can start yet (the desktop's permission dialog
is unanswered, for instance) the daemon keeps running, `status` shows
`state=starting` with the reason, and it tries again every 15 seconds.

## `[daemon]`

| Key | Default | |
| --- | --- | --- |
| `backend` | `auto` | `auto`, `x11`, `portal`, `layer_shell`, `evdev`, `windows`, `macos` |
| `beta` | `false` | shows the Beta tab of the settings window, where touch mode and its settings are (real size, rotation, tap, swipe, descriptor). It hides nothing in the files and changes no behaviour: a device with touch on keeps it while the tab is hidden |

The shortcuts are each device's own: see [`[keys]`](#keys).

## `[mouse]` and `[keyboard]`

| Key | Default | |
| --- | --- | --- |
| `buttons` | `5` | 1 to 8; `hid_share buttons` reports what your mice have |
| `button_map` | empty | HID button for PC button 1, 2, ...; `1,3,2` swaps right and middle |
| `[mouse] enabled` | `true` | `false` does not register the mouse on the device |
| `[keyboard] enabled` | `true` | `false` does not register the keyboard on the device |

## `[media]`

`enabled = false` does not register the media keys on the device (default
`true`; it does not follow `[keyboard] enabled`).

A shortcut for a media key is set per device, in its [`[keys]`](#keys). The
settings window has a button for each key in the device's Tools tab.

The keyboard's own volume and media keys need no setting. While the input is
on a device they go to that device and the computer does not act on them,
on every backend but `macos`, which does not capture those keys.

## `[touch]`

Touchscreen mode registers a touchscreen on the device (a fourth input next to
the mouse, keyboard and media keys). The mouse stays as the marker of where
the cursor is; the left button, pressed where the cursor is, becomes a tap
there, and moving while it is held drags. It needs the device's `real_width`,
`real_height` and `rotation`; without them the touchscreen is not registered and
`status` says what is missing (`touch`). The touchscreen is the real size of the
screen (turned by `rotation`), so a tap lands on the pixel the cursor is on.

Unlike the mouse, which Android turns with the screen, touch positions do not
follow the screen's rotation by themselves. `rotation` says how far the screen as
shown is turned from the touchscreen's own axes; if taps land with x and y
swapped or mirrored, turn it (the **Turn touch 90 degrees** button of the Beta tab)
until they land right, and set it again whenever the screen is turned. The **Test tap**
button taps a quarter in from the top left corner of the screen as shown (the control
command `probe DEVICE tap X Y` taps any pixel of it), which a wrong rotation puts in
another corner.

| Key | Default | |
| --- | --- | --- |
| `[touch] enabled` | `false` | `true` turns the mouse button `tap_button` into a tap |
| `tap_button` | `1` | 1 to 8; the mouse button that becomes a tap (the left one is 1); it need not be one of the `buttons` the mouse declares |
| `swipe` | `false` | `true` makes the wheel swipe a second finger; `false` leaves it a mouse wheel and the swipe settings below do nothing (the settings window shows them only while it is on) |
| `scroll` | `100` | -2000 to 2000; pixels of the device's screen the finger moves, all at once, for each wheel notch (times `scroll_sensitivity`), from where the cursor is; `0` leaves the wheel a mouse wheel, a negative value reverses the direction |
| `scroll_pan` | `100` | the same for the horizontal wheel |
| `scroll_start_ms` | `1` | 0 to 2000, in milliseconds (fractions allowed, e.g. `0.25`); time from the swiping finger being put down to its move, for every notch; `0` moves it in the very next report |
| `scroll_steps` | `4` | 1 to 64; the moves the distance of a notch is divided into (each is a touch report, at most one per polling period) |
| `scroll_total_ms` | `8` | 0 to 2000, in milliseconds (fractions allowed); time from the first move to the last of a notch, so the gap between moves is this divided by `scroll_steps - 1` |
| `scroll_overlap` | `restart` | `restart` or `add`; what the next wheel notch does when the swipe before it has not finished. `restart` lifts the old finger and makes the notch a swipe of its own from the cursor (what was left of the old one is lost; the view does not drift). `add` adds the notch to what is left and the same finger goes on, which takes it further from the cursor with every notch |
| `scroll_release_ms` | `200` | 0 to 2000, in milliseconds; time from the finger's move to its lift, for every notch; `0` lifts it in the report after the move |

**Why the cursor stands still during a drag.** When a touchscreen and a mouse
report at the same time, Android drops whichever was there first (observed on
a Galaxy Tab S11; see [libaoahid's API.md](https://github.com/nemarpuc/libaoahid/blob/main/docs/API.md)).
A mouse report in the middle of a drag would therefore cancel the finger. So
while the tap button is held no mouse report is sent: the movement is summed,
the finger follows where the cursor would be, and when the button is released
the finger is lifted first and then everything that was summed goes out as one
mouse report. The cursor appears to jump to the drop point at the release.
After a drag the position estimate is less exact, because Android scales that
one report as one movement, not as the many small steps it was calculated
from.

Two fingers are registered: the click uses the first, the scroll the second,
so a scroll can go on while a button is held. Every wheel notch is a swipe of
its own: the finger is put down where the cursor is, the distance is moved in
`scroll_steps` steps (the first `scroll_start_ms` after the finger is down, the
last `scroll_total_ms` after the first), and the finger is lifted
`scroll_release_ms` after the last step; Android takes a swipe split in steps
for a scroll more readily than a jump. A notch that comes meanwhile is handled
as `scroll_overlap` says; a notch that comes meanwhile lifts it first.
While a finger is down no mouse report is sent (the same reason as for a
drag above): movement that comes in meanwhile is summed and sent after the
lift. How the position is turned into the touchscreen's own coordinates, for
every rotation, is in [docs/MATH.md](MATH.md#touch).

## `[motion]`

Global defaults for pointer motion. Every setting here can be overridden per
device in its own file.

| Key | Default | |
| --- | --- | --- |
| `sensitivity` | `1.0` | 0.05 to 20; the mouse's movement is sent as it comes, times this |
| `sensitivity_y` | empty | vertical only; empty follows `sensitivity` |
| `scroll_sensitivity` | `1.0` | |
| `report_rate_hz` | `max` (a device may set its own) | `max` sends a report as soon as the previous one completes and sums what moved meanwhile; `every` sends each movement and scroll step as a report of its own, in order, with no limit; 10 to 8000 paces motion at that many reports a second |
| `entry` | `parked` | how the cursor enters a device: `parked` keeps the device's cursor in its bottom right corner while the input is on the PC, where it is all but hidden, and enters from there; `aligned` moves it from where it was left; `corner` takes the corner reference every time. `park = true\|false` and `entry = align\|corner` are also read |
| `resync_width` | `48` | pixels of uncertainty above which the corner reference is taken again |
| `enter_push` | `0` | pixels to push against the edge before crossing |
| `return_push` | empty | pixels past the device's edge before returning; empty is 1, or 16 when the gain is uncertain |
| `no_cross_while_button` | `true` | |

[MATH.md](MATH.md) defines each of these.

## `[adb]`

| Key | Default | |
| --- | --- | --- |
| `kill_server` | `true` | stop a running adb server before every scan that was asked for (the one made when the daemon starts, a reload, `rescan`, `connect`), since a server that claimed a phone keeps it from being found; also when a phone's proxy is to start and the server holds its ADB interface (once, then the proxy is tried again), and once more on exit if a proxy was served, so that adb sees the phone over USB again |
| `path` | empty | the adb program, run only to stop its server (`kill_server`); empty uses `PATH` |
| `first_port` | `6555` | where the proxy ports start (1024 to 65535, not 5555 to 5585) |

The daemon runs adb for one thing only: to stop its server (`kill_server`). Nothing is read
from a phone through adb. A device's ADB proxy is **off** until
its `proxy = true` is set (its page in the settings window, or its file); only
then is the phone's ADB interface served on `127.0.0.1:<port>`, and you run
`adb connect` yourself. A device's port is `first_port` plus the number of
devices before it (files are taken in the order of their names) unless its
file gives `port`; set `port` to keep one fixed.

On Linux with a glibc older than 2.34, or musl, an adb server started by `adb
connect` can keep the proxy's port bound after the daemon exits (the
descriptor cannot be closed on `exec` there).

`calibrate` (`hid_share calibrate [DEVICE]`) runs an interactive calibration
wizard in the terminal. It resets the cursor to the
top-left corner, lets you move the cursor across the width and height using the
Space key (tap for 1 count, hold for continuous motion, Backspace to step back,
Enter to confirm), and directly sets your device's screen dimensions (Width, Height,
Gain = 1.0, and Accel = off) based on the measured counts
([MATH.md](MATH.md#without-adb)). Upon confirmation, the dimensions are saved
directly into the device's settings file and the daemon is reloaded. 

## Boards

A device with `link = mim` is a microcontroller board reached over its serial
port (libmimhid). Its port and speed are in its own file (`[mim]` below). A
board that has a file is opened on the port its file names, without searching.
Other boards are found as the daemon starts and when the user asks (`rescan`, the
refresh icon of the settings window), and at no other time, because the search sends
a message to every serial port that could be one, at each of libmimhid's line rates. The search measures the
fastest rate each board's serial adapter carries, and a port in use (by this
program or another) is passed over.

The wait for a board's answer is not a setting (500 ms). A `[mim] timeout_ms`
line in `config.ini`, from before 0.4.0, is read and dropped.

## Per-device files (`device/<serial>.ini`)

Each configured device has its own file named after its serial number: a
phone's USB serial, or, for a board, `mim-` and the end of its serial port
(letters, digits, `-`, `_`, `.`; at most 64). A key left out takes the global
value where there is one, else is not set: the daemon then says in the
device's status what is missing, and does not guess it. A value is written as
a number, and its line is left out when it was never given (an empty value
reads as not set).

### `[device]`

| Key | Default | |
| --- | --- | --- |
| `serial` | empty | the serial the file is named after; a profile that names none is filed as `unassigned N.ini` until a device takes it |
| `link` | empty | `aoa` for a phone, `mim` for a microcontroller board. It is set when the device is added and has no default: without it the device's status says it is missing |
| `name` | empty | display name, at most 48 bytes, without `=`, `;`, `#`; it may not be another device's serial |
| `enabled` | `true` | `false` keeps the device configured but left alone |

### `[keys]`

The device's own shortcuts, all empty (none) to start with.

| Key | Works | |
| --- | --- | --- |
| `switch` | anywhere | takes the input to this device, and back to the computer when it has it |
| `lock` | while the input is on this device | keeps the input here whatever edge the cursor reaches, also toward a device beside it; press again to let go |
| `resync` | while the input is on this device | its next entry takes the corner reference |
| `previous`, `play_pause`, `next`, `brightness_up`, `brightness_down`, `volume_up`, `volume_down` | anywhere | sends that media key to this device |

A key is modifiers (`ctrl`, `shift`, `alt`, `meta`) and one key: a letter, a
digit, `f1`..`f24`, `escape`, `space`, `tab`, `enter`, `backspace`, `insert`,
`delete`, `home`, `end`, `pageup`, `pagedown`, `up`, `down`, `left`, `right`,
`pause`, `scrolllock`. Empty turns it off.

```ini
[keys]
switch = ctrl+alt+1
lock = ctrl+alt+l
play_pause = ctrl+alt+p
```

A `switch` or media key may not be any other key of any device. `lock` and
`resync` may be the same on every device: they act on the one that has the
input.

The lock is for a game that takes the mouse: no edge sends the input back.
It ends with the lock key, a `switch` key, `hid_share release` or `pause`,
or when the input is taken back for another reason: the device is closed, or
another device or a monitor comes or goes. It is not kept: a device starts
unlocked.

"Anywhere" needs a backend that sees the keyboard while the input is on the
computer (`local_hotkeys=yes` in `status`). On `portal` and `layer_shell` these
keys work only while the input is on a device; bind a desktop shortcut to
`hid_share enter NAME` or `hid_share media play_pause NAME` instead.

Until 0.3.4 the hotkeys were in `config.ini` and in the `[device]` section.
Those keys are still read, and dropped: set the keys again here.

### `[placement]`

| Key | Default | |
| --- | --- | --- |
| `beside` | empty | serial of another device to sit next to; empty sits beside a monitor |
| `monitor` | empty | monitor name as `status` shows it; empty is the first monitor |
| `side` | `right` | `left`, `right`, `top`, `bottom` |
| `start` | empty | where the device starts along the edge it sits on, from the start of that monitor's or neighbour's edge: in pixels, or as a percent of the edge (`25%`), which fits any monitor or device; not set, it is centred |
| `length` | empty | how much of that edge it covers: in pixels, or as a percent of the edge (`50%`); not set, all of it. The settings window writes percents when a device is dragged or resized in the picture |
| `corner_margin` | `5%` | dead zone at each corner of a monitor's edge (not used beside a device) |
| `rotation` | empty | touch mode only: `0`, `90`, `180`, `270`, how far the screen as shown is turned from the touchscreen's own axes (see [MATH.md](MATH.md#touch)). A mouse needs none. It is not followed: set it again when the screen is turned |

A `diagonal_inch` line (and `monitor_diagonal_inch` in `[placement]`) from before 0.4.0 is
read and dropped: a device now covers the whole edge it sits next to, or the ratio of it
`length` gives.

### `[detected]`

What the device's screen is, typed or measured:

| Key | Default | |
| --- | --- | --- |
| `width`, `height` | empty | how far the cursor can travel across and down the screen in its natural orientation: mouse counts when measured with the calibration wizard, pixels when typed together with a `gain`. The daemon needs them to know where the cursor is, because a mouse only says how far it moved. With `real_width` and `real_height` given they are the counts, and the gain follows from them |
| `real_width`, `real_height` | empty | the screen's real size in pixels as it is shown (2 to 65536), typed. When both are given they are the screen the daemon works in, and the touchscreen is built from them. With the travel measured in counts they give the gain |
| `pointer_speed` | `0` | Android's pointer speed setting (-7 to 7) |
| `density` | empty | the density Android lays out with (`wm density`), in dpi |
| `accel` | empty | `off` or `on`: whether the device's pointer acceleration is on |
| `gain` | empty | pixels the cursor moves per count at low speed, typed ([MATH.md](MATH.md#gain)); empty: calculated from the real size and the measured travel, else from `pointer_speed` and `density` |

**What Android's pointer speed changes.** Only `gain`, which follows from it:
`gain = density / 320 * 0.64 * S[speed] / 10 * 3.19` ([MATH.md](MATH.md#gain)),
with `density` the value above. The `gain` key is for a gain you typed or
measured: left empty, the daemon uses that calculated value, so changing
`pointer_speed` changes the gain in use at once. The settings
window shows the calculation with its numbers (Measure tab). A `gain` in a file that is only what the speed and density give
is read as empty. The counts sent do not depend on any of this (the mouse's
movement is sent as it comes); the gain only tells the daemon where Android's
cursor is, for the return and the entry. Turn the speed up on the device and
the cursor moves further for the same mouse movement, as it would for any USB
mouse.

The device is not used until its size (`real_width` and `real_height`, else `width` and
`height`), `accel` and a gain (typed, from the real size and the measured travel, or
`pointer_speed` with `density`) are set; its status names those that are not. The Setup tab of
the settings window walks through them, and goes away once nothing is left to set.

### `[motion]` (device override)

Any of `sensitivity`, `sensitivity_y`, `scroll_sensitivity`, `entry`,
`resync_width`, `enter_push`, `return_push`, `no_cross_while_button`,
`report_rate_hz` from the global `[motion]` section: a device is paced by its own
`report_rate_hz` when its file has one, else by the computer's.

### `[mouse]`, `[keyboard]` and `[media]` (device override)

`enabled = true|false` in each: whether this device gets that input; a section
the file leaves out follows config.ini. A running device has the matching
input registered or removed on `reload`, without being opened again. One added
this way takes no input for about 150 ms, while Android registers it.

### `[touch]` (device override)

Any of `enabled`, `tap_button`, `swipe`, `scroll`, `scroll_pan`, `scroll_start_ms`, `scroll_steps`, `scroll_total_ms`, `scroll_overlap` and `scroll_release_ms`
from the global `[touch]` section; a key the file leaves out follows config.ini.
A running device has the touchscreen registered or removed on `reload`, as for
the mouse, keyboard and media keys; a change of the device's `real_width` or
`real_height` registers it again with the new size.

### `[mim]` and `[usb]` (a board)

Written for a device with `link = mim`, and read for no other. A `[usb]` value
left out takes the default in the table; one given is used exactly. `baud` and
`port` have none: they are facts of the hardware. The default ids are the
pid.codes test id 1209:0001, which is for in-house testing only and must not be
on a device that is redistributed, sold or manufactured; no other maker's id or
name is a default, and the device's status says `usb_test_id=yes` while it is in
use. `vendor_id` and `product_id` go together: both left out are the test id,
one given alone is not completed, and the board is not opened until the other is
given too (the status names it). A mouse's values can be copied from the settings
window; whatever the mouse does not give returns to its default.

| Key | Default | |
| --- | --- | --- |
| `baud` | empty | the serial line rate of this board, one of libmimhid's rates (115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000, 4000000). Adding the board from the search (`rescan`) writes the fastest rate the search saw the board and its serial adapter carry; a rate they do not carry is refused when the board is opened. Boards differ, so it is the board's own, not the computer's |
| `port` | empty | the board's serial port: `/dev/ttyUSB0`, a `/dev/serial/by-id/` path (it stays the same when the board is plugged in again) or `COM3`. The device's `serial` is `mim-` and the end of this, with anything a serial may not hold turned into `_`; a file whose serial is another is refused. It is set when the device is added and is not typed in the settings window |
| `vendor_id`, `product_id` | `0x1209`, `0x0001` | the ids the board announces to its target, 0x0001 to 0xFFFF |
| `manufacturer`, `product` | `Generic`, `USB Input Device` | the strings it announces, printable ASCII, at most 31 characters |
| `language_id` | `0x0409` | the language id (LANGID) of every string it announces, 0x0001 to 0xFFFF |
| `serial` | empty | the serial number it announces; empty announces none |
| `serial_from_chip_id` | `false` | announce the chip's own id as the serial number when `serial` is empty |
| `bcd_device` | empty | the device release number it announces, 0x0001 to 0xFFFF; empty announces 0x0100 |
| `max_power_ma` | empty | the current it says it draws, 1 to 500 mA; empty announces 100 |
| `self_powered`, `remote_wakeup` | `false` | the two bits of the configuration's attributes |
| `configuration` | empty | the configuration string it announces; empty announces none |

### `[usb.mouse]`, `[usb.keyboard]`, `[usb.media]`, `[usb.touch]` (a board's interfaces)

Each Node of a board is one USB interface of its composite device, and each
section holds that interface's own values. Every key is optional and is written
to the file only when set; one left out takes the ordinary value. The sections
need no `[usb]` values, and the same key means the same in all four.

| Key | Default | |
| --- | --- | --- |
| `interface_number` | empty | 0 to 255 and below the board's interface count; empty gives the lowest numbers no pinned Node uses, in the order mouse, keyboard, media, touchscreen. Nodes with the same number share one interface (one HID interface with a Report ID for each), and then every one of them must have its own `report_id`; without that the board is not opened and the status says which |
| `interface_name` | empty | the interface string, printable ASCII, at most 31 characters; empty announces none |
| `poll_interval_ms` | empty | how often the target polls, 1 to 255 ms; empty takes the ordinary interval |
| `ep_in_size`, `ep_out_size` | empty | endpoint sizes in bytes, any value from 1 to 64 as given, not rounded (the OUT endpoint also 0 for none); empty takes the ordinary sizes |
| `bcd_hid` | empty | the HID release number, 0x0001 to 0xFFFF |
| `country_code` | empty | 0 to 255 |
| `report_id` | empty | 1 to 255; empty gives the report no Report ID. Nodes that share an interface must all have one, all different; Nodes on interfaces of their own may use any, the same too. Nothing is numbered for you |

### `[clone]` (a board presents a plugged-in device)

Written by the settings window (Connection tab, "Clone a plugged-in device"), not typed:
it holds a snapshot of a USB device that was plugged into this computer, so the board
presents that device whether or not it is still plugged in. For a board only. While it
exists it replaces the `[usb]` and `[usb.*]` values of the device (they stay in the file
and apply again when the clone is removed), and the touchscreen is not available.

- The device level: `source` (what it was called), `vendor_id`, `product_id`, `bcd_device`,
  `manufacturer`, `product`, `serial` (empty: none), `self_powered`, `remote_wakeup`,
  `max_power_ma` (0 to 510), `battery` (0 to 100: the level reported when the device has a
  battery), and `notes` (what cannot be copied, separated by ` | `).
- `interface_0` to `interface_7`, one per HID interface, in order. Ten fields separated by
  a space: the interface number, boot subclass, boot protocol, `bcdHID` (0x0111), country
  code, IN endpoint size, OUT endpoint size (0: none), poll interval (ms), the interface
  name in hex (`-`: none), and the report descriptor in hex.

The mouse, keyboard and media switches decide which parts of the device are presented: a
part that is off is cut out of the report descriptors the other computer sees, and an
interface left with nothing is not presented. The input goes into the device's own reports:
one keyboard, one mouse and one media collection are fed, and a device without one has
nothing for that input. A battery is reported only when the device has one. What no
interface of the board can answer (a vendor interface, a Feature report above 32 bytes) is
presented but silent. The ids are another maker's: use this for your own testing, not on a
device that is redistributed or sold.

### `[touch_hid]` (the touchscreen's descriptor)

Per device only, for a board and for a phone. The values of the touchscreen
Node's HID descriptor; every key is optional and is written only when set, and
one left out takes the ordinary value of the link. x and y always follow the
real size: their range is `0` to the size minus 1, and `coordinate_bits` only
sets their width (a board: 16; a phone: the smallest width that holds the size).
An optional field (pressure, width, height, azimuth, scan time) is declared when
its `*_max` is given; its `*_bits` left out is the smallest width that holds the
maximum, and `*_bits` without `*_max` is refused. A change makes the Node register
again.

| Key | Default | |
| --- | --- | --- |
| `maximum_contacts`, `contacts_per_report` | 2, 2 | 1 to 16; the report carries no more than the screen has |
| `contact_id_max`, `contact_id_bits` | a phone 15, a board 1; both 4 bits | 0 to 255; 1 to 8 |
| `contact_count_max`, `contact_count_bits` | a phone 2 in 2 bits, a board 2 in 5 bits | 1 to 16; 1 to 8 |
| `coordinate_bits` | see above | 8 to 16 |
| `pressure_max`, `pressure_bits` | not declared | 1 to 65535; 1 to 16 |
| `width_max`, `width_bits` | not declared | 1 to 65535; 1 to 16 |
| `height_max`, `height_bits` | not declared | 1 to 65535; 1 to 16 |
| `azimuth_max`, `azimuth_bits` | not declared | 1 to 65535; 1 to 16 |
| `scan_time_max`, `scan_time_bits` | not declared | 1 to 65535; 1 to 16 |
| `scan_time_unit_100us` | empty | the scan time's unit in 100 microseconds, 1 to 65535; needs `scan_time_max` |

Adding or removing a Node (the mouse, keyboard, media keys or touchscreen) of
an open board makes the board detach from its target and attach again, which
the target sees as unplugging and plugging in a device. All the changes of one
`reload` are made in one such step, and a set that has not changed makes none.
A board has no ADB interface: `proxy = true` on it is refused.

### `[adb]` (device override)

| Key | Default | |
| --- | --- | --- |
| `proxy` | `false` | serve this device's ADB interface on its port |
| `port` | next free | local TCP port for this device's ADB proxy |

## Status

`hid_share status` prints `key=value` lines. It is answered from a snapshot
the daemon keeps, so asking never holds up the input. Per device the keys are
`device.<serial>.<key>`:

| Key | Meaning |
| --- | --- |
| `name`, `state`, `status` | what it is called; `ready`, `error` or `absent`; why it is not ready |
| `serial`, `link`, `proxy` | its serial; `aoa` or `mim`; the proxy port or `off` |
| `wanted`, `plugged` | `yes` when `connect` was asked for it and has not been undone; `yes` when the last scan saw it plugged in |
| `locked` | `yes` while its lock key holds the input on it; only for a device that is open |
| `size`, `rotation` | the values in use: pixels, degrees (the touch rotation) |
| `mode`, `gain` | `exact` or `curve`; pixels per count at low speed |
| `segment`, `beside` | monitor, side, crossing span, device span (PC pixels) |
| `position` | the tracked cursor range in the display's view space: x low, x high, y low, y high |
| `touch`, `touch_error` | `off`, `on`, or `not set:` and what the touchscreen still needs; with touch on, how far a tap can be from the cursor: the width of the tracked range, x then y |
| `clone`, `clone_notes` | a board only, while it presents a plugged-in device (clone mode): what it was called when read, and what of it cannot be copied (separated by `\|`) |
| `usb_test_id` | a board only: `yes` while it announces the test id 1209:0001 (in-house testing only), `no` when its ids are its own |
| `reports`, `merged`, `depth` | USB reports sent; inputs summed into a report that was already waiting; reports waiting now |
| `samples`, `queue_us`, `total_us` | over the last 256 reports (microseconds, as typical, 99 of 100, worst since opened): from an input reaching the daemon to the start of its USB call, and to the report's completion. The device's own delay is not included |
| `realtime` | whether the sender thread got a real-time priority (needs `RLIMIT_RTPRIO`) |

Top level: `protocol` (the version of this format), `state` (`pc`, `android`,
`paused`, `starting`), `active`, `last_used`, `backend`,
`local_hotkeys`, `monitor.N`, `new.<serial>`, `new_link.<serial>` (`aoa` or `mim`: how it
was found), `config_error`, `capture_error`,
`scan_error`.

## Commands

`hid_share <command>` talks to the daemon over a Unix socket in
`$XDG_RUNTIME_DIR` (a named pipe on Windows) that only the same user can open.

| Command | |
| --- | --- |
| `status` | state, backend, monitors, configured devices and newly detected devices; see [Status](#status) |
| `connect SERIAL\|NAME` | open a device. One that has a file is opened as it is; a plugged-in device that has none is given a file first (nothing is read from it). Nothing else is touched. The daemon opens no device by itself: not when it starts, not on `reload`, and not when a device that has a file shows up in a scan. A device that was connected by this command and drops out (unplugged, a glitch) is opened again when it comes back, until `disconnect` or `forget` |
| `disconnect SERIAL\|NAME` | close a device and leave it closed |
| `forget SERIAL\|NAME` | remove a device and its configuration file |
| `enter [SERIAL\|NAME]` | move the input to that device (or the last used device) |
| `release` | bring input back to the computer |
| `pause`, `resume` | pause or resume cursor crossing |
| `resync` | re-reference the corner reference on the next crossing |
| `rescan` | look for phones and boards now and wait for the result. A scan is made when the daemon starts and when asked (no automatic background scanning); the first one and the ones asked for also search the serial ports for boards ([Boards](#boards)); the scans after a failure, `reload` or `connect` do not. A scan sends a request to every USB device, so none is made while a device has the input |
| `reload` | read the configuration files again; a device that is open stays open unless how it was opened changed (the mouse buttons, the proxy, its port), and then it is opened again; its mouse, keyboard, media and touch inputs are added or removed without opening it again; no other device is opened |
| `media KEY [SERIAL\|NAME]` | send a media key (`previous`, `play_pause`, `next`, `brightness_up`, `brightness_down`, `volume_up`, `volume_down`); without a device it goes to the one that has the input, else the last one used |
| `probe SERIAL\|NAME corner` | send the device's cursor to its top left corner |
| `probe SERIAL\|NAME move DX DY` | move the cursor by raw counts, up to 30000 |
| `reports SERIAL\|NAME` | `ok`, then what the last reports to that device carried, one line each: `N MS REPORT TEXT` (the line's number, the clock in milliseconds, the report it went out in, and the call: mouse move/scroll/button, key, media, touch contact down/move/lift with its raw position). Recorded only for a few seconds after each ask |
| `export [--device SERIAL\|NAME] FILE` | export the whole configuration, or one device, as one text file |
| `import FILE` | check FILE, take it as the configuration or as one device, and reload; this computer's `[adb]` section and board ports are kept (a board it does not have comes in switched off) |
| `buttons` | count the buttons of connected mice |
| `quit` | stop the daemon |
| `paths` | show the settings, device, state and control paths |
