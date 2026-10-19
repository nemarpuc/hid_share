// SPDX-License-Identifier: MIT
#include "calibrate.hpp"

#include "hid_share/config.hpp"
#include "hid_share/probe.hpp"
#include "control.hpp"
#include "paths.hpp"
#include "store.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace hids {
namespace {

#if !defined(_WIN32)
class RawTerminal {
public:
    RawTerminal() {
        if (tcgetattr(STDIN_FILENO, &orig_) == 0) {
            termios raw = orig_;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
                active_ = true;
        }
    }

    ~RawTerminal() { restore(); }

    void restore() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
            active_ = false;
        }
    }

    int read_key(int timeout_ms) {
        pollfd fds[1];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        int ret = poll(fds, 1, timeout_ms);
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            unsigned char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1)
                return static_cast<int>(c);
        }
        return -1;
    }

private:
    termios orig_{};
    bool active_{false};
};
#else
class RawTerminal {
public:
    void restore() {}
    int read_key(int timeout_ms) {
        if (timeout_ms > 0)
            Sleep(timeout_ms);
        if (_kbhit())
            return _getch();
        return -1;
    }
};
#endif

int measure_axis(const std::string& target, const char* axis_name, RawTerminal& term) {
    std::string answer;
    // Reset cursor to top-left corner
    if (!control_request("probe " + target + " corner", answer) ||
        answer.rfind("error=", 0) == 0) {
        std::fprintf(stderr, "\nFailed to reset cursor: %s\n", answer.c_str());
        return -1;
    }

    const bool across = std::strcmp(axis_name, "Horizontal (Width)") == 0;
    ProbeAxis axis;
    // One move along the axis; false, with the reason printed, when it failed.
    const auto move = [&](const int counts) {
        const std::string by = std::to_string(counts);
        const std::string cmd = "probe " + target + " move " + (across ? by + " 0" : "0 " + by);
        if (!control_request(cmd, answer) || answer.rfind("error=", 0) == 0) {
            std::fprintf(stderr, "\nFailed to move cursor: %s\n", answer.c_str());
            return false;
        }
        return true;
    };

    std::printf("\n  Measuring %s:\n", axis_name);
    std::printf("  - SPACE or 1: press 1 count,  2: press 10,  3: press 100\n");
    std::printf("  - ENTER: a press of 1 moved nothing, so the cursor is at the edge\n");
    std::printf("  - Q to abort\n");
    std::printf("  * Press 100 until the next press would pass the edge, then 10, then 1. The\n");
    std::printf("    press of 1 that moves nothing is not counted. If a press passed the edge,\n");
    std::printf("    quit and measure again.\n\n");

    while (true) {
        const int key = term.read_key(20);
        if (key < 0)
            continue;

        if (key == 'q' || key == 'Q' || key == 27) { // 27 = ESC
            std::puts("\n  Calibration aborted by user.");
            return -1;
        }

        if (key == '\n' || key == '\r') {
            const int travel = axis.finish(1);
            if (travel > 0) {
                std::puts("\n");
                return travel;
            }
            std::puts("\n  The last press must be a 1 that moved nothing.");
            continue;
        }

        const int step = (key == ' ' || key == '1') ? 1 : key == '2' ? 10 : key == '3' ? 100 : 0;
        if (step > 0) {
            if (!move(step))
                return -1;
            axis.press(step);
        }

        std::printf("\r  -> Accumulated: %5d counts  ", axis.total());
        std::fflush(stdout);
    }
}

} // namespace

