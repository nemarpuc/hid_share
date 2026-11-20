// SPDX-License-Identifier: MIT
// The control commands of Daemon::command that take more than a few lines.
// Every one runs on the capture thread (inside on_capture).
#include "daemon.hpp"

#include "store.hpp"

#include <algorithm>
#include <cmath>

namespace hids {
namespace {

// Counts that take a cursor to a display's edge from anywhere, and the most
// one `probe` may move.
constexpr int probe_reach = 30000;

} // namespace

std::string Daemon::rest_of(std::istringstream& from) {
    std::string text;
    std::getline(from, text);
    const size_t first = text.find_first_not_of(" \t");
    const size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

std::string Daemon::reload_files() {
    // The new files have to read well before anything is changed. A phone
    // that is open stays open unless how it was opened has changed.
    Config loaded;
    config_error_ = load_store(loaded);
    if (!config_error_.empty()) {
        publish_status();
        return "error=" + config_error_;
    }
    adopt_config(std::move(loaded));
    rebuild_phones();
    // The device thread scans (it also opens by what it found); a scan here
    // could replace the list it is about to open from.
    request_scan();
    return "ok";
}

// The display of a device was turned: "rotate 0|90|180|270|next [NAME]". The
// degrees are how far it is turned from the orientation its files were
// measured in, in the direction of the touch rotation; next adds 90. A name
// may hold spaces, so it is what is left of the line; none means the device
// that had the input last.
std::string Daemon::rotate_command(std::istringstream& words) {
    std::string turn_word;
    words >> turn_word;
    const std::string name = rest_of(words);
    const size_t target = name.empty() ? usual_phone() : phone_named(name);
    int wanted_turn = -1;
    if (turn_word == "0" || turn_word == "90" || turn_word == "180" || turn_word == "270")
        wanted_turn = std::stoi(turn_word);
    if (target >= phones_.size() || phones_[target]->link < 0)
        return "error=no such device, or it is not connected";
    if (turn_word != "next" && wanted_turn < 0)
        return "error=rotate 0|90|180|270|next [NAME]";
    Phone& phone = *phones_[target];
    phone.turn = turn_word == "next" ? (phone.turn + 90) % 360 : wanted_turn;
    // Lays every device out again, which gives the input back to the
    // computer and forgets where the cursors are.
    relayout();
    return "ok";
}

// One press of a media key, for a desktop shortcut to run where the backend
// cannot see the keyboard itself. "media KEY NAME" sends it to that device.
std::string Daemon::media_command(std::istringstream& words) {
    std::string name;
    words >> name;
    const std::string device = rest_of(words);
    const size_t target = device.empty() ? media_phone() : phone_named(device);
    for (const MediaKey& each : media_keys) {
        if (name != each.name)
            continue;
        if (!device.empty() && target >= phones_.size())
            return "error=no such device";
        if (!send_media(target, each.usage, true))
            return "error=no device is connected";
        static_cast<void>(send_media(target, each.usage, false));
        return "ok";
    }
    return "error=media previous|play_pause|next|brightness_up|brightness_down|"
           "volume_up|volume_down";
}

// Moves a connected device's cursor by raw counts, for measuring its display
// by eye when adb cannot be asked (docs/MATH.md), or taps a pixel of its
// screen as shown, to see how the touch is turned. "probe NAME corner",
// "probe NAME move DX DY" or "probe NAME tap X Y"; a name may hold spaces, so
// it is what is left of the line.
std::string Daemon::probe_command(std::istringstream& words) {
    std::string name = rest_of(words);
    std::string what;
    Delta counts{-probe_reach, -probe_reach};
    bool known = false;
    if (name.size() > 7 && name.compare(name.size() - 7, 7, " corner") == 0) {
        what = "corner";
        name.erase(name.size() - 7);
        known = true;
    } else {
        const size_t move = name.rfind(" move ");
        const size_t tap = name.rfind(" tap ");
        if (move != std::string::npos) {
            std::istringstream numbers(name.substr(move + 6));
            what = "move";
            known =
                static_cast<bool>(numbers >> counts.x >> counts.y) && (numbers >> std::ws).eof();
            name.erase(move);
        } else if (tap != std::string::npos) {
            std::istringstream numbers(name.substr(tap + 5));
            what = "tap";
            known =
                static_cast<bool>(numbers >> counts.x >> counts.y) && (numbers >> std::ws).eof();
            name.erase(tap);
        }
    }
    const size_t index = phone_named(name);
    Phone* phone =
        index < phones_.size() && phones_[index]->link >= 0 ? phones_[index].get() : nullptr;
    if (phone == nullptr)
        return "error=that device is not connected";
    if (active_ >= 0)
        return "error=bring the input back first";
    if (!known || std::abs(counts.x) > probe_reach || std::abs(counts.y) > probe_reach)
        return "error=probe DEVICE corner | move DX DY | tap X Y";
    if (what == "tap") {
        // The pixel is of the screen as shown, so it goes through the same
        // conversion as a click (docs/MATH.md, "Touch").
        if (!phone->link_options.touch || !phone->layout_ok)
            return "error=touch is not on for that device";
        if (counts.x < 0 || counts.y < 0 || counts.x >= phone->view.w || counts.y >= phone->view.h)
            return "error=that pixel is not on the screen";
        const TouchPoint at =
            to_touch_point(phone->view, touch_raw_size(phone->view, phone->rotation),
                           phone->rotation, counts.x, counts.y);
        Sink& sink = hub_.sink(phone->link);
        sink.touch(0, at.x, at.y, true);
        sink.touch(0, at.x, at.y, false);
        return "ok";
    }
    hub_.sink(phone->link).jump(counts);
    // The cursor is no longer where the session left it.
    if (phone->session != nullptr)
        phone->session->resync();
    return "ok";
}

// Opens a device. One that has a file is opened as it is; a plugged-in device
// that has none is given a file first (nothing is read from it). Nothing else
// is touched, and nothing is opened until this is asked for.
std::string Daemon::connect_command(std::istringstream& words) {
    const std::string name = rest_of(words);
    const size_t known = phone_named(name);
    if (known < config_.devices.size()) {
        const DeviceConfig& device = config_.devices[known];
        if (device.serial.empty())
            return "error=it waits for a device: connect a plugged-in one to give it "
                   "this profile";
        if (!device.enabled)
            return "error=the device is turned off";
        wanted_.insert(device.serial);
        request_scan();
        return "ok";
    }
    const auto plugged = std::find_if(new_.begin(), new_.end(),
                                      [&](const TargetInfo& info) { return info.serial == name; });
    // It names a file: nothing but a plain serial gets that far.
    if (!valid_serial(name))
        return "error=connect SERIAL|NAME";
    if (plugged == new_.end())
        return "error=no such device, and none with that serial is plugged in";
    if (active_ >= 0)
        leave();
    Config next = config_;
    // A profile that names no device is this one's now; otherwise it starts
    // from nothing.
    size_t index = next.devices.size();
    for (size_t i = 0; i < next.devices.size() && index == next.devices.size(); ++i) {
        if (next.devices[i].serial.empty())
            index = i;
    }
    if (index == next.devices.size())
        next.devices.emplace_back();
    DeviceConfig& device = next.devices[index];
    device.serial = name;
    device.enabled = true;
    // How it was found is how it is reached.
    device.link = plugged->transport == Transport::mim ? LinkKind::mim : LinkKind::aoa;
    if (plugged->transport == Transport::mim) {
        device.mim.port = plugged->detail;
        // The rate the search measured, unless one is already written.
        if (device.mim.baud == 0)
            device.mim.baud = plugged->baud;
        write_board_standard(device.mim);
    }
    const std::string problem = validate_config(next);
    if (!problem.empty())
        return "error=" + problem;
    if (!save_store(next))
        return "error=the device's file could not be written";
    wanted_.insert(name);
    return reload_files();
}

// Closes a device and leaves it closed, also when it is plugged in again.
std::string Daemon::disconnect_command(std::istringstream& words) {
    const size_t index = phone_named(rest_of(words));
    if (index >= phones_.size())
        return "error=no such device";
    Phone& phone = *phones_[index];
    wanted_.erase(phone.config.serial);
    if (phone.link >= 0) {
        if (active_ == static_cast<int>(index))
            leave();
        close_phone(phone);
        relayout();
    }
    publish_status();
    return "ok";
}

std::string Daemon::forget_command(std::istringstream& words) {
    const size_t index = phone_named(rest_of(words));
    if (index >= config_.devices.size() || index >= phones_.size())
        return "error=no such device";
    if (active_ >= 0)
        leave();
    Config next = config_;
    const std::string forgotten_serial = config_.devices[index].serial;
    wanted_.erase(forgotten_serial);
    next.devices.erase(next.devices.begin() + static_cast<std::ptrdiff_t>(index));
    // Whatever sat beside it goes back to a monitor's edge.
    for (DeviceConfig& other : next.devices) {
        if (other.beside == forgotten_serial)
            other.beside.clear();
    }
    return save_store(next) ? reload_files() : "error=the config could not be written";
}

} // namespace hids
