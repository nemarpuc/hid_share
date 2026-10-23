// SPDX-License-Identifier: MIT
#include "setup.hpp"

#include "app.hpp"
#include "field.hpp"
#include "hid_share/keys.hpp"
#include "actions.hpp"
#include "hid_share/accel_model.hpp"
#include "look.hpp"
#include "mice.hpp"
#include "placement.hpp"
#include "tabs.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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
// registered the HID devices. The daemon says "serial" of a device it holds
// open, whether or not the screen is set yet (which is what makes it ready).
[[nodiscard]] bool accepted(const App& app, const DeviceConfig& device) {
    return status_of(app, device, "state") == "ready" || !status_of(app, device, "serial").empty();
}

// The values the daemon needs of a screen before it can place it (the same as
// Daemon::measure()).
[[nodiscard]] std::string screen_missing(const DeviceConfig& device) {
    std::string list;
    const auto lack = [&](const char* name) {
        list += (list.empty() ? "" : ", ") + std::string(name);
    };
    if (view_width(device) < 2)
        lack("width");
    if (view_height(device) < 2)
        lack("height");
    if (effective_gain(device) <= 0.0)
        lack("gain");
    if (device.accel == AccelSetting::automatic)
        lack("pointer acceleration");
    return list;
}

[[nodiscard]] bool placed(const App& app, const DeviceConfig& device) {
    return !status_of(app, device, "segment").empty() || !status_of(app, device, "beside").empty();
}

// A line of the daemon's status that is not about one device.
[[nodiscard]] std::string top_status(const App& app, const char* const key) {
    const auto found = app.status.find(key);
    return found != app.status.end() ? found->second : std::string();
}

