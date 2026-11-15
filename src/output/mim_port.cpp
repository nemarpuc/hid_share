// SPDX-License-Identifier: MIT
// A microcontroller board on a serial port, through libmimusb. The board
// presents the Nodes to its own target as one USB HID device.
#include "hid_share/config.hpp"
#include "port.hpp"

#include <mimusb.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <mutex>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hids {
namespace {

// Every value below is this application's policy, stated explicitly rather
// than left to libmimusb's documented fallbacks (docs/API.md there).
// The search waits this long for each answer, at each rate it tries.
// How long one serial port is given to answer the search message.
constexpr uint32_t discover_timeout_ms = 50U;
// How long a queued report may keep the sender waiting for its fragments.
constexpr uint32_t drain_deadline_ms = 600U;
constexpr uint32_t touch_contacts = 2U;
constexpr uint32_t touch_bits = 16U;
// A mouse report carries movement the tracker has counted, so no report may be dropped: an IN
// endpoint waits for room (MIMUSB_FULL_WAIT) instead of discarding the newest or the oldest.
// The queue holds this many reports (it takes one slot more of the board's pool).
constexpr uint8_t endpoint_queue_depth = 4U;

// Consumer page usages the toggle Node declares; the key map only produces
// these. The same list and order as the phone side.
constexpr uint16_t media_usages[] = {0x00B5, 0x00B6, 0x00B7, 0x00CD, 0x00E2, 0x00E9, 0x00EA,
                                     0x0223, 0x0224, 0x0238, 0x0201, 0x006F, 0x0070};

mimusb_integer_field field(const int32_t minimum, const int32_t maximum,
                           const uint32_t bits) noexcept {
    mimusb_integer_field value{};
    value.logical_minimum = minimum;
    value.logical_maximum = maximum;
    value.bit_width = bits;
    return value;
}

// The Report ID of a Node: none unless the user gave one.
mimusb_report_id_option report_id_of(const NodeUsb& usb) noexcept {
    mimusb_report_id_option id{};
    if (usb.report_id) {
        id.enabled = 1U;
        id.value = static_cast<uint8_t>(*usb.report_id);
    }
    return id;
}

// What the interface says about itself to the other computer. Boot subclass and protocol are 0.
mimusb_hid_function function_of(const NodeUsb& usb) noexcept {
    mimusb_hid_function function{};
    function.bcd_hid = static_cast<uint16_t>(usb.bcd_hid.value_or(0U));
    function.country_code = static_cast<uint8_t>(usb.country_code.value_or(0U));
    return function;
}

mimusb_result create_mouse(const unsigned buttons, const NodeUsb& usb,
                           mimusb_spec** spec) noexcept {
    mimusb_mouse_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
    options.function = function_of(usb);
    options.button_count = buttons;
    options.x = field(-32767, 32767, 16);
    options.y = field(-32767, 32767, 16);
    options.enable_wheel = 1U;
    options.wheel = field(-127, 127, 8);
    options.enable_pan = 1U;
    options.pan = field(-127, 127, 8);
    return mimusb_spec_create_mouse(&options, spec);
}

mimusb_result create_keyboard(const NodeUsb& usb, mimusb_spec** spec) noexcept {
    mimusb_keyboard_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
    options.function = function_of(usb);
    // Through LANG and the keypad extras; the modifiers 0xE0-0xE7 are always
    // declared by the profile.
    options.usage_minimum = 0x04;
    options.usage_maximum = 0xDD;
    return mimusb_spec_create_keyboard(&options, spec);
}

mimusb_result create_toggle(const NodeUsb& usb, mimusb_spec** spec) noexcept {
    mimusb_toggle_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
    options.function = function_of(usb);
    options.application_page = 0x0CU;  // Consumer
    options.application_usage = 0x01U; // Consumer Control
    options.field_page = 0x0CU;
    // In media_usages order: Next, Prev, Stop, Play/Pause, Mute, Vol Up,
    // Vol Down, AC Home, AC Back, AC Pan, AC New, Brightness Up, Brightness Down.
    static const mimusb_usage_semantic semantics[] = {
        MIMUSB_USAGE_ONE_SHOT,  MIMUSB_USAGE_ONE_SHOT,          MIMUSB_USAGE_ONE_SHOT,
        MIMUSB_USAGE_ONE_SHOT,  MIMUSB_USAGE_ON_OFF_MAINTAINED, MIMUSB_USAGE_RETRIGGER,
        MIMUSB_USAGE_RETRIGGER, MIMUSB_USAGE_ONE_SHOT,          MIMUSB_USAGE_ONE_SHOT,
        MIMUSB_USAGE_ONE_SHOT,  MIMUSB_USAGE_SELECTOR_BITMAP,   MIMUSB_USAGE_RETRIGGER,
        MIMUSB_USAGE_RETRIGGER};
    static const char* types[] = {"EV_KEY", "EV_KEY", "EV_KEY", "EV_KEY", "EV_KEY",
                                  "EV_KEY", "EV_KEY", "EV_KEY", "EV_KEY", "EV_REL",
                                  "EV_KEY", "EV_KEY", "EV_KEY"};
    static const char* codes[] = {
        "KEY_NEXTSONG", "KEY_PREVIOUSSONG", "KEY_STOPCD",        "KEY_PLAYPAUSE", "KEY_MUTE",
        "KEY_VOLUMEUP", "KEY_VOLUMEDOWN",   "KEY_HOMEPAGE",      "KEY_BACK",      "REL_HWHEEL",
        "KEY_NEW",      "KEY_BRIGHTNESSUP", "KEY_BRIGHTNESSDOWN"};
    static_assert(std::size(semantics) == std::size(media_usages) &&
                  std::size(types) == std::size(media_usages) &&
                  std::size(codes) == std::size(media_usages));
    options.allowed_usages = media_usages;
    options.allowed_usage_count = std::size(media_usages);
    options.usage_semantics = semantics;
    options.expected_linux_event_types = types;
    options.expected_linux_codes = codes;
    return mimusb_spec_create_toggle(&options, spec);
}

// Declares an optional field when its maximum is given.
void optional_field(const std::optional<uint32_t>& maximum, const std::optional<uint32_t>& bits,
                    uint32_t& enable, mimusb_integer_field& out) noexcept {
    if (!maximum)
        return;
    enable = 1U;
    out = field(0, static_cast<int32_t>(*maximum), bits.value_or(bits_to_hold(*maximum)));
}

mimusb_result create_touchscreen(const Size size, const TouchHid& hid, const NodeUsb& usb,
                                 mimusb_spec** spec) noexcept {
    const uint32_t coordinate_bits = hid.coordinate_bits.value_or(touch_bits);
    if (size.w < 1 || size.h < 1 || size.w > (1 << coordinate_bits) ||
        size.h > (1 << coordinate_bits))
        return MIMUSB_ERR_PARAM;
    mimusb_touchscreen_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
    options.function = function_of(usb);
    // Contact 0 is the click, contact 1 the scroll; one report carries both.
    options.maximum_contacts = hid.maximum_contacts.value_or(touch_contacts);
    options.contacts_per_report = hid.contacts_per_report.value_or(touch_contacts);
    options.contact_id = field(0, static_cast<int32_t>(hid.contact_id_max.value_or(touch_contacts - 1)),
                               hid.contact_id_bits.value_or(4));
    options.x = field(0, size.w - 1, coordinate_bits);
    options.y = field(0, size.h - 1, coordinate_bits);
    options.contact_count =
        field(0, static_cast<int32_t>(hid.contact_count_max.value_or(touch_contacts)),
              hid.contact_count_bits.value_or(5));
    optional_field(hid.pressure_max, hid.pressure_bits, options.enable_pressure, options.pressure);
    optional_field(hid.width_max, hid.width_bits, options.enable_width, options.width);
    optional_field(hid.height_max, hid.height_bits, options.enable_height, options.height);
    optional_field(hid.azimuth_max, hid.azimuth_bits, options.enable_azimuth, options.azimuth);
    optional_field(hid.scan_time_max, hid.scan_time_bits, options.enable_scan_time,
                   options.scan_time);
    options.scan_time_unit_100us = hid.scan_time_unit_100us.value_or(0U);
    return mimusb_spec_create_touchscreen(&options, spec);
}

std::string describe(const mimusb_result result) {
    const mimusb_error_detail* detail = mimusb_last_error();
    // Copy the borrowed strings before mimusb_result_name() replaces them.
    const std::string field_name =
        detail != nullptr && detail->field != nullptr ? detail->field : "no field";
    const std::string reason =
        detail != nullptr && detail->reason != nullptr ? detail->reason : "no detail";
    return std::string(mimusb_result_name(result)) + " (" + field_name + ": " + reason + ")";
}

bool port_exists(const std::string& path) {
#ifdef _WIN32
    static_cast<void>(path);
    return true;
#else
    // Only a character device is a serial port: a file that was imported may
    // name any path, and opening a FIFO would wait for ever.
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISCHR(info.st_mode);
#endif
}

// The serial ports the open boards hold. Probing one of them would send a
// message into a running link and set its speed again.
struct Registry {
    std::mutex lock;
    std::vector<std::string> open;

