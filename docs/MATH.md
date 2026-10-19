# The arithmetic

Android gives a USB mouse no way to ask where the cursor is. hid_share
therefore keeps its own account of the position: it puts the cursor at a known
point once, then adds up everything it sends. This file is that account, in
the order the code applies it (`src/core/`).

## Coordinates

- **PC:** the desktop's pixel coordinates, x right, y down, as the capture
  backend reports them (logical pixels under a scaled Wayland output).
- **Android, "view" space:** the device's display as the user sees it, upright,
  in pixels, x right, y down, origin top left.

| Symbol | Meaning |
| --- | --- |
| `W, H` | width and height in view space |
| `t` | touch rotation: how far the display as shown is turned from the touchscreen's axes: 0, 90, 180, 270 (touch only) |
| `Wr, Hr` | the touchscreen's raw size: `W x H`, swapped when `t` is 90 or 270 |
| `da`, `dp` | pixels per millimetre of the device and of the PC monitor |
| `side` | which edge of the PC monitor the device sits at |
| `[a0, a1]` | the device's extent along that edge, in PC pixels (the anchor) |
| `[s0, s1]` | the part of the anchor the cursor can cross at (the segment) |
| `L` | the device's length along the shared edge: `H` for left/right, `W` for top/bottom |
| `G` | gain: pixels the device's cursor moves per count sent |

## Orientation

The mouse needs no rotation. Android turns a mouse's movement itself, so that
"right" on the mouse is "right" on the display in every rotation
([`CursorInputMapper::sync`](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/services/inputflinger/reader/mapper/CursorInputMapper.cpp)
calls `rotateDelta`). Confirmed on a Galaxy Tab S11 in rotation 90. The size
`W x H` is measured (or typed) in the mouse's own axes, that is the display as it
is shown, so the position is tracked without knowing how the device is turned
and nothing is sent turned.

## Touch

A touchscreen reports absolute positions in its own axes, `Wr x Hr` (the real
size `Rw x Rh`, swapped when `t` is 90 or 270; the touchscreen is built from it), and
Android does not turn them with the mouse: it turns the raw positions by the
inverse of the display's orientation to get the position on the rotated
display
([`TouchInputMapper::computeInputTransforms`](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/services/inputflinger/reader/mapper/TouchInputMapper.cpp),
steps 2 to 5, with the rotations of
[`ui::Transform::set`](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/ui/Transform.cpp)).
Nothing tells the host which way the display is turned, so `t` is set by hand:
turn it until a tap lands where the cursor is. Let `u = vx / (W - 1)` and
`v = vy / (H - 1)` be the tracked position as fractions of the display; the
conversion is the inverse of Android's:

| t | raw x (fraction) | raw y (fraction) |
| --- | --- | --- |
| 0 | u | v |
| 90 | 1 - v | u |
| 180 | 1 - u | 1 - v |
| 270 | v | 1 - u |

The raw position is the fraction times `Wr - 1` and `Hr - 1`, rounded and
clamped to the raw area. The tap position is the midpoint of the tracked range,
so it is as exact as the range is narrow (`exact` mode, or an edge that fixed
it). AOSP does this on floating-point positions and flips about the size rather
than the size less one, so with integer raw values the landing pixel can differ
by one in rotations 90, 180 and 270; that is far below what a tap needs. A
touchscreen that is external and has no display of its own to follow is put on
the internal display (`TouchInputMapper::findViewport`).

## Several monitors

Only the outline of all monitors together can be crossed. Where two monitors
touch, the cursor goes to the other monitor, as before.

For one edge of monitor `M`, every other monitor that touches that edge covers
part of it; what is left is the edge's outer spans. The segment is

```
segment = anchor ∩ (an outer span of the edge) − corner margin
```

taking the longest such piece. An anchor that touches no outer span is an
error. The corner margin (5% of the edge by default) keeps the segment away
from hot corners.

## Edge mapping

A PC coordinate `t` along the edge and a device coordinate `u` along its side:

```
u = (t - a0) * (L - 1) / (a1 - a0)
t = a0 + u * (a1 - a0) / (L - 1)        clamped to [s0, s1]
```

By default the anchor is as long as the device really is next to the monitor:

```
a1 - a0 + 1 = round(L / da * dp)        dp: the monitor's density along the shared edge
```

## Gain

`G` is how many pixels Android moves the cursor for one count. It is **not** 1.
Measured on a Galaxy Tab S11 (Android 16, display density 340) by reading the
position Android reports while a button is held:

