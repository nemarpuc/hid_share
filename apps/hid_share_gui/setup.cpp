// SPDX-License-Identifier: MIT
#include "setup.hpp"

#include "actions.hpp"
#include "app.hpp"
#include "field.hpp"
#include "hid_share/accel_model.hpp"
#include "hid_share/keys.hpp"
#include "look.hpp"
#include "mice.hpp"
#include "placement.hpp"

#include <cstdio>
#include <string>

using namespace hids;

namespace gui {
namespace {

[[nodiscard]] bool is_board(const DeviceConfig& device) noexcept {
    return device.link == LinkKind::mim;
}

// The device is plugged in, or already open.
[[nodiscard]] bool found(const App& app, const DeviceConfig& device) {
    const std::string state = status_of(app, device, "state");
    return status_of(app, device, "plugged") == "yes" || state == "ready" || state == "error";
}

// The other end has taken it: a board's target has mounted it, a phone has
// registered the HID devices.
[[nodiscard]] bool accepted(const App& app, const DeviceConfig& device) {
    return status_of(app, device, "state") == "ready";
}

// The values the daemon needs of a screen before it can place it (the same as
// Daemon::measure()).
[[nodiscard]] std::string screen_missing(const DeviceConfig& device) {
    std::string list;
    const auto lack = [&](const char* name) {
        list += (list.empty() ? "" : ", ") + std::string(name);
    };
    if (device.width < 2)
        lack("width");
    if (device.height < 2)
        lack("height");
    if (device.rotation < 0)
        lack("rotation");
    if (device.diagonal_inch <= 0.0)
        lack("diagonal size");
    if (effective_gain(device) <= 0.0)
        lack("gain");
    if (device.accel == AccelSetting::automatic)
        lack("pointer acceleration");
    return list;
}

[[nodiscard]] bool placed(const App& app, const DeviceConfig& device) {
    return !status_of(app, device, "segment").empty() || !status_of(app, device, "beside").empty();
}

// A step: its mark, its number and its title.
void step(const int number, const char* const title, const bool done) {
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s %d. %s", done ? "[x]" : "[ ]", number, title);
}

// What to know about a step, under its title.
void explain(const char* const text) {
    ImGui::Indent(look::px(26));
    look::dim(text);
    ImGui::Unindent(look::px(26));
}

void connect_button(App& app, const DeviceConfig& device, const bool allowed) {
    ImGui::Indent(look::px(26));
    ImGui::BeginDisabled(!app.daemon_running || !allowed || device.serial.empty());
    if (ImGui::Button("Connect")) {
        if (app.dirty) {
            app.failed = true;
            app.message = "Apply or revert your changes first.";
        } else {
            const std::string answer = command(app, "connect " + device.serial);
            app.failed = answer != "ok";
            app.message = app.failed ? (answer.rfind("error=", 0) == 0 ? answer.substr(6) : answer)
                                     : "Connecting " + device.serial;
            poll_daemon(app);
        }
    }
    ImGui::EndDisabled();
    ImGui::Unindent(look::px(26));
}

// Why the daemon could not open the device, in its own words.
void why_not_open(const App& app, const DeviceConfig& device) {
    const std::string status = status_of(app, device, "status");
    if (status.empty() || status == "not connected")
        return;
    ImGui::Indent(look::px(26));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::Unindent(look::px(26));
}

void row_for(App& app, DeviceConfig& device, const char* const section, const char* const name,
             const char* const label) {
    if (const ConfigKey* const key = find_config_key(KeyScope::device, section, name)) {
        ImGui::Indent(look::px(26));
        field(app, *key, &device, label);
        ImGui::Unindent(look::px(26));
    }
}

} // namespace

std::string board_missing(const DeviceConfig& device) {
    std::string list;
    const auto need = [&](const bool given, const char* name) {
        if (given)
            return;
        list += (list.empty() ? "" : ", ") + std::string(name);
    };
    need(!device.mim.port.empty(), "serial port");
    need(device.mim.baud != 0, "serial speed");
    need(device.mim.vendor_id != 0, "vendor id");
    need(device.mim.product_id != 0, "product id");
    need(!device.mim.manufacturer.empty(), "manufacturer");
    need(!device.mim.product.empty(), "product");
    return list;
}

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

bool setup_complete(const App& app, const DeviceConfig& device) {
    if (!found(app, device) || !accepted(app, device) || !screen_missing(device).empty() ||
        !placed(app, device))
        return false;
    return !is_board(device) || board_missing(device).empty();
}

void draw_setup(App& app, DeviceConfig& device) {
    if (device.link == LinkKind::unset) {
        look::heading("Setup");
        look::dim("The way to reach this device is not set. Add link = aoa (a phone) or link = mim "
                  "(a board) to its file; nothing is guessed.");
        return;
    }
    const bool board = is_board(device);
    const bool complete = setup_complete(app, device);

    look::heading(complete ? "Ready" : "Setup");
    if (!app.daemon_running) {
        look::dim("Start the daemon first (the button under the list).");
    } else if (app.dirty) {
        look::dim("You have unsaved changes: press Apply (bottom right) for them to count.");
    }

    int number = 0;

    // Found.
    const bool plugged = found(app, device);
    step(++number, board ? "The board is found" : "The phone is found", plugged);
    if (plugged) {
        if (board) {
            char line[200];
            std::snprintf(line, sizeof line, "%s, %u baud", device.mim.port.c_str(),
                          device.mim.baud);
            explain(line);
        }
    } else if (board) {
        explain("Plug the board in and press the refresh icon above the list: every serial port\n"
                "is asked whether a board answers. If it does not appear, check the TX, RX and\n"
                "ground wires, the board's power, that you may open the port (group uucp or\n"
                "dialout), and that the board runs the libmimhid 1.0.1 firmware.");
    } else {
        explain("Plug the phone in and press the refresh icon above the list.");
    }

    // What a board shows to the other computer.
    if (board) {
        const std::string missing = board_missing(device);
        step(++number, "What the other computer sees", missing.empty());
        if (missing.empty()) {
            const std::string name = device.mim.manufacturer + " " + device.mim.product;
            explain(name.c_str());
        } else {
            explain("The board shows itself to the other computer as a USB device with a name\n"
                    "and ids. Copy them from a mouse you have plugged in here.");
            ImGui::Indent(look::px(26));
            ImGui::TextWrapped("Not set: %s", missing.c_str());
            draw_copy_mouse(app, device);
            ImGui::Unindent(look::px(26));
        }
    }

    // Open: the other end has accepted it.
    const bool ready = accepted(app, device);
    step(++number, board ? "The other computer has taken it" : "The phone has taken it", ready);
    if (ready) {
        explain(board ? "It found a new USB device, and the input can be moved to it."
                      : "The phone registered the keyboard, mouse and touch devices.");
    } else {
        explain(board ? "Plug the board's USB cable into the other computer and press Connect.\n"
                        "Ready means that computer has found a new USB device. It may take a few\n"
                        "seconds; if it never does, the computer is asleep, or the cable carries\n"
                        "power only."
                      : "Unlock the phone and press Connect. Allow the USB request on the phone\n"
                        "if it asks. A running adb server holds the phone: the ADB tab of the\n"
                        "Computer can stop it.");
        why_not_open(app, device);
        connect_button(app, device, plugged && (!board || board_missing(device).empty()));
    }

    // How big the screen is and how far a count moves the cursor.
    const std::string lacking = screen_missing(device);
    step(++number, "The other screen", lacking.empty());
    if (lacking.empty()) {
        char line[160];
        std::snprintf(line, sizeof line, "%d x %d, gain %.4g px per count", device.width,
                      device.height, effective_gain(device));
        explain(line);
    } else {
        explain("The size of the screen and how far one mouse count moves the cursor. Measure\n"
                "both with the wizard below, and enter what it does not set.");
        ImGui::Indent(look::px(26));
        ImGui::TextWrapped("Not set: %s", lacking.c_str());
        ImGui::Unindent(look::px(26));
        row_for(app, device, "placement", "rotation", "Rotation of the screen now");
        row_for(app, device, "detected", "diagonal_inch", "Diagonal size");
        row_for(app, device, "detected", "accel", "Pointer acceleration");
        ImGui::Indent(look::px(26));
        if (ImGui::TreeNodeEx("Measure the size and the gain", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_probe(app, device);
            ImGui::TreePop();
        }
        ImGui::Unindent(look::px(26));
    }

    // Where it sits next to the computer's monitors.
    step(++number, "Where it sits", placed(app, device));
    if (ready && lacking.empty()) {
        explain("The cursor leaves this computer's monitor through the edge it sits next to.");
        ImGui::Indent(look::px(26));
        draw_placement_basics(app, device);
        ImGui::Unindent(look::px(26));
    } else {
        explain("Needs the steps above.");
    }

    // Using it.
    ImGui::Spacing();
    ImGui::Indent(look::px(26));
    ImGui::TextUnformatted("Use it");
    ImGui::Unindent(look::px(26));
    explain("Push the cursor out through the edge: the mouse and keyboard go to the device.\n"
            "To come back, push the cursor back through the same edge, or press the switch\n"
            "key (Keys tab).");
}

void draw_getting_started(App& app) {
    look::heading("Getting started");
    const bool any_device = !app.config.devices.empty() || !app.fresh.empty();

    step(1, "The daemon runs", app.daemon_running);
    explain("It does the work; this window only edits what it uses. Start it with the button\n"
            "under the list.");

    step(2, "Search for phones and boards", any_device);
    explain("Press the refresh icon above the list. Every serial port is asked whether a board\n"
            "answers, so this is done only when you press it. What it finds appears as\n"
            "\"+ name\".");

    step(3, "Add one", !app.config.devices.empty());
    explain("Click the \"+ name\". It is added and opened where it can be, and its Setup tab\n"
            "says what is left to do.");
}

} // namespace gui