    void add(const std::string& path) {
        const std::lock_guard<std::mutex> guard(lock);
        open.push_back(path);
    }
    void remove(const std::string& path) {
        const std::lock_guard<std::mutex> guard(lock);
        const auto at = std::find(open.begin(), open.end(), path);
        if (at != open.end())
            open.erase(at);
    }
    [[nodiscard]] std::vector<std::string> held() {
        const std::lock_guard<std::mutex> guard(lock);
        return open;
    }
};

class MimPort final : public Port {
  public:
    MimPort(mimusb_device* device, std::string path, const unsigned buttons,
            std::shared_ptr<Registry> registry, const MimDevice& mim)
        : device_(device),
          path_(std::move(path)),
          buttons_(buttons),
          registry_(std::move(registry)),
          mim_(mim) {
        // The waits of a call that re-enumerates the board: the link's answer time and the re-attach.
        waits_.struct_size = static_cast<uint32_t>(sizeof(waits_));
        waits_.control_timeout_ms = mim_.link_timeout_ms;
        waits_.reconnect_timeout_ms = mim_.reconnect_timeout_ms;
        registry_->add(path_);
    }
    ~MimPort() override {
        close();
        registry_->remove(path_);
    }

    [[nodiscard]] bool has(const NodeKind kind) const noexcept override {
        if (clone_) {
            switch (kind) {
            case node_mouse: return reports_.has_mouse();
            case node_keyboard: return reports_.has_keyboard();
            case node_media: return reports_.has_media();
            case node_touch: return false;
            }
        }
        return nodes_[kind] != nullptr;
    }