| pointer speed | counts sent | pixels moved | G |
| --- | --- | --- | --- |
| 0 | 1000 | 2169.1 | 2.169 |
| 3 | 500 | 1410.0 | 2.820 |
| -4 | 500 | 650.7 | 1.301 |

All three match

```
G = 0.64 * S[speed + 7] / 10 * 3.19 * density / 320
S = {1, 2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20}
```

to four digits. `0.64 * S / 10` and `3.19` are AOSP's sensitivity factor and the
base gain of the first segment of its mouse acceleration curve
([`AccelerationCurve.cpp`](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/input/AccelerationCurve.cpp));
`density` is what `wm density` prints. On that device the gain did not change
with speed (acceleration is off there through Samsung's "Enhance pointer
precision" setting), slow and fast movements both gave 2.169.

**From the real size.** With acceleration off, the gain can also be had without
Android's numbers, from the screen's real size `Rw x Rh` (typed) and the travel
the calibration wizard measured in counts, `cx` across and `cy` down (the counts
that took the cursor from the top left pixel to the last pixel of each edge; the
config's `width` and `height` are `cx + 1` and `cy + 1`):

```
G = ((Rw - 1) + (Rh - 1)) / (cx + cy)
```

Both axes are taken together because a count moves the cursor the same distance
on both, so each axis's reading error of a few counts weighs less against the
longer total. When `G` over one axis alone differs from the other by more than
1%, one edge was passed or stopped short of, and the settings window says so.
The wizard presses 100, 10 and then single counts toward the far edge, never past
it (a press that passed it makes the count wrong, and the wizard is run again). The
press of 1 that moves nothing found the edge and is not counted, so `cx` is the
counts of the presses before it (`ProbeAxis`). The last press that moved may have
been clamped part-way, so `cx` is exact to within one step: `dG / G` is below `1 / cx`, 0.05 % at 2000 counts. With
`Rw x Rh` given, the daemon works in real pixels: the
screen is `W = Rw`, `H = Rh`, and `G` is the pixels one count moves the cursor.

With acceleration on, AOSP's curve applies: for a speed `v` in mm/s (counts
taken as an 800 CPI mouse),

```
G(v) = 0.64 * S[speed + 7] / 10 * (base + reciprocal / v) * density / 320
```

| v up to | base | reciprocal |
| --- | --- | --- |
| 32.002 | 3.19 | 0 |
| 52.83 | 4.79 | -51.254 |
| 119.124 | 7.28 | -182.737 |
| ∞ | 15.04 | -1107.556 |

The daemon runs in one of two modes, by the device's `accel`:

| Mode | When | Gain used |
| --- | --- | --- |
| `exact` | `accel = off` | the device's `gain`, one number |
| `curve` | `accel = on` | a range: `G(v/2)` to `G(2v)`, since Android measures `v` itself; the device's `gain` is the low-speed gain it is scaled from |

What has not been measured: the curve with acceleration on, the gain behind
AOSP's own acceleration switch (Android 16), and Android versions before 16.

## Sensitivity

The mouse's movement is sent as it comes. A movement `(px, py)` that the
capture backend reports (the mouse's counts, or the pixels of the PC's cursor,
whichever that backend has; [PLATFORMS.md](PLATFORMS.md)) becomes counts by the
sensitivity alone:

```
kx = sensitivity
ky = sensitivity_y                            (defaults to sensitivity)

vx = px * kx + carry_x
counts_x = trunc(vx);  carry_x = vx - counts_x          (same for y)
```

At the default sensitivity of 1 the counts sent are the numbers the backend
reported. The gain `G` is not in this: how far Android moves its cursor for a
count is Android's business, and the PC's screen and the device's are not
matched to each other. The carry keeps a sensitivity below 1 from losing slow
movement. The position below is computed from the counts actually sent, so no
setting makes it drift; a wrong `G` makes the estimate wrong by that much,
which shows as a return or an entry that is off by that distance, never as a
different feel.

## Position

The position is kept as a range per axis, `[xl, xh]` and `[yl, yh]`. For a
report of `(cx, cy)` counts with gain between `gl` and `gh`:

```
xl' = clamp(xl + min(gl * cx, gh * cx), 0, W - 1)
xh' = clamp(xh + max(gl * cx, gh * cx), 0, W - 1)
```

and the same for y. Android clamps the cursor to the display, so pushing
against an edge collapses the range to a point there. In `exact` mode the range
is always a point.

## Entering

With the device to the right of the PC, the shared edge is the device's left
edge, `x = 0`. The other sides are the same with the axes exchanged.

The target height is `ue`, the edge mapping of where the PC cursor crossed.

**From the parked corner** (normal: `entry = parked` and `exact` mode). While the
input is on the PC the cursor sits in the device's bottom right corner,
`(W - 1, H - 1)`, a point known exactly (see [Returning](#returning)). One
report takes it to the shared edge at the new height: the x part overshoots
and is clamped to the edge, the y part is counted from the corner.

```
N = ceil((max(W, H) + 64) / Gmin)
(-N, round((ue - (H - 1)) / G))
```

Nothing is carried over from the previous visit, so no error can build up from
one crossing to the next. The report is one only because the gain of `exact`
mode does not depend on a report's length; in the other modes every entry takes
the corner reference below.

**Aligning** (`entry = aligned`). The cursor is where it was left, on the shared
edge at `y_last`. One report moves it to the new height:

```
counts_y = round((ue - y_last) / G)
```

**Corner reference** (first entry, after a change of layout, when the range is
wider than `resync_width`, on request, or always with `entry = corner`). Two
reports, sent separately so Android clamps the first before applying the
second:

```
1.  N = ceil((max(W, H) + 64) / Gmin)
    (-N, -N) toward the top corner if ue < L / 2, else (-N, +N)
2.  counts_y = round(ue / G)  from the top corner,  -round((L - 1 - ue) / G) from the bottom
```

## Returning

After every report, let `d` be the distance of the range's far bound from the
shared edge before the report, and `out` the counts sent toward the PC:

```
past = gl * out - d
if past > 0:  push += past          (movement Android certainly clamped)
if out < 0:   push = 0
return when push >= return_push
```

`return_push` is 1 pixel in `exact` mode and 16 otherwise. Because only
certainly-clamped movement counts, an uncertain gain can delay the return but
never causes it early.

While a device is locked (its `lock` key) no push is counted and nothing
returns, toward the PC or toward a device beside it. The range still follows
the clamp, so the position stays known.

The PC cursor is put at `t` of the range's midpoint, one pixel inside the
edge, and that midpoint is kept as `y_last`.

With `entry = parked` one more report then sends the device's cursor to its bottom
right corner, `(+N, +N)` in Android's own axes. Android turns a mouse's
movement with the display, so this is the bottom right of what is shown in
every rotation.
The arrow's tip is its top left, so nearly all of it is off the display there,
and the clamp makes the position `(W - 1, H - 1)` exactly.

## Without adb

A relative mouse is never told the display's size, and AOA HID carries nothing
back from the device: the accessory driver's `raw_request`, the only path for
a report toward the host, is an empty function
([f_accessory.c](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.15/drivers/usb/gadget/function/f_accessory.c),
`acc_hid_raw_request`). So the size has to be given once: typed, or measured with the calibration wizard.

It does not have to be given in pixels. Every formula above uses a position
only as `pixels / G`, the counts from an edge, so a device can be described in
counts alone:

```
Cx = counts from the left edge to the right edge
Cy = counts from the top edge to the bottom edge
W = Cx + 1,  H = Cy + 1,  G = 1
```

`Cx` and `Cy` are measured by eye: the cursor is sent to the top left corner,
then stepped until the arrow, whose tip is its top left, has all but left the
display at the far edge. A miscount of `e` counts moves the far edge by `e`
counts in the model and nothing else; it does not grow, because every return
parks the cursor in a corner again. How much of the monitor's edge the device
covers is not taken from physical sizes: `length` gives it, in pixels or as a ratio of
the edge, and without it the device covers the whole edge.

This holds while `G` is one number, that is with pointer acceleration off on
the device.

## Checks

`tests/test_core.cpp` runs 48 combinations (4 sides × exact and uncertain gain
× parked or not × 3 sensitivities), 20 round trips each, against a model of Android's clamp and gain, and checks that the real
position stays inside the range, that entry lands within half a count's travel
in exact mode, and that a return only happens with the cursor really on the
edge.

A count is the smallest step there is, so the cursor can be put no closer than
`G / 2` pixels to a wanted height: half a pixel at a gain of 1, 1.08 pixels at
the 2.169 measured above. On the tablet two entries landed 0.9 and 0.6 pixels
from the computed height. The tracked position has no such error: it is
computed from the counts that were sent, not from the height that was wanted.
