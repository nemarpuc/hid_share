// SPDX-License-Identifier: MIT
#include "page.hpp"

#include "actions.hpp"
#include "hid_share/accel_model.hpp"
#include "hid_share/keys.hpp"
#include "app.hpp"
#include "field.hpp"
#include "look.hpp"
#include "placement.hpp"
#include "setup.hpp"
#include "status_view.hpp"
#include "tabs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <span>
#include <string>

using namespace hids;

namespace gui {
namespace {

enum class ComputerTab { general, hid, motion, adb, tools, status, beta };
struct ComputerTabEntry {
    const char* label;
    ComputerTab id;
};
constexpr ComputerTabEntry computer_tabs[] = {
    {"General", ComputerTab::general}, {"Inputs", ComputerTab::hid},
    {"Pointer", ComputerTab::motion},  {"ADB (Android)", ComputerTab::adb},
    {"Tools", ComputerTab::tools},     {"Status", ComputerTab::status},
    {"Beta", ComputerTab::beta}};

struct DeviceTabEntry {
    const char* label;
    DeviceTab id;
};
constexpr DeviceTabEntry device_tabs[] = {
    {"Setup", DeviceTab::setup},       {"Connection", DeviceTab::device},
    {"Measure", DeviceTab::screen},    {"Layout", DeviceTab::placement},
    {"Inputs", DeviceTab::hid},        {"Shortcuts", DeviceTab::keys},
    {"Pointer", DeviceTab::motion},    {"Tools", DeviceTab::tools},
    {"Status", DeviceTab::status},     {"Beta", DeviceTab::beta}};

[[nodiscard]] bool is_board(const DeviceConfig& device) noexcept {
    return device.link == LinkKind::mim;
}

// The tabs the page shows. Setup is there while something is left to set, and
// stays while it is the open tab, so that it does not vanish under the reader
// the moment its last step is done.
struct TabList {
    std::array<DeviceTabEntry, std::size(device_tabs)> entries{};
    size_t count{};
};

[[nodiscard]] TabList tabs_of(const App& app, const DeviceConfig& device) noexcept {
    TabList list;
    const bool setup = needs_setup(device) || app.device_tab == static_cast<int>(DeviceTab::setup);
    for (const DeviceTabEntry& entry : device_tabs) {
        if ((entry.id != DeviceTab::setup || setup) && (entry.id != DeviceTab::beta || app.config.beta))
            list.entries[list.count++] = entry;
    }
    return list;
}

[[nodiscard]] const char* kind_tag(const LinkKind link) noexcept {
    return link == LinkKind::mim ? "MIM" : link == LinkKind::aoa ? "AOA" : "link not set";
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
void draw_gain_line(App& app, DeviceConfig& device) {
    const double in_use = effective_gain(device);
    char text[300];
    switch (gain_source(device)) {
    case GainSource::none:
        look::dim("Gain in use: not known yet (measure the travel, or type the gain).");
        return;
    case GainSource::typed:
        std::snprintf(text, sizeof text, "Gain in use: %.4g px per count (typed).", in_use);
        look::dim(text);
        if (const double measured = measured_gain(device);
            measured > 0.0 && std::fabs(measured - in_use) > 0.01 * measured) {
            std::snprintf(text, sizeof text,
                          "The real size and the measured travel give %.4g instead.", measured);
            look::dim(text);
            if (ImGui::SmallButton("Use that: drop the typed gain")) {
                device.gain = 0.0;
                edited_directly(app, device.serial, "detected");
            }
        }
        return;
    case GainSource::measured:
        std::snprintf(text, sizeof text,
                      "Gain in use: (%d - 1 + %d - 1) / (%d - 1 + %d - 1) = %.4g px per count "
                      "(the real size over the measured travel).",
                      device.real_width, device.real_height, device.width, device.height, in_use);
        look::dim(text);
        {
            const double across = (device.real_width - 1.0) / (device.width - 1.0);
            const double down = (device.real_height - 1.0) / (device.height - 1.0);
            if (std::fabs(across - down) > 0.01 * in_use) {
                std::snprintf(text, sizeof text,
                              "Across alone it is %.4g and down alone %.4g: measure again.", across,
                              down);
                look::dim(text);
                // The screen may have been turned after it was measured.
                const double turned_across = (device.real_width - 1.0) / (device.height - 1.0);
                const double turned_down = (device.real_height - 1.0) / (device.width - 1.0);
                if (std::fabs(turned_across - turned_down) <= 0.01 * in_use) {
                    look::dim("Swapped, the two agree: the screen was probably turned after it "
                              "was measured. Measure again, or swap the travel.");
                    if (ImGui::SmallButton("Swap the travel across and down")) {
                        std::swap(device.width, device.height);
                        edited_directly(app, device.serial, "detected");
                    }
                }
            }
        }
        return;
    case GainSource::android:
        std::snprintf(text, sizeof text, "Gain in use: %d / 320 x %.4g x %.4g = %.4g px per count.",
                      device.density_dpi, pointer_speed_factor(device.pointer_speed),
                      first_segment_gain, in_use);
        look::dim(text);
        return;
    }
}

void draw_device_screen(App& app, DeviceConfig& device) {
    look::heading("Measure the travel");
    draw_probe(app, device);

    ImGui::Spacing();
    look::heading("Travel and gain");
    look::note("About these numbers",
               "The travel is how far the cursor can go across and down, in mouse counts when the\n"
               "wizard measured it. The daemon needs it to know where that cursor is: a mouse only\n"
               "ever says how far it moved. With the real size typed in, the gain (pixels one count\n"
               "moves the cursor) is the real size over the travel, both axes together. Type a gain\n"
               "only to override that; if you type the real pixel size as the width and height\n"
               "instead, the gain is the pixels one count moves the cursor.");
    draw_row(app, KeyScope::device, "detected", "width", &device, "Travel across");
    draw_row(app, KeyScope::device, "detected", "height", &device, "Travel down");
    draw_row(app, KeyScope::device, "detected", "gain", &device, "Gain (typed)");
    draw_gain_line(app, device);

    ImGui::Spacing();
    look::heading("About the other screen");
    look::dim("Its pointer acceleration.");
    draw_row(app, KeyScope::device, "detected", "accel", &device, "Pointer acceleration");

    if (ImGui::CollapsingHeader("Advanced")) {
        look::dim("Where the cursor waits on this computer, and where it re-enters the device:\n"
                  "Parked hides it in the bottom right corner and counts from there; Aligned keeps\n"
                  "it where it left the edge and counts from there.");
        draw_row(app, KeyScope::both, "motion", "entry", &device, "Cursor mode");
        if (!is_board(device)) {
            ImGui::Spacing();
            look::dim("For a phone, the gain can follow from Android's pointer speed and the\n"
                      "density it lays out with (docs/MATH.md) instead of being measured.");
            draw_row(app, KeyScope::device, "detected", "density", &device, "Screen density");
            draw_row(app, KeyScope::device, "detected", "pointer_speed", &device,
                     "Android pointer speed");
        }
    }
}

void draw_usb_values(App& app, DeviceConfig& device) {
    if (device.clone)
        look::dim("A clone is presented: its ids, strings and power values are used instead of "
                  "the ones below, which come back when the clone is removed.");
    look::dim("A row left empty takes the default shown in it. The default ids are the pid.codes "
              "test id 1209:0001, for in-house testing only; give both ids or neither.");
    draw_copy_mouse(app, device);
    ImGui::Spacing();
    draw_row(app, KeyScope::device, "usb", "vendor_id", &device, "Vendor id (0x0001-0xFFFF)");
    draw_row(app, KeyScope::device, "usb", "product_id", &device, "Product id (0x0001-0xFFFF)");
    draw_row(app, KeyScope::device, "usb", "manufacturer", &device, "Manufacturer");
    draw_row(app, KeyScope::device, "usb", "product", &device, "Product");
    if (ImGui::CollapsingHeader("Advanced")) {
        draw_row(app, KeyScope::device, "usb", "language_id", &device, "Language id of the strings");
        draw_row(app, KeyScope::device, "usb", "serial", &device, "Serial number announced");
        draw_row(app, KeyScope::device, "usb", "serial_from_chip_id", &device,
                 "Use the chip's id as the serial number");
        draw_row(app, KeyScope::device, "usb", "configuration", &device, "Configuration string");
        draw_row(app, KeyScope::device, "usb", "bcd_device", &device,
                 "Device release (empty: 0x0100)");
        draw_row(app, KeyScope::device, "usb", "max_power_ma", &device,
                 "Current drawn (empty: 100)");
        draw_row(app, KeyScope::device, "usb", "self_powered", &device, "Self powered");
        draw_row(app, KeyScope::device, "usb", "remote_wakeup", &device, "Can wake the target");
    }
    if (ImGui::CollapsingHeader("USB interface of each input")) {
        look::dim("Each input is one USB interface of the board. A row left empty takes the "
                  "ordinary value.");
        const std::pair<const char*, const char*> nodes[] = {{"Mouse", "usb.mouse"},
                                                             {"Keyboard", "usb.keyboard"},
                                                             {"Media keys", "usb.media"},
                                                             {"Touchscreen", "usb.touch"}};
        for (const auto& [label, section] : nodes) {
            if (!ImGui::TreeNode(label))
                continue;
            for (const ConfigKey& key : config_keys()) {
                if (key.scope == KeyScope::device && std::strcmp(key.section, section) == 0)
                    field(app, key, &device);
            }
            ImGui::TreePop();
        }
    }
    look::dim("A change here takes the board off its target and puts it back: apply it, then "
              "disconnect and connect the board.");
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
        draw_row(app, KeyScope::device, "mim", "baud", &device, "Serial speed");
        const std::string missing = board_missing(device);
        if (!missing.empty())
            look::dim(("Not set: " + missing + ". It is not opened until they are.").c_str());
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Clone a plugged-in USB device",
                                    device.clone ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            draw_clone(app, device);
        // Open while the board still lacks something it announces.
        const ImGuiTreeNodeFlags open = missing.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen;
        if (ImGui::CollapsingHeader("What the board announces (USB)", open))
            draw_usb_values(app, device);
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
    if (touch_of(app.config, device).enabled)
        look::dim("Touch mode is on; its settings are in the Beta tab.");
}

void draw_beta_tab(App& app, DeviceConfig& device) {
    look::dim("Beta: touch mode, still being worked out. The settings of the other tabs do not "
              "depend on it.");
    look::note("About touch",
               "The left click becomes a tap where the cursor is, and the wheel swipes a second\n"
               "finger (the swipe rows do not change the mouse scroll). The touchscreen is built\n"
               "from the real size of the other screen as it is shown; unlike the mouse, its\n"
               "positions do not follow the screen's rotation by themselves, so the rotation is\n"
               "set here and again whenever the screen is turned. Use the test tap to check it.");
    draw_row(app, KeyScope::device, "touch", "enabled", &device, "Touch mode");
    draw_row(app, KeyScope::device, "detected", "real_width", &device, "Screen width");
    draw_row(app, KeyScope::device, "detected", "real_height", &device, "Screen height");
    draw_row(app, KeyScope::device, "placement", "rotation", &device, "Touch rotation");
    if (ImGui::Button("Turn touch 90 degrees")) {
        device.rotation = (std::max(device.rotation, 0) + 90) % 360;
        edited_directly(app, device.serial, "placement");
    }
    ImGui::SameLine();
    // A tap a quarter of the way in from the top left corner of the screen as
    // shown: a wrong rotation puts it in another corner.
    ImGui::BeginDisabled(!app.daemon_running || app.dirty || !has_real_size(device));
    if (ImGui::Button("Test tap near the top left")) {
        const std::string answer =
            command(app, "probe " + device.serial + " tap " + std::to_string(device.real_width / 4) +
                             " " + std::to_string(device.real_height / 4));
        app.failed = answer != "ok";
        app.message = app.failed ? (answer.rfind("error=", 0) == 0 ? answer.substr(6) : answer)
                                 : "Tapped a quarter in from the top left corner.";
    }
    ImGui::EndDisabled();
    if (app.dirty)
        look::dim("Apply first: the test tap uses what the daemon runs with.");
    if (ImGui::CollapsingHeader("Touchscreen descriptor (HID)")) {
        look::dim("The values of the touchscreen's HID descriptor; x and y follow the real size. "
                  "A row left empty takes the ordinary value of the link. Pressure, width, height, "
                  "azimuth and scan time are declared when their maximum is given. A change makes "
                  "the touchscreen register again.");
        draw_group(app, "Touchscreen HID", &device);
    }
    draw_row(app, KeyScope::device, "touch", "tap_button", &device, "Tap button");
    draw_row(app, KeyScope::device, "touch", "swipe", &device, "Swipe with the wheel");
    if (ImGui::CollapsingHeader("Swipe settings")) {
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

// The place of the remembered tab in this device's tabs: the first when it has
// no such tab (Setup is gone once nothing is left to set).
[[nodiscard]] size_t tab_place(const App& app, const TabList& tabs) noexcept {
    for (size_t at = 0; at < tabs.count; ++at) {
        if (static_cast<int>(tabs.entries[at].id) == app.device_tab)
            return at;
    }
    return 0;
}

void draw_device_page(App& app, DeviceConfig& device) {
    const TabList tabs = tabs_of(app, device);
    switch (tabs.entries[tab_place(app, tabs)].id) {
    case DeviceTab::setup:
        draw_setup(app, device);
        break;
    case DeviceTab::device:
        draw_device_tab(app, device);
        break;
    case DeviceTab::screen:
        draw_device_screen(app, device);
        break;
    case DeviceTab::placement:
        draw_placement(app, device);
        break;
    case DeviceTab::hid:
        draw_hid_tab(app, device);
        break;
    case DeviceTab::beta:
        draw_beta_tab(app, device);
        break;
    case DeviceTab::keys:
        look::dim("Switch and the media keys work wherever the input is; lock and resync while "
                  "it is on this device.");
        draw_group(app, "Keys", &device);
        look::heading("Media");
        draw_group(app, "Media", &device);
        break;
    case DeviceTab::motion:
        draw_group(app, "Motion", &device);
        break;
    case DeviceTab::tools:
        draw_device_tools(app, device);
        break;
    case DeviceTab::status:
        draw_status(app, &device);
        if (ImGui::CollapsingHeader("Reports"))
            draw_reports(app, device);
        break;
    }
}

void draw_computer_page(App& app) {
    const size_t shown = std::size(computer_tabs) - (app.config.beta ? 0 : 1);
    const std::span<const ComputerTabEntry> tabs(computer_tabs, shown);
    const size_t at = std::min(static_cast<size_t>(std::max(app.computer_tab, 0)), tabs.size() - 1);
    switch (tabs[at].id) {
    case ComputerTab::general:
        draw_new_devices(app);
        if (app.config.devices.empty()) {
            draw_getting_started(app);
            ImGui::Spacing();
        }
        draw_group(app, "General", nullptr);
        draw_row(app, KeyScope::computer, "mouse", "buttons", nullptr, "Mouse buttons");
        draw_row(app, KeyScope::computer, "mouse", "button_map", nullptr, "Button map");
        break;
    case ComputerTab::hid:
        look::dim("What a device gets unless its own Inputs tab says otherwise.");
        draw_row(app, KeyScope::computer, "mouse", "enabled", nullptr, "Mouse");
        draw_row(app, KeyScope::computer, "keyboard", "enabled", nullptr, "Keyboard");
        draw_row(app, KeyScope::computer, "media", "enabled", nullptr, "Media keys");
        break;
    case ComputerTab::motion:
        draw_group(app, "Motion", nullptr);
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
    case ComputerTab::beta:
        look::dim("Beta: what a device gets unless its own Beta tab says otherwise.");
        draw_row(app, KeyScope::computer, "touch", "enabled", nullptr, "Touch mode");
        draw_row(app, KeyScope::computer, "touch", "tap_button", nullptr, "Tap button");
        draw_row(app, KeyScope::computer, "touch", "swipe", nullptr, "Swipe with the wheel");
        if (ImGui::CollapsingHeader("Swipe settings")) {
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
        const TabList tabs = tabs_of(app, *device);
        std::array<const char*, std::size(device_tabs)> names{};
        for (size_t each = 0; each < tabs.count; ++each)
            names[each] = tabs.entries[each].label;
        const int chosen = look::choose(names.data(), static_cast<int>(tabs.count),
                                        static_cast<int>(tab_place(app, tabs)));
        app.device_tab = static_cast<int>(tabs.entries[static_cast<size_t>(chosen)].id);
    } else {
        std::array<const char*, std::size(computer_tabs)> names{};
        int count = 0;
        for (const ComputerTabEntry& entry : computer_tabs) {
            if (entry.id != ComputerTab::beta || app.config.beta)
                names[static_cast<size_t>(count++)] = entry.label;
        }
        app.computer_tab = look::choose(names.data(), count, std::min(app.computer_tab, count - 1));
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
        std::string(look::mark(mark_state(app, device), device.enabled)) + " " +
        (device.serial.empty() ? std::string("(waits for a device)") : label_of(device));
    if (tag)
        label += std::string("  [") + kind_tag(device.link) + "]";
    ImGui::PushID(static_cast<int>(index));
    if (ImGui::Selectable(label.c_str(), has_selection(app) && app.device == index)) {
        app.computer = false;
        app.device = index;
        // A device with something left to set opens on its Setup, another leaves it.
        if (needs_setup(device))
            app.device_tab = static_cast<int>(DeviceTab::setup);
        else if (app.device_tab == static_cast<int>(DeviceTab::setup))
            app.device_tab = static_cast<int>(DeviceTab::device);
    }
    ImGui::PopID();
}

void draw_fresh_rows(App& app, const LinkKind kind) {
    for (const NewDevice& fresh : app.fresh) {
        if (fresh.link != kind)
            continue;
        const std::string label = "+ " + (fresh.product.empty() ? fresh.serial : fresh.product);
        ImGui::PushID(fresh.serial.c_str());
        const bool chosen = ImGui::Selectable(label.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: add it and open it", fresh.serial.c_str());
        ImGui::PopID();
        if (chosen) {
            // Adding it changes what is listed: this loop ends here.
            connect_device(app, fresh.serial);
            return;
        }
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
    if (look::icon_button_refresh("##rescan_devices",
                                  "Search for phones and boards. Every serial port is asked "
                                  "whether a board answers; ports in use are passed over."))
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
    ImGui::BeginDisabled(!app.config_error.empty() || !app.field_errors.empty());
    if (ImGui::Button("Apply"))
        apply(app);
    ImGui::EndDisabled();
}

} // namespace

void draw_window(App& app) {
    // A device that was sent the input to try it gives it back on time.
    if (app.try_release_at > 0.0 && ImGui::GetTime() >= app.try_release_at) {
        app.try_release_at = 0.0;
        const std::string answer = command(app, "release");
        app.failed = answer != "ok";
        app.message = app.failed ? answer : "The input is back on this computer.";
    }
    if (app.probe_finished) {
        app.probe_finished = false;
        app.device_tab = static_cast<int>(DeviceTab::placement);
    }
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
