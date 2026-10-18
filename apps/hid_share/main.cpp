// SPDX-License-Identifier: MIT
//
// hid_share: sends one command to the running daemon and prints its answer.
#include "hid_share/config.hpp"

#include "calibrate.hpp"
#include "control.hpp"
#include "paths.hpp"
#include "store.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void usage() {
    std::puts("usage: hid_share <command>\n"
              "\n"
              "  status              state, monitors and devices\n"
              "  enter [DEVICE]      move the input to a device (default: the last one used)\n"
              "  release             bring the input back to this computer\n"
              "  pause, resume       stop and restart crossing at the screen edge\n"
              "  resync              take the corner reference on the next crossing\n"
              "  connect DEVICE      open a device; a plugged-in one with no file gets one\n"
              "                      of its own; nothing is read from it. Nothing is opened\n"
              "                      until this is asked for\n"
              "  disconnect DEVICE   close a device and leave it closed\n"
              "  fill DEVICE         read a connected device through its ADB proxy (which\n"
              "                      has to be on) and write what it says into its file\n"
              "  calibrate [DEVICE]  interactive wizard to measure screen bounds via Space key\n"
              "  forget DEVICE       remove a device and its file\n"
              "  rescan              look for plugged-in devices now\n"
              "  find_boards         search the serial ports for boards that have no file\n"
              "  media KEY [DEVICE]  press a media key: previous, play_pause, next,\n"
              "                      brightness_up, brightness_down, volume_up,\n"
              "                      volume_down\n"
              "  probe DEVICE corner | move DX DY\n"
              "                      move a device's cursor by raw counts\n"
              "  export [--device DEVICE] FILE\n"
              "                      write the config, or one device, to FILE\n"
              "  import FILE         check FILE, take it as the config or as a device, reload\n"
              "  reload              read the config again\n"
              "  buttons             count the buttons of the connected mice\n"
              "  quit                stop the daemon\n"
              "  paths               print where the settings, the devices and the socket are\n"
              "\n"
              "DEVICE is a device's name or serial. The daemon is hid_share_daemon.\n"
              "See docs/CONFIG.md.");
}

} // namespace

int main(const int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-V") == 0) {
        std::puts("hid_share " HID_SHARE_VERSION);
        return 0;
    }
    if (std::strcmp(argv[1], "calibrate") == 0) {
        return hids::run_calibrate(argc, argv);
    }
    if (std::strcmp(argv[1], "paths") == 0) {
        std::printf("config=%s\ndevices=%s\nstate=%s\ncontrol=%s\n",
                    hids::config_file_path().c_str(), hids::devices_dir().c_str(),
                    hids::state_file_path().c_str(), hids::control_endpoint().c_str());
        return 0;
    }
    // "export [--device NAME] FILE" and "import FILE": everything, or one
    // device, as one text.
    const bool exporting = std::strcmp(argv[1], "export") == 0;
    if (exporting || std::strcmp(argv[1], "import") == 0) {
        const bool one = exporting && argc == 5 && std::strcmp(argv[2], "--device") == 0;
        if (argc != 3 && !one) {
            std::fputs("hid_share: export [--device NAME] FILE | import FILE\n", stderr);
            return 2;
        }
        const std::string file = argv[argc - 1];
        hids::Config current;
        std::string problem = hids::load_store(current);
        if (exporting) {
            if (!problem.empty()) {
                std::fprintf(stderr, "hid_share: %s\n", problem.c_str());
                return 1;
            }
            std::string text = hids::format_config(current);
            if (one) {
                const size_t index = hids::find_device(current, argv[3]);
                if (index >= current.devices.size()) {
                    std::fprintf(stderr, "hid_share: there is no device called %s\n", argv[3]);
                    return 1;
                }
                text = hids::format_device(current.devices[index]);
            }
            if (!hids::write_file(file, text)) {
                std::fprintf(stderr, "hid_share: %s could not be written\n", file.c_str());
                return 1;
            }
            return 0;
        }
        std::string text;
        if (!hids::read_file(file, text)) {
            std::fprintf(stderr, "hid_share: %s could not be read\n", file.c_str());
            return 1;
        }
        // One device's file adds that device, or replaces the one with its
        // serial; anything else is a whole config.
        hids::Config imported;
        hids::DeviceConfig device;
        if (hids::parse_device(text, device).empty() && !device.serial.empty()) {
            imported = current;
            const size_t index = hids::find_device(imported, device.serial);
            if (index < imported.devices.size())
                imported.devices[index] = device;
            else
                imported.devices.push_back(device);
            problem = hids::validate_config(imported);
        } else {
            problem = hids::parse_config(text, imported);
        }
        if (!problem.empty()) {
            std::fprintf(stderr, "hid_share: %s: %s\n", file.c_str(), problem.c_str());
            return 1;
        }
        // The adb program is run by the daemon, so a file from elsewhere
        // never chooses it or what it is used for: this computer's adb
        // settings stay.
        if (hids::keep_local_adb(imported, current))
            std::fputs("hid_share: adb settings are not imported; this computer's are kept\n",
                       stderr);
        if (!hids::save_store(imported)) {
            std::fputs("hid_share: the config could not be written\n", stderr);
            return 1;
        }
        // A daemon that is not running reads the files when it starts.
        std::string answer;
        if (hids::control_request("reload", answer) && answer.rfind("error=", 0) == 0) {
            std::fprintf(stderr, "hid_share: %s\n", answer.c_str());
            return 1;
        }
        return 0;
    }

    std::string line;
    for (int index = 1; index < argc; ++index) {
        if (index > 1)
            line += ' ';
        line += argv[index];
    }
    std::string answer;
    if (!hids::control_request(line, answer)) {
        std::fputs("hid_share: the daemon is not running (start hid_share_daemon)\n", stderr);
        return 1;
    }
    if (!answer.empty() && answer.back() != '\n')
        answer += '\n';
    std::fputs(answer.c_str(), stdout);
    return answer.rfind("error=", 0) == 0 ? 1 : 0;
}
