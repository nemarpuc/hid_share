// SPDX-License-Identifier: MIT
#pragma once

#include "hid_share/config.hpp"
#include "hid_share/geometry.hpp"
#include "hid_share/probe.hpp"
#include "mice.hpp"
#include "tabs.hpp"

#include <imgui.h>

#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct GLFWwindow;

namespace gui {

// How often the daemon's status is read while the window is shown. The
// daemon answers from a snapshot, so asking never holds up the input.
inline constexpr double poll_seconds = 0.25;

// A device that is plugged in but has no file yet.
struct NewDevice {
    std::string serial;
    std::string product;
    // How the daemon found it.
    hids::LinkKind link{hids::LinkKind::unset};
};

// Everything the window shows: the config as it is being edited, and what
// the daemon last said. It holds no setting of its own; "Apply" writes the
// config and asks the daemon to read it again.
struct App {
    hids::Config config;
    // Non-empty: the files on disk could not be read. What is shown is not
    // what is on disk, so nothing is saved over it.
    std::string config_error;
    bool dirty{};

    // A row that was typed and refused, by row, with the parser's reason; and
    // the text being typed in a row.
    std::map<std::string, std::string> field_errors;
    std::map<std::string, std::string> editing;

    std::map<std::string, std::string> status;
    std::vector<hids::Monitor> monitors;
    std::vector<NewDevice> fresh;
    // The mice offered for a board's identity, and when they were read.
    std::vector<Mouse> mice;
    double mice_read_at{-10.0};
    // The plugged-in devices offered for a board to copy, and when they were read.
    std::vector<hids::CloneSource> clone_sources;
    double clone_read_at{-10.0};
    bool daemon_running{};
    double next_poll{};
    // The input was sent to a device to try it, and when it is taken back.
    double try_release_at{};

    // What is open: the computer's page, or the page of one device, and which
    // part of each page.
    bool computer{true};
    size_t device{};
    int computer_tab{};
    int device_tab{static_cast<int>(DeviceTab::device)};
    // Asked once before a device and its file are removed, and before the
    // daemon is stopped.
    std::string forgetting;
    bool quitting{};

    // The line at the bottom, and whether it reports a failure.
    std::string message;
    bool failed{};


    // The Reports part of Status: the lines of the device named by reports_serial, the
    // number of the last one taken, and whether the list is frozen.
    std::deque<std::string> reports;
    std::string reports_serial;
    long long reports_last{-1};
    bool reports_frozen{};
    double reports_next{};

    // Calibrating display gain: stepped from its top left corner.
    bool probing{};
    int probe_step{1}; // 1: measuring width, 2: measuring height, 3: completed
    std::string probe_serial;
    // The travel of each axis in counts, once its step is done.
    int probe_x{};
    int probe_y{};
    // The presses of the axis being measured, and how far the cursor was moved
    // down (or right) from the corner to be seen; both are put back after an undo.
    hids::ProbeAxis probe_axis;
    int probe_offset{};
    // The wizard has saved: the window moves on to where the device sits.
    bool probe_finished{};

    // Dragging a device in the picture: where along it the mouse took hold.
    int drag_offset{};
    std::string transfer_path;

    std::filesystem::path program_dir;
};

// app.cpp
void load_config(App& app);
void poll_daemon(App& app);
void apply(App& app);
// Sends one command; the answer without its line end, or why none came.
std::string command(App& app, const std::string& line);
void start_daemon(App& app);
void rescan_devices(App& app);
// Asks the daemon to look for boards on the serial ports, which it does only
// when asked.
void connect_device(App& app, const std::string& serial);
// What the daemon reports about a device, by its serial.
[[nodiscard]] std::string status_of(const App& app, const hids::DeviceConfig& device,
                                    const char* key);
// Whether a device's page is open. Devices are shown only while the daemon runs.
[[nodiscard]] bool has_selection(const App& app);
[[nodiscard]] std::vector<hids::Monitor> toolkit_monitors();
[[nodiscard]] std::filesystem::path program_directory();
void open_in_file_manager(const std::filesystem::path& path);

// A value set by something other than its own row (the picture, the probe):
// what was typed in that device's rows of `section`, and a refusal they hold,
// no longer say what the config says. Marks the config as edited.
void edited_directly(App& app, const std::string& serial, const std::string& section);

// layout_view.cpp
void draw_layout(App& app);

} // namespace gui