    // In a clone the calls only collect what is to be said; flush() sends it in the
    // device's own reports.
    [[nodiscard]] bool mouse_move(const int32_t dx, const int32_t dy) override {
        if (clone_) {
            move_x_ += dx;
            move_y_ += dy;
            return true;
        }
        return ok(mimusb_mouse_move(nodes_[node_mouse], dx, dy));
    }
    [[nodiscard]] bool mouse_scroll(const int32_t wheel, const int32_t pan) override {
        if (clone_) {
            move_wheel_ += wheel;
            move_pan_ += pan;
            return true;
        }
        return ok(mimusb_mouse_scroll(nodes_[node_mouse], wheel, pan));
    }
    [[nodiscard]] bool button(const unsigned button, const bool down) override {
        if (clone_) {
            if (button >= 1 && button <= 32) {
                const uint32_t bit = 1U << (button - 1);
                buttons_down_ = down ? (buttons_down_ | bit) : (buttons_down_ & ~bit);
                buttons_changed_ = true;
            }
            return true;
        }
        return ok(mimusb_mouse_button(nodes_[node_mouse], button, down ? 1U : 0U));
    }
    [[nodiscard]] bool key(const uint16_t usage, const bool down) override {
        if (clone_) {
            if (usage <= 0xFF)
                queue(node_keyboard, reports_.key(static_cast<uint8_t>(usage), down));
            return true;
        }
        return ok(mimusb_keyboard_key(nodes_[node_keyboard], usage, down ? 1U : 0U));
    }
    [[nodiscard]] bool media(const uint16_t usage, const bool down) override {
        if (clone_) {
            queue(node_media, reports_.consumer(usage, down));
            return true;
        }
        return ok(mimusb_toggle_usage(nodes_[node_media], usage, down ? 1U : 0U));
    }
    [[nodiscard]] bool touch(const unsigned contact, const bool down, const int32_t x,
                             const int32_t y) override {
        if (clone_) {
            error_ = "a clone has no touchscreen";
            return false;
        }
        return ok(mimusb_touchscreen_contact(nodes_[node_touch], contact, down ? 1U : 0U, x, y,
                                             nullptr));
    }