int run_calibrate(int argc, char** argv) {
    std::string target_name;
    if (argc >= 3)
        target_name = argv[2];

    Config current;
    std::string problem = load_store(current);
    if (!problem.empty()) {
        std::fprintf(stderr, "hid_share: %s\n", problem.c_str());
        return 1;
    }

    if (current.devices.empty()) {
        std::fputs("hid_share calibrate: no devices configured in settings.\n", stderr);
        return 1;
    }

    size_t dev_index = 0;
    if (!target_name.empty()) {
        dev_index = find_device(current, target_name);
        if (dev_index >= current.devices.size()) {
            std::fprintf(stderr, "hid_share calibrate: device '%s' not found.\n",
                         target_name.c_str());
            return 1;
        }
    }

    DeviceConfig& dev = current.devices[dev_index];
    const std::string dev_id = dev.serial.empty() ? dev.name : dev.serial;

    // Verify daemon is running
    std::string answer;
    if (!control_request("status", answer)) {
        std::fputs("hid_share calibrate: daemon is not running (start hid_share_daemon).\n",
                   stderr);
        return 1;
    }

    std::puts("======================================================================");
    std::puts(" hid_share Interactive Calibration Wizard");
    std::printf(" Target device: %s (serial: %s)\n", dev.name.c_str(), dev.serial.c_str());
    std::puts("======================================================================");
    std::puts("\n[NOTICE]");
    std::puts("  1. Before calibrating, please make sure 'Pointer Acceleration' /");
    std::puts("     'Enhance pointer precision' is turned OFF in your Android settings.");
    std::puts("     This ensures a constant linear gain across your display.");
    std::puts("  2. If you change pointer speed/settings on your device later,");
    std::puts("     re-calibration will be required.\n");

    std::printf("Press ENTER to begin calibration...");
    std::fflush(stdout);
    {
        int ch;
        while ((ch = std::getchar()) != '\n' && ch != EOF) {}
    }

    RawTerminal term;

    // Step 1: Measure horizontal
    std::puts("\n[Step 1/2] Width Measurement");
    int count_x = measure_axis(dev_id, "Horizontal (Width)", term);
    if (count_x <= 0) {
        term.restore();
        std::fputs("Calibration failed or aborted.\n", stderr);
        return 1;
    }

    // Step 2: Measure vertical
    std::puts("[Step 2/2] Height Measurement");
    int count_y = measure_axis(dev_id, "Vertical (Height)", term);
    if (count_y <= 0) {
        term.restore();
        std::fputs("Calibration failed or aborted.\n", stderr);
        return 1;
    }

    term.restore();

    const int comp_w = count_x + 1;
    const int comp_h = count_y + 1;

    std::puts("======================================================================");
    std::puts(" Calibration Summary");
    std::printf("   Target: %s\n", dev.name.c_str());
    std::printf("   Measured Counts: X = %d,  Y = %d (Accel = off)\n", count_x, count_y);
    std::puts("======================================================================");

    // The real size turns the counts into a gain; without it a count is the unit.
    int real_w = dev.real_width;
    int real_h = dev.real_height;
    std::printf("Real width of that screen in pixels as it is shown [%d, empty keeps it]: ", real_w);
    std::fflush(stdout);
    char text[32] = {0};
    if (std::fgets(text, sizeof(text), stdin) == nullptr)
        return 0;
    if (text[0] != '\n')
        real_w = std::atoi(text);
    std::printf("Real height in pixels [%d, empty keeps it]: ", real_h);
    std::fflush(stdout);
    if (std::fgets(text, sizeof(text), stdin) == nullptr)
        return 0;
    if (text[0] != '\n')
        real_h = std::atoi(text);
    const bool real = real_w >= 2 && real_h >= 2;
    if (real) {
        std::printf("   Gain = %.4g px per count (across alone %.4g, down alone %.4g)\n",
                    (real_w - 1.0 + real_h - 1.0) / (count_x + count_y),
                    (real_w - 1.0) / count_x, (real_h - 1.0) / count_y);
    } else {
        std::puts("   No real size: the gain is 1, the sizes stay counts and touch cannot be used.");
    }

    std::printf("Save these to device settings? [Y/n]: ");
    std::fflush(stdout);

    char choice[16] = {0};
    if (std::fgets(choice, sizeof(choice), stdin) == nullptr)
        return 0;

    if (choice[0] == '\n' || choice[0] == 'y' || choice[0] == 'Y') {
        dev.width = comp_w;
        dev.height = comp_h;
        dev.real_width = real ? real_w : 0;
        dev.real_height = real ? real_h : 0;
        dev.gain = real ? 0.0 : 1.0;
        dev.accel = AccelSetting::off;
        if (!save_store(current)) {
            std::fputs("Error: failed to save settings file.\n", stderr);
            return 1;
        }
        std::string reload_ans;
        static_cast<void>(control_request("reload", reload_ans));
        std::puts("Successfully saved and reloaded daemon!");
    } else {
        std::puts("Cancelled. Settings were not modified.");
    }

    return 0;
}

} // namespace hids
