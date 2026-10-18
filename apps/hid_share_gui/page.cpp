// SPDX-License-Identifier: MIT
#include "page.hpp"

#include "actions.hpp"
#include "hid_share/accel_model.hpp"
#include "hid_share/keys.hpp"
#include "app.hpp"
#include "field.hpp"
#include "look.hpp"
#include "mice.hpp"
#include "placement.hpp"
#include "status_view.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <span>
#include <string>

using namespace hids;

namespace gui {
namespace {

enum class ComputerTab { general, hid, motion, board, adb, tools, status };
struct ComputerTabEntry {
    const char* label;
    ComputerTab id;
};
constexpr ComputerTabEntry computer_tabs[] = {
    {"General", ComputerTab::general}, {"HID", ComputerTab::hid},
    {"Motion", ComputerTab::motion},   {"Board (MIM)", ComputerTab::board},
    {"ADB (Android)", ComputerTab::adb}, {"Tools", ComputerTab::tools},
    {"Status", ComputerTab::status}};

enum class DeviceTab { device, keys, hid, usb, placement, screen, motion, tools, status, reports };
struct DeviceTabEntry {
    const char* label;
    DeviceTab id;
};
// A phone has its ADB on the Device tab and its screen read through adb; a
// board has the USB values it announces on a tab of its own.
constexpr DeviceTabEntry phone_tabs[] = {
    {"Device", DeviceTab::device},       {"Keys", DeviceTab::keys},
    {"HID", DeviceTab::hid},             {"Placement", DeviceTab::placement},
    {"Screen", DeviceTab::screen},       {"Motion", DeviceTab::motion},
    {"Tools", DeviceTab::tools},         {"Status", DeviceTab::status},
    {"Reports", DeviceTab::reports}};
constexpr DeviceTabEntry board_tabs[] = {
    {"Device", DeviceTab::device},       {"Keys", DeviceTab::keys},
    {"HID", DeviceTab::hid},             {"USB", DeviceTab::usb},
    {"Placement", DeviceTab::placement}, {"Screen", DeviceTab::screen},
    {"Motion", DeviceTab::motion},       {"Tools", DeviceTab::tools},
    {"Status", DeviceTab::status},       {"Reports", DeviceTab::reports}};

[[nodiscard]] bool is_board(const DeviceConfig& device) noexcept {
    return device.link == LinkKind::mim;
}

[[nodiscard]] std::span<const DeviceTabEntry> tabs_of(const DeviceConfig& device) noexcept {
    return is_board(device) ? std::span<const DeviceTabEntry>(board_tabs)
                            : std::span<const DeviceTabEntry>(phone_tabs);
}

[[nodiscard]] const char* kind_tag(const LinkKind link) noexcept {
    return link == LinkKind::mim ? "MIM" : link == LinkKind::aoa ? "AOA" : "link not set";
}

// What a board cannot be opened without and has not been given.
[[nodiscard]] std::string board_missing(const App& app, const DeviceConfig& device) {
    std::string list;
    const auto need = [&](const bool given, const char* name) {
        if (given)
            return;
        list += (list.empty() ? "" : ", ") + std::string(name);
    };
    need(app.config.mim.baud != 0, "serial speed (Computer, Board tab)");
    need(app.config.mim.timeout_ms != 0, "link timeout (Computer, Board tab)");
    need(!device.mim.port.empty(), "serial port");
    need(device.mim.vendor_id != 0, "vendor id");
    need(device.mim.product_id != 0, "product id");
    need(!device.mim.manufacturer.empty(), "manufacturer");
    need(!device.mim.product.empty(), "product");
    return list;
}

// Every row of a heading of the tables that belongs on this page.
void draw_group(App& app, const char* const group, DeviceConfig* const device) {
    for (const ConfigKey& key : config_keys()) {
        if (std::strcmp(key.group, group) != 0)
            continue;
        if (device != nullptr ? key.scope == KeyScope::computer : key.scope == KeyScope::device)
            continue;
        field(app, key, device);
    }
}

// One row by its place in the files, under a name that says what it is.
void draw_row(App& app, const KeyScope scope, const char* const section, const char* const key,
              DeviceConfig* const device, const char* const label) {
    if (const ConfigKey* const found = find_config_key(scope, section, key))
        field(app, *found, device, label);
}

// How to reach the proxy once it runs.
void draw_proxy_command(App& app, const DeviceConfig& device) {
    const std::string port = status_of(app, device, "proxy");
    // Only a plain port number goes into a command line to copy.
    if (port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string::npos)
        return;
    const std::string line = "adb connect 127.0.0.1:" + port;
    look::row("Reach it with", nullptr, look::px(260));
    ImGui::TextUnformatted(line.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy"))
        ImGui::SetClipboardText(line.c_str());
    look::row_end();
}

// What the gain in use comes to, with the numbers it is made of.
void draw_gain_line(const DeviceConfig& device) {
    const double in_use = effective_gain(device);
    if (in_use <= 0.0) {
        look::dim("Gain in use: not known yet (enter the density, or fill from adb).");
        return;
    }
    char text[240];
    if (device.gain > 0.0) {
        std::snprintf(text, sizeof text, "Gain in use: %.4g px per count (typed).", in_use);
    } else {
        std::snprintf(text, sizeof text, "Gain in use: %d / 320 x %.4g x %.4g = %.4g px per count.",
                      device.density_dpi, pointer_speed_factor(device.pointer_speed),
                      first_segment_gain, in_use);
    }
    look::dim(text);
}

void draw_device_screen(App& app, DeviceConfig& device) {
    look::heading("Display Resolution");
    look::dim(is_board(device)
                  ? "Screen size of the target in pixels, or total mouse travel counts when\n"
                    "calibrated. Configured automatically by the wizard below."
                  : "Screen size in pixels (when read via ADB) or total mouse travel counts\n"
                    "(when calibrated without ADB). Configured automatically by the wizard below.");
    draw_row(app, KeyScope::device, "detected", "width", &device, "Width");
    draw_row(app, KeyScope::device, "detected", "height", &device, "Height");

    ImGui::Spacing();
    look::heading("Pointer Gain (Sensitivity)");
    look::dim("Pixels moved on the device per 1 mouse count (e.g. 2.0 = 2 px per mouse step).");
    draw_row(app, KeyScope::device, "detected", "gain", &device, "Gain");
    draw_gain_line(device);

    ImGui::Spacing();
    draw_probe(app, device);

    ImGui::Spacing();
    look::heading("Cursor Idle / Entry Mode");
    look::dim("How cursor behaves when returning to PC and re-entering the device:\n"
              "- Parked: Hides in bottom-right corner while on PC, calculated from corner.\n"
              "- Aligned: Stays at last exit position on edge, calculated from there.");
    draw_row(app, KeyScope::both, "motion", "entry", &device, "Cursor mode");

    ImGui::Spacing();
    if (is_board(device)) {
        look::dim("A board's target is not read through adb: enter the size, or use the wizard.\n"
                  "Turn the pointer acceleration of the target off, so a count always moves the\n"
                  "cursor the same distance, and set it off here too.");
        draw_row(app, KeyScope::device, "detected", "diagonal_inch", &device, "Diagonal size");
        draw_row(app, KeyScope::device, "detected", "accel", &device, "Pointer acceleration");
        return;
    }
    if (ImGui::CollapsingHeader("Advanced / ADB details")) {
        const std::string proxy = status_of(app, device, "proxy");
        const bool proxy_ready = !proxy.empty() && proxy != "off";
        ImGui::BeginDisabled(!app.daemon_running || !proxy_ready || !app.filling.empty());
        if (ImGui::Button("Fill from ADB"))
            start_fill(app, device.serial);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!proxy_ready)
            look::dim("Connect the device and turn its ADB proxy on first.");
        else
            look::dim("Reads values automatically from Android via ADB.");

        ImGui::Spacing();
        draw_row(app, KeyScope::device, "detected", "diagonal_inch", &device, "Diagonal size");
        draw_row(app, KeyScope::device, "detected", "density", &device, "Screen density");
        draw_row(app, KeyScope::device, "detected", "pointer_speed", &device, "Android pointer speed");
        draw_row(app, KeyScope::device, "detected", "accel", &device, "Pointer acceleration");
        draw_row(app, KeyScope::device, "detected", "read_at", &device, "Last read at");
    }
}

void draw_device_tab(App& app, DeviceConfig& device) {
    draw_row(app, KeyScope::device, "device", "serial", &device, "Serial number");
    look::row("Reached by", nullptr, look::px(260));
    ImGui::TextUnformatted(is_board(device) ? "a board (MIM)"
                           : device.link == LinkKind::aoa ? "Android Open Accessory (AOA)"
                                                          : "not set");
    look::row_end();
    draw_row(app, KeyScope::device, "device", "name", &device, "Name");
    draw_row(app, KeyScope::device, "device", "enabled", &device, "In use");
    if (is_board(device)) {
        draw_row(app, KeyScope::device, "mim", "port", &device, "Serial port");
        const std::string missing = board_missing(app, device);
        if (!missing.empty())
            look::dim(("Not set: " + missing + ". It is not opened until they are.").c_str());
        return;
    }
    if (device.link == LinkKind::unset) {
        look::dim("The way to reach this device is not set. Add link = aoa (a phone) or link = mim "
                  "(a board) to its file; nothing is guessed.");
        return;
    }
    draw_row(app, KeyScope::device, "adb", "proxy", &device, "ADB proxy");
    draw_row(app, KeyScope::device, "adb", "port", &device, "ADB proxy port");
    draw_proxy_command(app, device);
}

void draw_hid_tab(App& app, DeviceConfig& device) {
    if (is_board(device)) {
        look::dim("Adding or removing one of these makes the board detach from its target and "
                  "attach again, which the target sees as unplugging a device. One change of "
                  "several is one such step; a setting left as it is makes none.");
    }
    draw_row(app, KeyScope::device, "mouse", "enabled", &device, "Mouse");
    draw_row(app, KeyScope::device, "keyboard", "enabled", &device, "Keyboard");
    draw_row(app, KeyScope::device, "media", "enabled", &device, "Media keys");
    look::heading("Touchscreen");
    look::dim("The left click becomes a tap where the cursor is, and the wheel swipes.");
    look::dim("Needs the width, height and rotation of the device (Screen tab).");
    look::dim("Swipe rows: the wheel swipes a second finger; they do not change the mouse scroll.");
    draw_row(app, KeyScope::device, "touch", "enabled", &device, "Touch mode");
    draw_row(app, KeyScope::device, "touch", "tap_button", &device, "Tap button");
    draw_row(app, KeyScope::device, "touch", "swipe", &device, "Swipe with the wheel");
    if (touch_of(app.config, device).swipe) {
        draw_row(app, KeyScope::device, "touch", "scroll", &device, "Swipe distance per notch");
        draw_row(app, KeyScope::device, "touch", "scroll_pan", &device,
                 "Swipe distance per notch, sideways");
        draw_row(app, KeyScope::device, "touch", "scroll_start_ms", &device,
                 "Swipe start delay (0 = next report)");
        draw_row(app, KeyScope::device, "touch", "scroll_steps", &device, "Swipe steps per notch");
        draw_row(app, KeyScope::device, "touch", "scroll_total_ms", &device,
                 "Swipe time, first to last step");
        draw_row(app, KeyScope::device, "touch", "scroll_overlap", &device,
                 "Next notch during a swipe");
        draw_row(app, KeyScope::device, "touch", "scroll_release_ms", &device,
                 "Swipe lift delay (0 = next report)");
    }
}

// Gives the board the identity of a mouse or touchscreen that is plugged in here: its ids
// and everything else it announces.
void draw_copy_mouse(App& app, DeviceConfig& device) {
    if (ImGui::Button("Copy from a plugged-in mouse or touchscreen...")) {
        app.mice = list_mice();
        ImGui::OpenPopup("copy_mouse");
    }
    if (!ImGui::BeginPopup("copy_mouse"))
        return;
    if (app.mice.empty())
        ImGui::TextUnformatted("No USB mouse or touchscreen found (this works on Linux only).");
    for (const Mouse& mouse : app.mice) {
        char label[192];
        std::snprintf(label, sizeof label, "%s: %s  (%04x:%04x)",
                      mouse.kind == PointerKind::mouse ? "mouse" : "touchscreen",
                      mouse.name.c_str(), mouse.vendor_id, mouse.product_id);
        if (!ImGui::Selectable(label))
            continue;
        // What the device announces is what the board announces: a value it does
        // not give is taken off. One that cannot be taken keeps the parser's reason
        // on its row.
        const auto put = [&](const char* const name, const std::string& value) {
            const ConfigKey& key = *find_config_key(KeyScope::device, "usb", name);
            return value.empty() ? clear_field(app, key, &device)
                                 : set_field(app, key, &device, value);
        };
        const auto hex = [](const unsigned value) {
            char text[16];
            std::snprintf(text, sizeof text, "0x%04x", value);
            return std::string(value != 0 ? text : "");
        };
        bool all = true;
        all &= put("vendor_id", hex(mouse.vendor_id));
        all &= put("product_id", hex(mouse.product_id));
        all &= put("manufacturer", mouse.manufacturer);
        all &= put("product", mouse.product);
        all &= put("serial", mouse.serial);
        all &= put("configuration", mouse.configuration);
        all &= put("bcd_device", hex(mouse.bcd_device));
        all &= put("max_power_ma",
                   mouse.max_power_ma != 0 ? std::to_string(mouse.max_power_ma) : std::string());
        all &= put("self_powered", mouse.self_powered ? "true" : "false");
        all &= put("remote_wakeup", mouse.remote_wakeup ? "true" : "false");
        app.failed = !all;
        app.message = all ? "Copied. Apply, then disconnect and connect the board."
                          : "Copied what could be; the rows with a reason were refused.";
    }
    ImGui::EndPopup();
}

void draw_usb_tab(App& app, DeviceConfig& device) {
    look::dim("What the board announces to its target as the USB device it is. None of these has "
              "a default, and no maker's name is assumed: the board is not opened while the "
              "ids and the two strings are empty.");
    draw_row(app, KeyScope::device, "usb", "vendor_id", &device, "Vendor id (0x0001-0xFFFF)");
    draw_row(app, KeyScope::device, "usb", "product_id", &device, "Product id (0x0001-0xFFFF)");
    draw_row(app, KeyScope::device, "usb", "manufacturer", &device, "Manufacturer");
    draw_row(app, KeyScope::device, "usb", "product", &device, "Product");
    draw_row(app, KeyScope::device, "usb", "serial", &device, "Serial number announced");
    draw_row(app, KeyScope::device, "usb", "serial_from_chip_id", &device,
             "Use the chip's id as the serial number");
    draw_row(app, KeyScope::device, "usb", "configuration", &device, "Configuration string");
    draw_row(app, KeyScope::device, "usb", "bcd_device", &device, "Device release (empty: 0x0100)");
    draw_row(app, KeyScope::device, "usb", "max_power_ma", &device, "Current drawn (empty: 100)");
    draw_row(app, KeyScope::device, "usb", "self_powered", &device, "Self powered");
    draw_row(app, KeyScope::device, "usb", "remote_wakeup", &device, "Can wake the target");
    look::dim("A change here takes the board off its target and puts it back: apply it, then "
              "disconnect and connect the board.");
    draw_copy_mouse(app, device);
}

// The place of the remembered tab in this device's tabs: the first when it has
// no such tab (a phone has no USB tab).
[[nodiscard]] size_t tab_place(const App& app, const DeviceConfig& device) noexcept {
    const std::span<const DeviceTabEntry> tabs = tabs_of(device);
    for (size_t at = 0; at < tabs.size(); ++at) {
        if (static_cast<int>(tabs[at].id) == app.device_tab)
            return at;
    }
    return 0;
}

void draw_device_page(App& app, DeviceConfig& device) {
    const std::span<const DeviceTabEntry> tabs = tabs_of(device);
    switch (tabs[tab_place(app, device)].id) {
    case DeviceTab::device:
        draw_device_tab(app, device);
        break;
    case DeviceTab::keys:
        look::dim("Switch and the media keys work wherever the input is; lock and resync while "
                  "it is on this device.");
        draw_group(app, "Keys", &device);
        look::heading("Media");
        draw_group(app, "Media", &device);
        break;
    case DeviceTab::hid:
        draw_hid_tab(app, device);
        break;
    case DeviceTab::usb:
        draw_usb_tab(app, device);
        break;
    case DeviceTab::placement:
        draw_placement(app, device);
        break;
    case DeviceTab::screen:
        draw_device_screen(app, device);
        break;
    case DeviceTab::motion:
        draw_group(app, "Motion", &device);
        break;
    case DeviceTab::tools:
        draw_device_tools(app, device);
        break;
    case DeviceTab::status:
        draw_status(app, &device);
        break;
    case DeviceTab::reports:
        draw_reports(app, device);
        break;
    }
}

void draw_computer_page(App& app) {
    const std::span<const ComputerTabEntry> tabs(computer_tabs);
    const size_t at = std::min(static_cast<size_t>(std::max(app.computer_tab, 0)), tabs.size() - 1);
    switch (tabs[at].id) {
    case ComputerTab::general:
        draw_group(app, "General", nullptr);
        draw_row(app, KeyScope::computer, "mouse", "buttons", nullptr, "Mouse buttons");
        draw_row(app, KeyScope::computer, "mouse", "button_map", nullptr, "Button map");
        break;
    case ComputerTab::hid:
        look::dim("What a device gets unless its own HID tab says otherwise.");
        draw_row(app, KeyScope::computer, "mouse", "enabled", nullptr, "Mouse");
        draw_row(app, KeyScope::computer, "keyboard", "enabled", nullptr, "Keyboard");
        draw_row(app, KeyScope::computer, "media", "enabled", nullptr, "Media keys");
        look::heading("Touchscreen");
        draw_row(app, KeyScope::computer, "touch", "enabled", nullptr, "Touch mode");
        draw_row(app, KeyScope::computer, "touch", "tap_button", nullptr, "Tap button");
        draw_row(app, KeyScope::computer, "touch", "swipe", nullptr, "Swipe with the wheel");
        if (app.config.touch.swipe) {
            draw_row(app, KeyScope::computer, "touch", "scroll", nullptr,
                     "Swipe distance per notch");
            draw_row(app, KeyScope::computer, "touch", "scroll_pan", nullptr,
                     "Swipe distance per notch, sideways");
            draw_row(app, KeyScope::computer, "touch", "scroll_start_ms", nullptr,
                     "Swipe start delay (0 = next report)");
            draw_row(app, KeyScope::computer, "touch", "scroll_steps", nullptr,
                     "Swipe steps per notch");
            draw_row(app, KeyScope::computer, "touch", "scroll_total_ms", nullptr,
                     "Swipe time, first to last step");
            draw_row(app, KeyScope::computer, "touch", "scroll_overlap", nullptr,
                     "Next notch during a swipe");
            draw_row(app, KeyScope::computer, "touch", "scroll_release_ms", nullptr,
                     "Swipe lift delay (0 = next report)");
        }
        break;
    case ComputerTab::motion:
        draw_group(app, "Motion", nullptr);
        break;
    case ComputerTab::board:
        look::dim("How every board is reached over its serial port. Neither value has a default: "
                  "a board is not opened while one is empty.");
        draw_row(app, KeyScope::computer, "mim", "baud", nullptr, "Serial speed");
        draw_row(app, KeyScope::computer, "mim", "timeout_ms", nullptr, "Link timeout");
        ImGui::Spacing();
        look::heading("Finding boards");
        look::dim("A board that has a file is opened on the port its file names, without "
                  "searching. Looking for others sends a message to every serial port that could "
                  "be a board, so it is done only when you ask, and not while a board is open.");
        ImGui::BeginDisabled(!app.daemon_running);
        if (ImGui::Button("Find boards"))
            find_boards(app);
        ImGui::EndDisabled();
        if (app.status.count("scan_error") != 0) {
            ImGui::Spacing();
            ImGui::TextWrapped("ERR: %s", app.status["scan_error"].c_str());
        }
        break;
    case ComputerTab::adb:
        look::dim("Only phones (AOA) use adb.");
        draw_row(app, KeyScope::computer, "adb", "kill_server", nullptr,
                 "Stop a running adb server");
        draw_row(app, KeyScope::computer, "adb", "path", nullptr, "The adb program");
        draw_row(app, KeyScope::computer, "adb", "first_port", nullptr, "First proxy port");
        break;
    case ComputerTab::tools:
        draw_actions(app);
        draw_files(app);
        break;
    case ComputerTab::status:
        draw_status(app, nullptr);
        break;
    }
}

void draw_main(App& app) {
    if (!app.config_error.empty()) {
        ImGui::TextWrapped("ERR: the settings files could not be read: %s. What is shown is not "
                           "what is on disk, so nothing is saved over it.",
                           app.config_error.c_str());
    }
    const bool device_page = !app.computer && has_selection(app);
    DeviceConfig* const device = device_page ? &app.config.devices[app.device] : nullptr;
    ImGui::TextUnformatted(device == nullptr        ? "Computer"
                           : device->serial.empty() ? "A profile that waits for a device"
                                                    : label_of(*device).c_str());
    if (device != nullptr) {
        ImGui::SameLine();
        look::dim((std::string("  ") + kind_tag(device->link)).c_str());
    }
    draw_summary(app, device);
    ImGui::Spacing();
    ImGui::PushID("tabs");
    if (device != nullptr) {
        const std::span<const DeviceTabEntry> tabs = tabs_of(*device);
        std::array<const char*, 10> names{};
        for (size_t each = 0; each < tabs.size(); ++each)
            names[each] = tabs[each].label;
        const int chosen = look::choose(names.data(), static_cast<int>(tabs.size()),
                                        static_cast<int>(tab_place(app, *device)));
        app.device_tab = static_cast<int>(tabs[static_cast<size_t>(chosen)].id);
    } else {
        std::array<const char*, 7> names{};
        for (size_t each = 0; each < std::size(computer_tabs); ++each)
            names[each] = computer_tabs[each].label;
        app.computer_tab = look::choose(names.data(), static_cast<int>(names.size()),
                                        app.computer_tab);
    }
    ImGui::PopID();
    ImGui::Spacing();
    if (device != nullptr)
        draw_device_page(app, *device);
    else
        draw_computer_page(app);
}

void draw_header(const App& app) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(app.daemon_running ? "daemon: running" : "daemon: not running");
    const auto backend = app.status.find("backend");
    if (backend != app.status.end()) {
        ImGui::SameLine();
        ImGui::Text("   backend: %s", backend->second.c_str());
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(app.dirty ? "   * unsaved changes" : "");
}

// One device as a row of the list; `tag` says how it is reached.
void draw_device_row(App& app, const size_t index, const bool tag) {
    const DeviceConfig& device = app.config.devices[index];
    std::string label =
        std::string(look::mark(status_of(app, device, "state"), device.enabled)) + " " +
        (device.serial.empty() ? std::string("(waits for a device)") : label_of(device));
    if (tag)
        label += std::string("  [") + kind_tag(device.link) + "]";
    ImGui::PushID(static_cast<int>(index));
    if (ImGui::Selectable(label.c_str(), has_selection(app) && app.device == index)) {
        app.computer = false;
        app.device = index;
    }
    ImGui::PopID();
}

void draw_fresh_rows(App& app, const LinkKind kind) {
    for (const NewDevice& fresh : app.fresh) {
        if (fresh.link != kind)
            continue;
        const std::string label = "+ " + (fresh.product.empty() ? fresh.serial : fresh.product);
        ImGui::PushID(fresh.serial.c_str());
        if (ImGui::Selectable(label.c_str()))
            connect_device(app, fresh.serial);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: add it and open it", fresh.serial.c_str());
        ImGui::PopID();
    }
}

void draw_list(App& app) {
    if (ImGui::Selectable("Computer", app.computer || !has_selection(app)))
        app.computer = true;
    ImGui::Separator();
    if (!app.daemon_running) {
        look::dim("Start the daemon to see the devices.");
        return;
    }
    const float side = ImGui::GetFrameHeight();
    ImGui::AlignTextToFramePadding();
    look::dim("devices");
    ImGui::SameLine();
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - side));
    if (look::icon_button_refresh("##rescan_devices", "Search for devices"))
        rescan_devices(app);