    [[nodiscard]] bool flush(const uint8_t nodes) override {
        if (clone_)
            return flush_clone(nodes);
        // Queue every Node's report before waiting on any, so they overlap on
        // the wire instead of running one after another.
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            if ((nodes & node_bit(static_cast<NodeKind>(kind))) != 0 &&
                !ok(mimusb_node_submit(nodes_[kind], mim_.link_timeout_ms)))
                return false;
        }
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            // Also sends the fragments a large move or a touch frame still owes.
            if ((nodes & node_bit(static_cast<NodeKind>(kind))) != 0 &&
                !ok(mimusb_node_submit_blocking(nodes_[kind], drain_deadline_ms)))
                return false;
        }
        return true;
    }

    // Opening or closing a Node makes the board detach from its target and
    // attach again, which the target sees as unplugging the device. So any
    // change to the set of Nodes closes them all and opens the wanted ones
    // again, with the interface numbers the user pinned and the lowest free
    // ones for the rest, in one batch: one re-attach. A set that has not
    // changed, and whose interface and touchscreen values have not, does nothing.
    [[nodiscard]] bool set_nodes(const LinkOptions& options, uint8_t& added) override {
        if (options.clone)
            return set_clone(options, added);
        added = 0;
        uint8_t wanted = 0;
        const auto want = [&](const bool on, const NodeKind kind) {
            if (on)
                wanted = static_cast<uint8_t>(wanted | node_bit(kind));
        };
        want(options.mouse, node_mouse);
        want(options.keyboard, node_keyboard);
        want(options.media, node_media);
        want(options.touch, node_touch);

        uint8_t have = 0;
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            if (nodes_[kind] != nullptr)
                have = static_cast<uint8_t>(have | node_bit(static_cast<NodeKind>(kind)));
        }
        const bool resized = options.touch && (touch_size_.w != options.touch_size.w ||
                                               touch_size_.h != options.touch_size.h ||
                                               !(touch_hid_ == options.touch_hid));
        // A Node that stays is rebuilt when its own interface values changed.
        bool usb_changed = false;
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            usb_changed = usb_changed || ((wanted & node_bit(static_cast<NodeKind>(kind))) != 0 &&
                                          !(node_usb_[kind] == options.node_usb[kind]));
        }
        const bool leaving_clone = clone_.has_value();
        if (wanted == have && !resized && !usb_changed && !leaving_clone)
            return true;

        const std::array<uint32_t, usb_nodes> numbers = interface_numbers(options.node_usb, wanted);
        std::vector<uint32_t> used;
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            if ((wanted & node_bit(static_cast<NodeKind>(kind))) != 0)
                used.push_back(numbers[kind]);
        }
        if (const std::string problem = shared_interface_error(options.node_usb, wanted);
            !problem.empty()) {
            error_ = problem;
            return false;
        }
        mimusb_device_info info{};
        if (!ok(mimusb_device_board_info(device_, &info)))
            return false;
        if (const std::string problem = interface_limit_error(used, info.max_interfaces);
            !problem.empty()) {
            error_ = problem;
            return false;
        }

        if (!ok(mimusb_device_batch_begin(device_)))
            return false;
        if (!close_nodes())
            return false;
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            if ((wanted & node_bit(static_cast<NodeKind>(kind))) == 0)
                continue;
            if (!open_node(static_cast<NodeKind>(kind), numbers[kind], options))
                return false;
        }
        if (!ok(mimusb_device_batch_end(device_, &waits_)))
            return false;
        if (wanted != 0 && !ok(mimusb_device_wait_ready(device_, mim_.ready_timeout_ms)))
            return false;
        touch_size_ = options.touch ? options.touch_size : Size{};
        touch_hid_ = options.touch ? options.touch_hid : TouchHid{};
        node_usb_ = options.node_usb;
        added = wanted;
        return true;
    }

    // Presents the plugged-in device the snapshot describes: one raw Node for each of its
    // interfaces that the switches leave anything in, with the descriptors cut to what is
    // on. The Nodes are rebuilt (one re-attach) only when the snapshot or the switches
    // changed; a new battery level is only a report.
    [[nodiscard]] bool set_clone(const LinkOptions& options, uint8_t& added) {
        added = 0;
        const CloneRoles roles{options.mouse, options.keyboard, options.media};
        CloneSnapshot shape = *options.clone;
        shape.battery = 0;
        const bool profile_open = std::any_of(nodes_.begin(), nodes_.end(),
                                              [](const mimusb_node* n) { return n != nullptr; });
        if (clone_ && !profile_open && *clone_ == shape && clone_roles_ == roles) {
            if (options.clone->battery != battery_) {
                battery_ = options.clone->battery;
                return send_battery();
            }
            return true;
        }

        CloneReports built;
        if (const std::string problem = CloneReports::build(shape.device, roles, built);
            !problem.empty()) {
            error_ = problem;
            return false;
        }
        mimusb_device_info info{};
        if (!ok(mimusb_device_board_info(device_, &info)))
            return false;
        mimusb_board_limits limits{};
        limits.struct_size = static_cast<uint32_t>(sizeof(limits));
        if (!ok(mimusb_device_limits(device_, &limits)))
            return false;
        feature_limit_ = limits.max_feature_bytes;
        const std::vector<CloneReports::Node>& nodes = built.nodes();
        size_t bytes = 0;
        for (const CloneReports::Node& each : nodes)
            bytes += each.descriptor.size();
        if (nodes.size() > info.max_interfaces) {
            error_ = "the clone presents " + std::to_string(nodes.size()) +
                     " interfaces; the board holds " + std::to_string(info.max_interfaces);
            return false;
        }
        if (bytes > info.max_descriptor_bytes) {
            error_ = "the clone's descriptors are " + std::to_string(bytes) +
                     " bytes; the board has room for " + std::to_string(info.max_descriptor_bytes);
            return false;
        }

        if (!ok(mimusb_device_batch_begin(device_)))
            return false;
        if (!close_nodes())
            return false;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const CloneInterface& itf = shape.device.interfaces[nodes[i].source];
            // What the plugged-in device said about this interface is all there is: a value it
            // did not give is an error, not something to fill in.
            if (itf.interval_ms == 0 || itf.ep_in_size == 0 || itf.bcd_hid == 0) {
                error_ = "the clone's interface " + std::to_string(itf.number) +
                         " has no " +
                         (itf.interval_ms == 0 ? "poll interval"
                          : itf.ep_in_size == 0 ? "IN endpoint size"
                                                : "bcdHID");
                return false;
            }
            mimusb_node* opened = nullptr;
            if (!open_clone_node(i, itf, nodes[i], limits, &opened))
                return false;
            clone_nodes_.push_back(opened);
        }
        if (!ok(mimusb_device_batch_end(device_, &waits_)))
            return false;
        if (!nodes.empty() && !ok(mimusb_device_wait_ready(device_, mim_.ready_timeout_ms)))
            return false;

        reports_ = std::move(built);
        clone_ = shape;
        clone_roles_ = roles;
        battery_ = options.clone->battery;
        move_x_ = move_y_ = move_wheel_ = move_pan_ = 0;
        buttons_down_ = 0;
        buttons_changed_ = false;
        pending_ = {};
        touch_size_ = Size{};
        touch_hid_ = TouchHid{};
        node_usb_ = {};
        if (reports_.has_mouse())
            added = static_cast<uint8_t>(added | node_bit(node_mouse));
        if (reports_.has_keyboard())
            added = static_cast<uint8_t>(added | node_bit(node_keyboard));
        if (reports_.has_media())
            added = static_cast<uint8_t>(added | node_bit(node_media));
        return send_battery();
    }

    // The board has re-attached and mounted before set_nodes returns.
    [[nodiscard]] int64_t settle_ns() const noexcept override { return 0; }

    [[nodiscard]] int start_proxy(uint16_t, bool& held, bool& absent) override {
        held = absent = false;
        return proxy_refused;
    }
    void stop_proxy() override {}

    void close() override {
        // A clone's Nodes are raw: they let go of nothing by themselves.
        if (device_ != nullptr && clone_)
            for (const CloneOut& out : reports_.neutral())
                static_cast<void>(send(out));
        // The first close consumes the Device and its Nodes for every result.
        if (device_ != nullptr)
            static_cast<void>(
                mimusb_device_close(device_, mim_.link_timeout_ms, mim_.drain_timeout_ms));
        device_ = nullptr;
        nodes_ = {};
        clone_nodes_.clear();
    }

    [[nodiscard]] std::string error() const override { return error_; }

    // The serial port going away is only learned of by asking.
    [[nodiscard]] bool healthy() override {
        return device_ == nullptr || ok(mimusb_device_latched_error(device_));
    }

  private:
    [[nodiscard]] bool ok(const mimusb_result result) {
        if (result == MIMUSB_OK)
            return true;
        error_ = describe(result);
        return false;
    }

    // The endpoint number of an address must be one the board's USB controller has (the target's
    // SET_CONFIGURATION would fail otherwise); else error_ says so.
    [[nodiscard]] bool endpoint_in_range(const uint8_t address, const uint32_t highest) {
        if ((address & 0x0FU) <= highest)
            return true;
        error_ = "endpoint address 0x" + [&] {
            char text[8];
            std::snprintf(text, sizeof text, "%02X", address);
            return std::string(text);
        }() + " is above the board's highest endpoint number " + std::to_string(highest);
        return false;
    }

    // Closes every Node, of the profiles and of a clone, inside a batch the caller opened.
    [[nodiscard]] bool close_nodes() {
        for (mimusb_node*& node : nodes_) {
            if (node == nullptr)
                continue;
            const mimusb_result closed = mimusb_node_close(node, &waits_, mim_.drain_timeout_ms);
            // Every result but these consumed the Node, an error included.
            if (closed != MIMUSB_ERR_PARAM && closed != MIMUSB_ERR_UNSET_FIELD)
                node = nullptr;
            if (!ok(closed))
                return false;
        }
        for (mimusb_node*& node : clone_nodes_) {
            if (node == nullptr)
                continue;
            const mimusb_result closed = mimusb_node_close(node, &waits_, mim_.drain_timeout_ms);
            // Every result but these consumed the Node, an error included.
            if (closed != MIMUSB_ERR_PARAM && closed != MIMUSB_ERR_UNSET_FIELD)
                node = nullptr;
            if (!ok(closed))
                return false;
        }
        clone_nodes_.clear();
        feature_rules_.clear();
        clone_.reset();
        return true;
    }

    // One report of the clone: an Input report goes out as it is; a Feature report is what
    // the board answers a read with, which it holds up to its max_feature_bytes of.
    [[nodiscard]] bool send(const CloneOut& out) {
        mimusb_node* node = clone_nodes_[out.node];
        if (out.feature) {
            if (out.bytes.size() > feature_limit_)
                return true;
            const auto rule = feature_rules_.find({out.node, out.report_id});
            if (rule == feature_rules_.end()) {
                error_ = "the clone has no Feature report " + std::to_string(out.report_id);
                return false;
            }
            std::vector<uint8_t> data;
            if (reports_.nodes()[out.node].has_ids)
                data.push_back(out.report_id);
            data.insert(data.end(), out.bytes.begin(), out.bytes.end());
            return ok(mimusb_node_set_rule_data(node, rule->second, data.data(), data.size(),
                                                mim_.link_timeout_ms));
        }
        return ok(mimusb_node_endpoint_write(node, static_cast<uint8_t>(0x81U + out.node),
                                             out.bytes.data(), out.bytes.size(),
                                             mim_.link_timeout_ms));
    }

    [[nodiscard]] bool send_battery() {
        const std::optional<CloneOut> out = reports_.battery(static_cast<int>(battery_));
        return !out || send(*out);
    }

    void queue(const NodeKind kind, std::vector<CloneOut> outs) {
        for (CloneOut& out : outs)
            pending_[kind].push_back(std::move(out));
    }

    [[nodiscard]] bool flush_clone(const uint8_t nodes) {
        if ((nodes & node_bit(node_mouse)) != 0 && reports_.has_mouse() &&
            (buttons_changed_ || move_x_ != 0 || move_y_ != 0 || move_wheel_ != 0 ||
             move_pan_ != 0)) {
            const auto clamp = [](const int64_t v) {
                return static_cast<int>(std::clamp<int64_t>(v, -1000000, 1000000));
            };
            queue(node_mouse, reports_.mouse(clamp(move_x_), clamp(move_y_), clamp(move_wheel_),
                                             clamp(move_pan_), buttons_down_));
            move_x_ = move_y_ = move_wheel_ = move_pan_ = 0;
            buttons_changed_ = false;
        }
        for (const NodeKind kind : {node_mouse, node_keyboard, node_media}) {
            if ((nodes & node_bit(kind)) == 0)
                continue;
            std::vector<CloneOut> outs = std::move(pending_[kind]);
            pending_[kind].clear();
            for (const CloneOut& out : outs)
                if (!send(out))
                    return false;
        }
        return true;
    }

    // One interface of the plugged-in device, written out as it is: its report descriptor is
    // not checked, and the requests a HID interface takes (HID 1.11 7.1.1, 7.2) are answered by
    // the rules below. A Feature report is answered from data set later with send().
    [[nodiscard]] bool open_clone_node(const size_t index, const CloneInterface& itf,
                                       const CloneReports::Node& shape,
                                       const mimusb_board_limits& limits, mimusb_node** out) {
        const uint8_t in_address = static_cast<uint8_t>(0x81U + index);
        const uint8_t out_address = static_cast<uint8_t>(0x01U + index);
        const uint16_t in_size = static_cast<uint16_t>(std::min(itf.ep_in_size, limits.max_endpoint_bytes));
        const uint16_t out_size = static_cast<uint16_t>(std::min(itf.ep_out_size, limits.max_endpoint_bytes));
        if (!endpoint_in_range(in_address, limits.max_endpoint_number) ||
            (out_size > 0 && !endpoint_in_range(out_address, limits.max_endpoint_number)))
            return false;

        std::array<mimusb_usb_endpoint, 2> endpoints{};
        endpoints[0].address = in_address;
        endpoints[0].transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        endpoints[0].interval = static_cast<uint8_t>(itf.interval_ms);
        endpoints[0].max_packet_size = in_size;
        endpoints[0].policy.on_full = MIMUSB_FULL_WAIT;
        endpoints[0].policy.depth = endpoint_queue_depth;
        endpoints[1].address = out_address;
        endpoints[1].transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        endpoints[1].interval = static_cast<uint8_t>(itf.interval_ms);
        endpoints[1].max_packet_size = out_size;

        const uint16_t descriptor_length = static_cast<uint16_t>(shape.descriptor.size());
        const std::array<uint8_t, 9> hid = {
            9, 0x21, static_cast<uint8_t>(itf.bcd_hid & 0xFFU), static_cast<uint8_t>(itf.bcd_hid >> 8),
            itf.country_code, 1, 0x22, static_cast<uint8_t>(descriptor_length & 0xFFU),
            static_cast<uint8_t>(descriptor_length >> 8)};
        const std::array<mimusb_usb_item, 3> items = {
            mimusb_usb_item_make_descriptor(hid.data(), hid.size()),
            mimusb_usb_item_make_endpoint(endpoints[0]), mimusb_usb_item_make_endpoint(endpoints[1])};
        const mimusb_usb_setting setting{items.data(), out_size > 0 ? size_t{3} : size_t{2}};
        mimusb_usb_interface iface{};
        iface.number = static_cast<uint8_t>(index);
        iface.class_code = 0x03U; // HID
        iface.subclass = itf.boot_subclass;
        iface.protocol = itf.boot_protocol;
        iface.name = itf.name.empty() ? nullptr : itf.name.c_str();
        iface.settings = &setting;
        iface.setting_count = 1;

        std::vector<mimusb_control_rule> rules;
        const auto rule = [&](const uint8_t type, const uint8_t request,
                              const mimusb_control_rule_action action) {
            mimusb_control_rule r{};
            r.request_type_mask = 0xFF;
            r.request_type_value = type;
            r.request_mask = 0xFF;
            r.request_value = request;
            r.index_mask = 0xFFFF;
            r.index_value = static_cast<uint16_t>(index);
            r.action = action;
            return r;
        };
        static const uint8_t idle_rate = 0;       // HID 1.11 7.2.4
        static const uint8_t report_protocol = 1; // HID 1.11 7.2.6
        mimusb_control_rule report = rule(0x81, 0x06, MIMUSB_CONTROL_ANSWER); // GET_DESCRIPTOR
        report.value_mask = 0xFFFF;
        report.value_value = 0x2200;
        report.data = shape.descriptor.data();
        report.length = shape.descriptor.size();
        rules.push_back(report);
        mimusb_control_rule set_report = rule(0x21, 0x09, MIMUSB_CONTROL_ACCEPT); // Output reports
        set_report.value_mask = 0xFF00;
        set_report.value_value = 0x0200;
        rules.push_back(set_report);
        rules.push_back(rule(0x21, 0x0A, MIMUSB_CONTROL_ACCEPT)); // SET_IDLE
        rules.push_back(rule(0x21, 0x0B, MIMUSB_CONTROL_ACCEPT)); // SET_PROTOCOL
        mimusb_control_rule get_idle = rule(0xA1, 0x02, MIMUSB_CONTROL_ANSWER);
        get_idle.data = &idle_rate;
        get_idle.length = 1;
        rules.push_back(get_idle);
        mimusb_control_rule get_protocol = rule(0xA1, 0x03, MIMUSB_CONTROL_ANSWER);
        get_protocol.data = &report_protocol;
        get_protocol.length = 1;
        rules.push_back(get_protocol);
        std::vector<std::vector<uint8_t>> blanks;
        blanks.reserve(shape.parsed.reports.size());
        for (const HidReportSize& each : shape.parsed.reports) {
            // GET_REPORT (HID 1.11 7.2.1): an Input report is the last one sent, a Feature
            // report is the bytes set with send(), zeros until then.
            mimusb_control_rule get = rule(0xA1, 0x01, each.feature ? MIMUSB_CONTROL_ANSWER
                                                                    : MIMUSB_CONTROL_ANSWER_LAST_SENT);
            get.value_mask = 0xFFFF;
            get.value_value = static_cast<uint16_t>((each.feature ? 0x0300U : 0x0100U) | each.report_id);
            if (each.feature) {
                blanks.emplace_back((each.bits + 7U) / 8U + (shape.has_ids ? 1U : 0U), uint8_t{0});
                if (shape.has_ids)
                    blanks.back()[0] = each.report_id;
                get.data = blanks.back().data();
                get.length = blanks.back().size();
                feature_rules_[{index, each.report_id}] = rules.size();
            } else {
                get.endpoint_address = in_address;
                get.first_byte_filter = shape.has_ids ? 1U : 0U;
                get.first_byte = each.report_id;
            }
            rules.push_back(get);
        }

        mimusb_node_config config{};
        mimusb_node_config_init(&config);
        config.interfaces = &iface;
        config.interface_count = 1;
        config.rules = rules.data();
        config.rule_count = rules.size();
        mimusb_spec* spec = nullptr;
        if (!ok(mimusb_spec_create_interfaces(&config, &spec)))
            return false;
        mimusb_node_placement placement;
        mimusb_node_placement_init(&placement);
        const mimusb_result result = mimusb_node_open(device_, spec, &placement, &waits_, out);
        mimusb_spec_release(spec);
        return ok(result);
    }

    [[nodiscard]] bool open_node(const NodeKind kind, const uint32_t interface,
                                 const LinkOptions& options) {
        // Each Spec is created in its own statement: as a call argument it would
        // be read before the factory beside it had filled it in.
        const NodeUsb& usb = options.node_usb[kind];
        // Nothing is filled in for a value the user left out.
        if (const std::string absent = node_usb_missing(usb); !absent.empty()) {
            error_ = "not set: " + absent;
            return false;
        }
        mimusb_spec* spec = nullptr;
        mimusb_result result = MIMUSB_OK;
        switch (kind) {
        case node_mouse:
            result = create_mouse(buttons_, options.node_usb[node_mouse], &spec);
            break;
        case node_keyboard:
            result = create_keyboard(options.node_usb[node_keyboard], &spec);
            break;
        case node_media:
            result = create_toggle(options.node_usb[node_media], &spec);
            break;
        case node_touch:
            result = create_touchscreen(options.touch_size, options.touch_hid,
                                        options.node_usb[node_touch], &spec);
            break;
        }
        if (!ok(result))
            return false;
        mimusb_board_limits board{};
        board.struct_size = static_cast<uint32_t>(sizeof(board));
        if (!ok(mimusb_device_limits(device_, &board))) {
            mimusb_spec_release(spec);
            return false;
        }
        if (!open_placed(spec, interface, usb.interface_name, static_cast<uint32_t>(*usb.ep_in_size),
                         static_cast<uint32_t>(*usb.ep_out_size),
                         static_cast<uint32_t>(*usb.poll_interval_ms), board, &nodes_[kind])) {
            nodes_[kind] = nullptr;
            return false;
        }
        return true;
    }

    // Places a HID Spec on the board and opens it as a Node, then lets go of the Spec (the Node
    // keeps its own reference). An endpoint's number follows its interface's: IN 0x81 + n,
    // OUT 0x01 + n. A zero `out_size` means no OUT endpoint; the OUT endpoint polls as often as
    // the IN one.
    [[nodiscard]] bool open_placed(mimusb_spec* spec, const uint32_t interface,
                                   const std::string& name, const uint32_t in_size,
                                   const uint32_t out_size, const uint32_t interval_ms,
                                   const mimusb_board_limits& board, mimusb_node** out) {
        std::array<mimusb_placed_endpoint, 2> endpoints{};
        endpoints[0].role = MIMUSB_ROLE_HID_IN;
        endpoints[0].endpoint.address = static_cast<uint8_t>(0x81U + interface);
        endpoints[0].endpoint.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        endpoints[0].endpoint.interval = static_cast<uint8_t>(interval_ms);
        endpoints[0].endpoint.max_packet_size = static_cast<uint16_t>(in_size);
        endpoints[0].endpoint.policy.on_full = MIMUSB_FULL_WAIT;
        endpoints[0].endpoint.policy.depth = endpoint_queue_depth;
        size_t count = 1;
        if (out_size > 0) {
            endpoints[1].role = MIMUSB_ROLE_HID_OUT;
            endpoints[1].endpoint.address = static_cast<uint8_t>(0x01U + interface);
            endpoints[1].endpoint.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
            endpoints[1].endpoint.interval = static_cast<uint8_t>(interval_ms);
            endpoints[1].endpoint.max_packet_size = static_cast<uint16_t>(out_size);
            count = 2;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!endpoint_in_range(endpoints[i].endpoint.address, board.max_endpoint_number)) {
                mimusb_spec_release(spec);
                return false;
            }
        }
        mimusb_node_placement placement;
        mimusb_node_placement_init(&placement);
        placement.first_interface = static_cast<uint8_t>(interface);
        placement.name = name.empty() ? nullptr : name.c_str();
        placement.endpoints = endpoints.data();
        placement.endpoint_count = count;
        const mimusb_result result = mimusb_node_open(device_, spec, &placement, &waits_, out);
        mimusb_spec_release(spec);
        return ok(result);
    }

    mimusb_device* device_;
    std::string path_;
    unsigned buttons_;
    std::shared_ptr<Registry> registry_;
    // What the board was opened with: its waits are the user's.
    MimDevice mim_;
    mimusb_commit_timeouts waits_{};
    std::array<mimusb_node*, node_kinds> nodes_{};
    // The size and descriptor values the touch Node was built for.
    Size touch_size_{};
    TouchHid touch_hid_;
    // The interface values the Nodes were built with.
    std::array<NodeUsb, usb_nodes> node_usb_{};
    // The clone presented, without its battery level, and what it was built under.
    std::optional<CloneSnapshot> clone_;
    CloneRoles clone_roles_;
    CloneReports reports_;
    std::vector<mimusb_node*> clone_nodes_;
    // The control rule that answers each Feature report of a clone: (interface, Report ID) to its number.
    std::map<std::pair<size_t, uint8_t>, size_t> feature_rules_;
    size_t feature_limit_{};
    uint32_t battery_{};
    // What has been said since the last flush.
    int64_t move_x_{};
    int64_t move_y_{};
    int64_t move_wheel_{};
    int64_t move_pan_{};
    uint32_t buttons_down_{};
    bool buttons_changed_{};
    std::array<std::vector<CloneOut>, node_kinds> pending_;
    std::string error_;
};