// "the right edge of HDMI-A-2" from a status line "HDMI-A-2 right lo hi alo ahi" (or "beside"
// with the serial of the device it sits beside); empty when there is none.
[[nodiscard]] std::string edge_in_words(const App& app, const DeviceConfig& device) {
    std::string line = status_of(app, device, "segment");
    const bool beside = line.empty();
    if (beside)
        line = status_of(app, device, "beside");
    std::vector<std::string> words;
    for (size_t at = 0; at < line.size();) {
        const size_t end = line.find(' ', at);
        words.push_back(line.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    if (words.size() < 6)
        return {};
    std::string name;
    for (size_t at = 0; at + 5 < words.size(); ++at)
        name += (name.empty() ? "" : " ") + words[at];
    const std::string& side = words[words.size() - 5];
    return "the " + side + " edge of " + (beside ? "the device " : "monitor ") + name;
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

// One button that does what is needed: unsaved changes are saved first (they are
// what the daemon is to open the device with), then the device is connected.
void connect_button(App& app, const DeviceConfig& device, const bool allowed) {
    ImGui::Indent(look::px(26));
    ImGui::BeginDisabled(!app.daemon_running || !allowed || device.serial.empty());
    if (ImGui::Button(app.dirty ? "Save and connect" : "Connect")) {
        if (app.dirty)
            apply(app);
        if (!app.failed) {
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

void row_for_key(App& app, DeviceConfig& device, const char* const section, const char* const name,
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
    // Both ids left out are the test id (mim_of); one alone is not completed.
    need((device.mim.vendor_id != 0) == (device.mim.product_id != 0),
         "vendor id and product id together");
    return list;
}

namespace {

// What a mouse or touchscreen announces is what the board announces: a value it
// does not give is taken off. One that cannot be taken keeps the parser's reason
// on its row.
void copy_identity(App& app, DeviceConfig& device, const Mouse& mouse) {
    const auto put = [&](const char* const name, const std::string& value) {
        const ConfigKey& key = *find_config_key(KeyScope::device, "usb", name);
        return value.empty() ? clear_field(app, key, &device) : set_field(app, key, &device, value);
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
    app.message = all ? "Copied. Connect the board to use it."
                      : "Copied what could be; the rows with a reason were refused.";
}

[[nodiscard]] std::string describe(const Mouse& mouse) {
    char text[192];
    std::snprintf(text, sizeof text, "%s: %s  (%04x:%04x)",
                  mouse.kind == PointerKind::mouse ? "mouse" : "touchscreen", mouse.name.c_str(),
                  mouse.vendor_id, mouse.product_id);
    return text;
}

// The mice and touchscreens plugged in here, read again every couple of seconds
// while they are on show.
void refresh_mice(App& app) {
    const double now = ImGui::GetTime();
    if (now - app.mice_read_at < 2.0)
        return;
    app.mice = list_mice();
    app.mice_read_at = now;
}

} // namespace

void draw_copy_mouse(App& app, DeviceConfig& device) {
    if (ImGui::Button("Copy from a plugged-in mouse or touchscreen...")) {
        app.mice = list_mice();
        app.mice_read_at = ImGui::GetTime();
        ImGui::OpenPopup("copy_mouse");
    }
    if (!ImGui::BeginPopup("copy_mouse"))
        return;
    if (app.mice.empty())
        ImGui::TextUnformatted("No USB mouse or touchscreen found (this works on Linux only).");
    for (const Mouse& mouse : app.mice) {
        if (ImGui::Selectable(describe(mouse).c_str()))
            copy_identity(app, device, mouse);
    }
    ImGui::EndPopup();
}

namespace {

[[nodiscard]] std::string clone_name(const CloneSource& source) {
    char ids[16];
    std::snprintf(ids, sizeof ids, "%04x:%04x", source.vendor_id, source.product_id);
    const std::string name = source.product.empty() ? std::string(ids) : source.product;
    return name + " (" + ids + ")";
}

// Reads the device again from the kernel and keeps it as the board's clone.
void take_clone(App& app, DeviceConfig& device, const CloneSource& source) {
    CloneSnapshot snapshot;
    if (const std::string problem = read_clone_device(source, sysfs_usb_devices, snapshot.device);
        !problem.empty()) {
        app.failed = true;
        app.message = problem;
        return;
    }
    if (snapshot.device.interfaces.size() > clone_max_interfaces) {
        app.failed = true;
        app.message = "That device has more HID interfaces than a board can hold.";
        return;
    }
    snapshot.source = clone_name(source);
    snapshot.battery = device.clone ? device.clone->battery : 100;
    device.clone = std::move(snapshot);
    app.dirty = true;
    app.failed = false;
    app.message = "Read. Apply, then disconnect and connect the board.";
}

} // namespace

void draw_clone(App& app, DeviceConfig& device) {
    look::dim("The board presents a USB device that is plugged into this computer as its own: "
              "its ids, interfaces, report descriptors and Report IDs, read as they are. Only "
              "its looks are copied; nothing answers the maker's own software. The ids are "
              "another maker's: for your own testing, not for a device that is distributed or "
              "sold.");
    if (ImGui::Button("Clone a plugged-in device...")) {
        app.clone_sources = list_clone_sources();
        app.clone_read_at = ImGui::GetTime();
        ImGui::OpenPopup("clone_pick");
    }
    if (ImGui::BeginPopup("clone_pick")) {
        if (ImGui::GetTime() - app.clone_read_at > 2.0) {
            app.clone_sources = list_clone_sources();
            app.clone_read_at = ImGui::GetTime();
        }
        if (app.clone_sources.empty())
            ImGui::TextUnformatted("No USB device with a HID interface found (Linux only).");
        for (const CloneSource& source : app.clone_sources) {
            char line[256];
            std::snprintf(line, sizeof line, "%s   %s, %u HID interface%s",
                          clone_name(source).c_str(), source.manufacturer.c_str(),
                          source.hid_interfaces, source.hid_interfaces == 1 ? "" : "s");
            if (ImGui::Selectable(line))
                take_clone(app, device, source);
        }
        ImGui::EndPopup();
    }
    if (!device.clone)
        return;

    ImGui::SameLine();
    if (ImGui::Button("Read again")) {
        const CloneDevice& was = device.clone->device;
        bool found_again = false;
        for (const CloneSource& source : list_clone_sources()) {
            if (source.vendor_id == was.vendor_id && source.product_id == was.product_id) {
                take_clone(app, device, source);
                found_again = true;
                break;
            }
        }
        if (!found_again) {
            app.failed = true;
            app.message = "That device is not plugged in here.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove the clone")) {
        device.clone.reset();
        app.dirty = true;
        return;
    }

    CloneSnapshot& clone = *device.clone;
    ImGui::Spacing();
    ImGui::Text("Presenting: %s", clone.source.c_str());
    const CloneRoles roles{mouse_of(app.config, device), keyboard_of(app.config, device),
                           media_of(app.config, device)};
    CloneReports reports;
    if (const std::string problem = CloneReports::build(clone.device, roles, reports);
        !problem.empty()) {
        ImGui::TextWrapped("ERR: %s", problem.c_str());
        return;
    }
    const size_t presented = reports.nodes().size();
    ImGui::Text("%zu of %zu interfaces are presented; the mouse, keyboard and media switches "
                "(Inputs tab) cut the parts that are off out of them.",
                presented, clone.device.interfaces.size());
    bool moved = false;
    for (size_t i = 0; i < presented; ++i)
        moved = moved || clone.device.interfaces[reports.nodes()[i].source].number != i;
    if (moved)
        look::dim("An interface left out moves the ones after it down by one: a board numbers "
                  "its interfaces without gaps.");
    if (presented == 0)
        look::dim("Nothing is presented: switch an input on.");
    const auto where = [&](const bool have, const char* what) {
        ImGui::BulletText("%s: %s", what, have ? "fed" : "not on the device, or switched off");
    };
    where(reports.has_keyboard(), "Keyboard keys");
    where(reports.has_media(), "Media keys");
    where(reports.has_mouse(), "Mouse");
    if (reports.has_battery()) {
        const size_t source = reports.nodes()[reports.battery_node()].source;
        ImGui::BulletText("Battery: in interface %u, as %s report",
                          static_cast<unsigned>(clone.device.interfaces[source].number),
                          reports.battery_is_feature() ? "a Feature (read on request)"
                                                       : "an Input");
        int level = static_cast<int>(clone.battery);
        look::row("Battery level (%)", nullptr, look::px(260));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("##clone_battery", &level, 1, 10)) {
            clone.battery = static_cast<uint32_t>(std::clamp(level, 0, 100));
            app.dirty = true;
        }
        look::row_end();
    } else {
        ImGui::BulletText("Battery: the device has none, so none is presented");
    }
    if (!clone.device.notes.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("What cannot be copied:");
        for (const std::string& note : clone.device.notes)
            ImGui::BulletText("%s", note.c_str());
    }
    look::dim("Touch mode is not available while a clone is presented. A change here takes the "
              "board off its target and puts it back: apply it, then disconnect and connect "
              "the board.");
}

bool setup_complete(const App& app, const DeviceConfig& device) {
    return found(app, device) && accepted(app, device) &&
           (!is_board(device) || board_missing(device).empty()) && screen_missing(device).empty() &&
           placed(app, device);
}

bool needs_setup(const DeviceConfig& device) {
    return device.link == LinkKind::unset || (is_board(device) && !board_missing(device).empty()) ||
           !screen_missing(device).empty();
}

std::string mark_state(const App& app, const DeviceConfig& device) {
    const std::string state = status_of(app, device, "state");
    if (state == "ready" || !device.enabled)
        return state;
    if (is_board(device) && !board_missing(device).empty())
        return "error";
    const std::string reason = status_of(app, device, "status");
    if (status_of(app, device, "wanted") == "yes" && !reason.empty() && reason != "not connected")
        return "error";
    return state;
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

    // This computer's side: the desktop has to let the daemon take the mouse and keyboard.
    if (!top_status(app, "capture_error").empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("ERR: this computer's input could not be set up: %s",
                           top_status(app, "capture_error").c_str());
    } else if (top_status(app, "state") == "starting") {
        ImGui::Spacing();
        look::dim("The daemon is still starting. If the desktop asks for permission to capture\n"
                  "input, answer its dialog.");
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
                "dialout), and that the board runs the libmimhid 1.3.3 firmware.");
    } else {
        explain("Plug the phone in and press the refresh icon above the list.");
    }

    // What a board shows to the other computer.
    if (board) {
        const std::string missing = board_missing(device);
        step(++number, "What the other computer sees", missing.empty());
        if (missing.empty()) {
            const MimDevice shown = mim_of(device);
            const std::string name = shown.manufacturer + " " + shown.product;
            explain(name.c_str());
        } else {
            explain("The board shows itself to the other computer as a USB device with a name\n"
                    "and ids. One click copies them from a mouse you have plugged in here.");
            ImGui::Indent(look::px(26));
            ImGui::TextWrapped("Not set: %s", missing.c_str());
            refresh_mice(app);
            if (app.mice.empty())
                look::dim("No USB mouse or touchscreen found here (this works on Linux only).");
            int shown = 0;
            for (size_t at = 0; at < app.mice.size() && shown < 4; ++at, ++shown) {
                ImGui::PushID(static_cast<int>(at));
                if (ImGui::Button(("Use " + describe(app.mice[at])).c_str()))
                    copy_identity(app, device, app.mice[at]);
                ImGui::PopID();
            }
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

    // How big the screen is: measured once with the wizard.
    const std::string lacking = screen_missing(device);
    step(++number, "Measure the other screen", lacking.empty());
    if (lacking.empty()) {
        char line[160];
        std::snprintf(line, sizeof line, "%d x %d px, gain %.4g", view_width(device),
                      view_height(device), effective_gain(device));
        explain(line);
    } else {
        ImGui::Indent(look::px(26));
        ImGui::TextWrapped("Not set: %s", lacking.c_str());
        ImGui::Unindent(look::px(26));
        ImGui::Indent(look::px(26));
        draw_probe(app, device);
        if (!app.probing) {
            if (ImGui::SmallButton("Enter the values by hand instead"))
                app.device_tab = static_cast<int>(DeviceTab::screen);
        }
        ImGui::Unindent(look::px(26));
    }

    // Where it sits next to the computer's monitors.
    step(++number, "Where it sits", placed(app, device));
    if (accepted(app, device) && lacking.empty()) {
        explain("Pick the monitor and the side it sits on, or drag it into place on the picture.");
        ImGui::Indent(look::px(26));
        draw_placement_basics(app, device);
        if (ImGui::SmallButton("Open the picture"))
            app.device_tab = static_cast<int>(DeviceTab::placement);
        ImGui::Unindent(look::px(26));
        if (!placed(app, device)) {
            const std::string why = status_of(app, device, "status");
            if (!why.empty() && why != "not connected") {
                ImGui::Indent(look::px(26));
                ImGui::TextWrapped("%s", why.c_str());
                ImGui::Unindent(look::px(26));
            }
        }
    } else {
        explain("Needs the steps above.");
    }

    // Using it.
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("    %d. Try it", ++number);
    if (!complete) {
        explain("Needs the steps above.");
    } else {
        const std::string edge = edge_in_words(app, device);
        const std::string hint =
            "Push the cursor out through " + (edge.empty() ? std::string("the edge it sits next to") : edge) +
            ": the mouse and keyboard go to the device.\nPush it back through the same edge to "
            "come back, or press the switch key.";
        explain(hint.c_str());
        row_for_key(app, device, "keys", "switch", "Switch key (optional)");
        ImGui::Indent(look::px(26));
        const bool trying = app.try_release_at > 0.0;
        ImGui::BeginDisabled(trying || !app.daemon_running);
        if (ImGui::Button("Try now: send the input here for 5 seconds")) {
            const std::string answer = command(app, "enter " + device.serial);
            app.failed = answer != "ok";
            app.message = app.failed ? (answer.rfind("error=", 0) == 0 ? answer.substr(6) : answer)
                                     : "The input is on the device. It comes back in 5 seconds.";
            if (!app.failed)
                app.try_release_at = ImGui::GetTime() + 5.0;
            poll_daemon(app);
        }
        ImGui::EndDisabled();
        ImGui::Unindent(look::px(26));
    }
}

void draw_new_devices(App& app) {
    if (app.fresh.empty())
        return;
    look::heading("Found");
    std::string chosen;
    for (const NewDevice& fresh : app.fresh) {
        const char* const kind = fresh.link == LinkKind::mim   ? "board"
                                 : fresh.link == LinkKind::aoa ? "phone"
                                                               : "device";
        ImGui::PushID(fresh.serial.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::Text("A %s: %s", kind, fresh.product.empty() ? fresh.serial.c_str() : fresh.product.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Set it up"))
            chosen = fresh.serial;
        ImGui::PopID();
    }
    // Adding a device changes what was found: the list above is done with.
    if (!chosen.empty())
        connect_device(app, chosen);
    ImGui::Spacing();
}

void draw_getting_started(App& app) {
    look::heading("Getting started");

    step(1, "The daemon runs", app.daemon_running);
    explain("It does the work; this window only edits what it uses. Start it with the button\n"
            "under the list. It looks for phones and boards as it starts.");

    step(2, "Add a phone or a board", !app.config.devices.empty());
    explain("What was found appears as \"+ name\"; click it to add it. Press the refresh icon\n"
            "to look again (every serial port is asked whether a board answers). The Setup tab\n"
            "of the device then says what is left to do.");
}

} // namespace gui