    // A device is shown as connected when it is plugged in or has been opened.
    std::vector<size_t> boards;
    std::vector<size_t> phones;
    std::vector<size_t> disconnected;
    for (size_t index = 0; index < app.config.devices.size(); ++index) {
        const DeviceConfig& device = app.config.devices[index];
        const bool plugged = status_of(app, device, "plugged") == "yes" ||
                             status_of(app, device, "state") == "ready" ||
                             status_of(app, device, "state") == "error";
        if (!plugged)
            disconnected.push_back(index);
        else if (device.link == LinkKind::mim)
            boards.push_back(index);
        else
            phones.push_back(index);
    }

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    look::dim("boards (MIM)");
    ImGui::SameLine();
    const float find_width = ImGui::CalcTextSize("Find").x + ImGui::GetStyle().FramePadding.x * 2.0F;
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - find_width));
    ImGui::PushID("find_boards");
    if (ImGui::SmallButton("Find"))
        find_boards(app);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Search the serial ports for boards. Done only when asked, and not "
                          "while a board is open.");
    ImGui::PopID();
    for (const size_t index : boards)
        draw_device_row(app, index, false);
    draw_fresh_rows(app, LinkKind::mim);

    ImGui::Spacing();
    look::dim("phones (AOA)");
    for (const size_t index : phones)
        draw_device_row(app, index, false);
    draw_fresh_rows(app, LinkKind::aoa);
    if (std::any_of(app.fresh.begin(), app.fresh.end(),
                    [](const NewDevice& each) { return each.link == LinkKind::unset; }))
        draw_fresh_rows(app, LinkKind::unset);

    if (!disconnected.empty()) {
        ImGui::Spacing();
        if (ImGui::TreeNode("Disconnected")) {
            for (const size_t index : disconnected)
                draw_device_row(app, index, true);
            ImGui::TreePop();
        }
    }
}