class MimBackend final : public Backend {
  public:
    [[nodiscard]] const char* name() const noexcept override { return "boards (MIM)"; }
    [[nodiscard]] std::string start() override { return {}; }
    void stop() override { listed_.clear(); }

    [[nodiscard]] std::vector<TargetInfo> scan(const ScanRequest& request,
                                               std::string& error) override {
        const std::vector<std::string> held = registry_->held();
        // libmimusb opens a port for one user at a time, so the search passes
        // over the boards that are open here.
        if (request.probe_boards)
            probe(error);
        // What is listed: the boards the last search found, the ports the user
        // named and the boards that are open, once each, where they exist.
        std::vector<Listed> all = found_;
        for (const std::string& path : request.board_ports)
            all.push_back({path, {}, 0U});
        for (const std::string& path : held)
            all.push_back({path, {}, 0U});

        listed_.clear();
        std::vector<TargetInfo> targets;
        for (const Listed& each : all) {
            const bool again =
                std::any_of(listed_.begin(), listed_.end(),
                            [&](const std::string& path) { return path == each.path; });
            const bool holding = std::find(held.begin(), held.end(), each.path) != held.end();
            if (again || (!holding && !port_exists(each.path)))
                continue;
            listed_.push_back(each.path);
            TargetInfo info;
            info.transport = Transport::mim;
            info.serial = mim_serial_of(each.path);
            info.product = each.description;
            info.detail = each.path;
            info.baud = each.baud;
            targets.push_back(std::move(info));
        }
        return targets;
    }

