# hid_share

Move the cursor past the edge of your screen and it continues on another machine, keyboard included. Push it back and it returns.

Each device is reached one of two ways:

- **A board (MIM):** a Raspberry Pi Pico or Pico 2 running [libmimusb](https://github.com/nemarpuc/libmimusb) firmware, between this computer (serial) and the other one (USB keyboard and mouse). Works with anything that takes a USB keyboard and mouse.
- **A phone (AOA):** an Android phone on its own USB cable, through [libaoahid](https://github.com/nemarpuc/libaoahid) (Android Open Accessory 2.0 HID). No extra hardware.

Linux (X11, Wayland), Windows and macOS.

> **Status:** the phone is used end to end with a Galaxy Tab S11 (Android 16) on Arch Linux, KDE Plasma (Wayland). The board is tested against libmimusb's simulated board only, not a real board. Windows and macOS are built in CI and not run on hardware. Details: [docs/USAGE.md](docs/USAGE.md#what-has-been-tested).

## Install

Download an archive from Releases: `linux-x86_64`, `linux-aarch64`, `macos-arm64`, `macos-x86_64`, `windows-x86_64`, `windows-arm64`, with `SHA256SUMS`. Linux needs glibc 2.35 or newer, plus `libusb-1.0`, `libdbus-1`, `libwayland-client` and `libxcb` from your distribution.

Or build:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/libaoahid;/path/to/libmimusb"
cmake --build build
ctest --test-dir build
```

Backends and the packages they need: [docs/PLATFORMS.md](docs/PLATFORMS.md).

## Use

1. Run `hid_share_gui` and press the button at the bottom left to start the daemon.
2. Press the refresh icon above the device list. A phone or board appears as `+ ...`; press it.
3. Follow its **Setup** tab, step by step.
4. Push the cursor through the edge. Push back through the same edge to return.

Phone setup, board setup, shortcuts and the ADB proxy: [docs/USAGE.md](docs/USAGE.md).

## Documents

| | |
|---|---|
| [docs/USAGE.md](docs/USAGE.md) | Setup, shortcuts, delay, ADB proxy, what has been tested |
| [docs/CONFIG.md](docs/CONFIG.md) | Every setting and command |
| [docs/PLATFORMS.md](docs/PLATFORMS.md) | Backends, permissions, limits |
| [docs/COEXISTENCE.md](docs/COEXISTENCE.md) | Sharing a screen edge with the desktop's own edge features |
| [docs/MATH.md](docs/MATH.md) | How the other cursor's position is tracked |

## License

MIT. Vendored: [aoahid_adb_proxy](third_party/licenses/aoahid_adb_proxy-LICENSE.txt) (MIT), [doctest](tests/third_party/doctest.h) (MIT), and the Wayland protocol descriptions under `protocols/` (their own notices, in each file).