void draw_left(App& app) {
    const float buttons = sidebar_buttons_height(app);
    ImGui::BeginChild("list", ImVec2(0, -buttons));
    draw_list(app);
    ImGui::EndChild();
    draw_sidebar_buttons(app);
}

void draw_footer(App& app) {
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    if (app.message.empty())
        look::dim("ready");
    else
        ImGui::TextUnformatted(((app.failed ? "ERR: " : "OK: ") + app.message).c_str());

    // Saving is at the bottom right, where a form's buttons are.
    const float padding = ImGui::GetStyle().FramePadding.x * 2.0F;
    const float discard_width = ImGui::CalcTextSize("Discard changes").x + padding;
    const float apply_width = ImGui::CalcTextSize("Apply").x + padding;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - discard_width - apply_width -
                    ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Discard changes")) {
        load_config(app);
        app.failed = false;
        app.message = "Changes discarded.";
    }
    ImGui::SameLine();
    // Not held back by "unsaved": the first edit of a row is committed in the same
    // frame a click on this button starts, after this has been drawn.
    ImGui::BeginDisabled(!app.config_error.empty() || !app.field_errors.empty() || app.stale);
    if (ImGui::Button("Apply"))
        apply(app);
    ImGui::EndDisabled();
}

} // namespace

void draw_window(App& app) {
    continue_fill(app);
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw_header(app);
    ImGui::Separator();
    const float footer = ImGui::GetFrameHeightWithSpacing() + look::px(10);
    ImGui::BeginChild("left", ImVec2(look::px(190), -footer), ImGuiChildFlags_Borders);
    draw_left(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("main", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    draw_main(app);
    ImGui::EndChild();
    draw_footer(app);
    draw_confirmations(app);
    ImGui::End();
}

} // namespace gui