    [[nodiscard]] std::unique_ptr<Port> open(const size_t index, const int,
                                             const LinkOptions& options,
                                             std::string& error) override {
        if (index >= listed_.size()) {
            error = "the board is not in the last scan";
            return nullptr;
        }
        const std::string& path = listed_[index];
        uint8_t wanted = 0;
        if (options.mouse)
            wanted = static_cast<uint8_t>(wanted | node_bit(node_mouse));
        if (options.keyboard)
            wanted = static_cast<uint8_t>(wanted | node_bit(node_keyboard));
        if (options.media)
            wanted = static_cast<uint8_t>(wanted | node_bit(node_media));
        if (options.touch)
            wanted = static_cast<uint8_t>(wanted | node_bit(node_touch));
        MimDevice mim = options.mim;
        mim.node = options.node_usb;
        // The port is the one this backend found; the file's own is not read here.
        if (mim.port.empty())
            mim.port = path;
        const std::string missing = board_missing(mim, wanted, options.clone.has_value());
        if (!missing.empty()) {
            error = "not set: " + missing;
            return nullptr;
        }

        mimusb_device_config device{};
        mimusb_device_config_init(&device);
        device.transport.baud_rate = mim.baud;
        device.transport.handshake_timeout_ms = mim.handshake_timeout_ms;
        // The largest report libmimusb builds. A board that holds less (a full-speed
        // chip takes 64) is asked for its limit below and opened again with it.
        device.transport.maximum_report_bytes = MIMUSB_HOST_MAX_REPORT_BYTES;
        device.target.linux_hid.descriptor_bytes = mim.linux_hid.descriptor_bytes;
        device.target.linux_hid.fields_per_report = mim.linux_hid.fields_per_report;
        device.target.linux_hid.global_stack_depth = mim.linux_hid.global_stack_depth;
        device.target.linux_hid.report_size_bits = mim.linux_hid.report_size_bits;
        device.target.linux_hid.usages = mim.linux_hid.usages;
        device.target.linux_hid.report_data_bits = mim.linux_hid.report_data_bits;
        // The Nodes are fixed and range-checked by the profile factories.
        device.transport.validate_reports = 0U;
        // The board detaches from its target when this program closes it.
        device.transport.keep_attached = 0U;
        device.transport.log_level = MIMUSB_LOG_DISABLED;
        device.usb_device.vendor_id = mim.vendor_id;
        device.usb_device.product_id = mim.product_id;
        device.usb_device.bcd_device = mim.bcd_device;
        device.usb_device.bcd_usb = mim.bcd_usb;
        device.usb_device.ep0_max_packet_size = static_cast<uint8_t>(mim.ep0_max_packet_size);
        device.usb_device.language_id = mim.language_id;
        device.usb_device.manufacturer = mim.manufacturer.c_str();
        device.usb_device.product = mim.product.c_str();
        device.usb_device.serial = mim.serial.empty() ? nullptr : mim.serial.c_str();
        device.usb_device.serial_from_chip_id = mim.serial_from_chip_id ? 1U : 0U;
        // Each interface says its own class (USB 2.0 9.6.1: class 0 at the device level).
        device.usb_device.device_class = 0;
        device.usb_device.device_subclass = 0;
        device.usb_device.device_protocol = 0;
        // The board has one configuration, and the stack numbers it 1.
        device.usb_config.configuration_value = 1;
        device.usb_config.string = mim.configuration.empty() ? nullptr : mim.configuration.c_str();
        // USB 2.0 9.6.3: bit 7 is reserved and set; bit 6 self-powered, bit 5 remote wakeup.
        const uint8_t self_pw = mim.self_powered ? 0x40 : 0;
        const uint8_t rem_wk = mim.remote_wakeup ? 0x20 : 0;
        device.usb_config.bm_attributes = static_cast<uint8_t>(0x80 | self_pw | rem_wk);
        // bMaxPower is in units of 2 mA (USB 2.0 9.6.3).
        device.usb_config.bmax_power = static_cast<uint8_t>(mim.max_power_ma / 2);

        mimusb_device* opened = nullptr;
        mimusb_result result = mimusb_device_open(path.c_str(), &device, &opened);
        if (result == MIMUSB_ERR_OVERFLOW) {
            const uint32_t limit = board_report_limit(path, device, mim);
            if (limit != 0U) {
                device.transport.maximum_report_bytes = limit;
                result = mimusb_device_open(path.c_str(), &device, &opened);
            }
        }
        if (result != MIMUSB_OK) {
            error = "the board could not be opened: " + describe(result);
            return nullptr;
        }
        auto port = std::make_unique<MimPort>(opened, path, options.buttons, registry_, mim);
        uint8_t added = 0;
        if (!port->set_nodes(options, added)) {
            error = "the input devices could not be registered: " + port->error();
            return nullptr;
        }
        return port;
    }

