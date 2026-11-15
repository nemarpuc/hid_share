# Contributing

Use the [issue tracker](https://github.com/nemarpuc/hid_share/issues) for proposed changes and submit patches through [pull requests](https://github.com/nemarpuc/hid_share/pulls).

## Build and test

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/libaoahid;/path/to/libmimusb" -DHID_SHARE_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The board test (`hid_share.board`) runs against libmimusb's simulated board and is built only when
`-DHID_SHARE_MIMUSB_SOURCE_DIR=<libmimusb source>` and `-DHID_SHARE_TINYUSB_DIR=<TinyUSB checkout>` are given.

Format with `clang-format` (see `.clang-format`).

## Where things are

- `src/core`: arithmetic and config, covered by `tests/test_core.cpp` and described in [docs/MATH.md](docs/MATH.md).
- `src/capture`: one backend per way of reading this computer's input.
- `src/output`: the links to devices. A `Backend` finds targets and a `Port` is one open target (`aoa_port.cpp`, `mim_port.cpp`).
- `apps/`: `hid_share_daemon`, `hid_share` (CLI) and `hid_share_gui`.

## Submitting changes

- Add a test for a changed rule or setting; document a new setting in [docs/CONFIG.md](docs/CONFIG.md).
- Put a user-visible change in `CHANGELOG.md`.
- Say what was run. A claim that something works on hardware needs the device, the OS and the backend.

## Code of Conduct

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
