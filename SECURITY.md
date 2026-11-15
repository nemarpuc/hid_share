# Security Policy

## Supported Versions

Only the [latest release](https://github.com/nemarpuc/hid_share/releases/latest) receives security fixes. Before 1.0.0 a fix may come with a change of the minor number.

## Scope

- **It reads and sends your input.** While input is on a device, the daemon captures this computer's keyboard and mouse and sends them to the device. Run it only on a machine you trust.
- **The command line talks to the daemon through a UNIX socket** created with mode 0600 (owner only).
- **The settings folder.** Settings name the `adb` program to run, so a `settings` folder that is not this user's own, or that others can write to, is not used; the user's own folder is used instead ([docs/CONFIG.md](docs/CONFIG.md#files)).
- **USB access.** On Linux the user needs access to the phone's USB device or the board's serial port; review the udev or group rule you install.
- **The ADB proxy** (phones, off by default) opens a port on `127.0.0.1`. Any local process can connect to it while it is on.

## Reporting a Vulnerability

Report vulnerabilities privately through the repository's [GitHub Security Advisories form](https://github.com/nemarpuc/hid_share/security/advisories/new). Do not put device serial numbers, ADB keys, or recordings of real input in a public issue.