  private:
    // The largest report the board at path takes, learned by opening it with the
    // smallest limit; 0 when that fails too. The limit never exceeds libmimusb's.
    [[nodiscard]] static uint32_t board_report_limit(const std::string& path,
                                                     mimusb_device_config config,
                                                     const MimDevice& mim) {
        config.transport.maximum_report_bytes = 1U;
        mimusb_device* probe = nullptr;
        if (mimusb_device_open(path.c_str(), &config, &probe) != MIMUSB_OK)
            return 0U;
        mimusb_board_limits limits{};
        limits.struct_size = static_cast<uint32_t>(sizeof(limits));
        const bool known = mimusb_device_limits(probe, &limits) == MIMUSB_OK;
        static_cast<void>(mimusb_device_close(probe, mim.link_timeout_ms, mim.drain_timeout_ms));
        if (!known)
            return 0U;
        return limits.max_report_bytes < MIMUSB_HOST_MAX_REPORT_BYTES
                   ? limits.max_report_bytes
                   : static_cast<uint32_t>(MIMUSB_HOST_MAX_REPORT_BYTES);
    }

    struct Listed {
        std::string path;
        std::string description;
        uint32_t baud{};
    };

    // Sends a message to every serial port that could be a board and keeps the
    // ones that answer.
    void probe(std::string& error) {
        found_.clear();
        mimusb_discovery* discovery = nullptr;
        const mimusb_result result = mimusb_discover(discover_timeout_ms, &discovery);
        if (result != MIMUSB_OK) {
            error = "the search for boards failed: " + describe(result);
            return;
        }
        const size_t count = mimusb_discovery_count(discovery);
        for (size_t index = 0; index < count; ++index) {
            const mimusb_device_info* info = mimusb_discovery_get(discovery, index);
            if (info == nullptr || info->port_name[0] == '\0')
                continue;
            std::string text = info->description;
            text += " (firmware " + std::to_string(info->version_major) + "." +
                    std::to_string(info->version_minor) + "." +
                    std::to_string(info->version_patch) + ")";
            // It answers, but cannot be opened until it is flashed again.
            if (!info->firmware_compatible) {
                text += ": flash the libmimusb " + std::to_string(MIMUSB_VERSION_MAJOR) + "." +
                        std::to_string(MIMUSB_VERSION_MINOR) + "." +
                        std::to_string(MIMUSB_VERSION_PATCH) + " firmware";
                found_.push_back({info->port_name, std::move(text), 0U});
                continue;
            }
            found_.push_back({info->port_name, std::move(text), info->baud_rate});
        }
        mimusb_discovery_destroy(discovery);
    }

    std::shared_ptr<Registry> registry_ = std::make_shared<Registry>();
    // The boards the last search found.
    std::vector<Listed> found_;
    // The paths of the last scan, in its order, for open().
    std::vector<std::string> listed_;
};

} // namespace

std::unique_ptr<Backend> make_mim_backend() { return std::make_unique<MimBackend>(); }

} // namespace hids
