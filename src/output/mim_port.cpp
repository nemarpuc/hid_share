// SPDX-License-Identifier: MIT
// A microcontroller board on a serial port, through libmimhid. The board
// presents the Nodes to its own target as one USB HID device.
#include "hid_share/config.hpp"
#include "port.hpp"

#include <mimhid.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <mutex>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hids {
namespace {

// Every value below is this application's policy, stated explicitly rather
// than left to libmimhid's documented fallbacks (docs/API.md there).
// The search waits this long for each answer, at each rate it tries.
constexpr uint32_t discover_timeout_ms = 50U;
// How long the serial link waits for a board's answer to a command, and for a
// report to leave.
constexpr uint32_t link_timeout_ms = 500U;
constexpr uint32_t reconnect_timeout_ms = 3000U;
constexpr uint32_t close_drain_timeout_ms = 1000U;
constexpr uint32_t drain_deadline_ms = 600U;
// The target's mount after the board re-attaches.
constexpr uint32_t ready_timeout_ms = 5000U;
constexpr uint16_t usb_bcd_device = 0x0100;
constexpr uint16_t usb_language_id = 0x0409;
constexpr uint32_t usb_max_power_ma = 100U;
constexpr uint32_t descriptor_policy_bytes = 4096U;
constexpr uint16_t hid_bcd = 0x0111;
constexpr uint32_t touch_contacts = 2U;
constexpr uint32_t touch_bits = 16U;

// Consumer page usages the toggle Node declares; the key map only produces
// these. The same list and order as the phone side.
constexpr uint16_t media_usages[] = {0x00B5, 0x00B6, 0x00B7, 0x00CD, 0x00E2, 0x00E9, 0x00EA,
                                     0x0223, 0x0224, 0x0238, 0x0201, 0x006F, 0x0070};

mimhid_integer_field field(const int32_t minimum, const int32_t maximum,
                           const uint32_t bits) noexcept {
    mimhid_integer_field value{};
    value.logical_minimum = minimum;
    value.logical_maximum = maximum;
    value.bit_width = bits;
    return value;
}

// The Report ID of a Node: none unless the user gave one.
mimhid_report_id_option report_id_of(const NodeUsb& usb) noexcept {
    mimhid_report_id_option id{};
    if (usb.report_id) {
        id.enabled = 1U;
        id.value = static_cast<uint8_t>(*usb.report_id);
    }
    return id;
}

mimhid_result create_mouse(const unsigned buttons, const NodeUsb& usb,
                           mimhid_spec** spec) noexcept {
    mimhid_mouse_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
    options.button_count = buttons;
    options.x = field(-32767, 32767, 16);
    options.y = field(-32767, 32767, 16);
    options.enable_wheel = 1U;
    options.wheel = field(-127, 127, 8);
    options.enable_pan = 1U;
    options.pan = field(-127, 127, 8);
    return mimhid_spec_create_mouse(&options, spec);
}

mimhid_result create_keyboard(const NodeUsb& usb, mimhid_spec** spec) noexcept {
    mimhid_keyboard_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
    // Through LANG and the keypad extras; the modifiers 0xE0-0xE7 are always
    // declared by the profile.
    options.usage_minimum = 0x04;
    options.usage_maximum = 0xDD;
    return mimhid_spec_create_keyboard(&options, spec);
}

mimhid_result create_toggle(const NodeUsb& usb, mimhid_spec** spec) noexcept {
    mimhid_toggle_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
    options.application_page = 0x0CU;  // Consumer
    options.application_usage = 0x01U; // Consumer Control
    options.field_page = 0x0CU;
    // In media_usages order: Next, Prev, Stop, Play/Pause, Mute, Vol Up,
    // Vol Down, AC Home, AC Back, AC Pan, AC New, Brightness Up, Brightness Down.
    static const mimhid_usage_semantic semantics[] = {
        MIMHID_USAGE_ONE_SHOT,  MIMHID_USAGE_ONE_SHOT,          MIMHID_USAGE_ONE_SHOT,
        MIMHID_USAGE_ONE_SHOT,  MIMHID_USAGE_ON_OFF_MAINTAINED, MIMHID_USAGE_RETRIGGER,
        MIMHID_USAGE_RETRIGGER, MIMHID_USAGE_ONE_SHOT,          MIMHID_USAGE_ONE_SHOT,
        MIMHID_USAGE_ONE_SHOT,  MIMHID_USAGE_SELECTOR_BITMAP,   MIMHID_USAGE_RETRIGGER,
        MIMHID_USAGE_RETRIGGER};
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
    return mimhid_spec_create_toggle(&options, spec);
}

// Declares an optional field when its maximum is given.
void optional_field(const std::optional<uint32_t>& maximum, const std::optional<uint32_t>& bits,
                    uint32_t& enable, mimhid_integer_field& out) noexcept {
    if (!maximum)
        return;
    enable = 1U;
    out = field(0, static_cast<int32_t>(*maximum), bits.value_or(bits_to_hold(*maximum)));
}

mimhid_result create_touchscreen(const Size size, const TouchHid& hid, const NodeUsb& usb,
                                 mimhid_spec** spec) noexcept {
    const uint32_t coordinate_bits = hid.coordinate_bits.value_or(touch_bits);
    if (size.w < 1 || size.h < 1 || size.w > (1 << coordinate_bits) ||
        size.h > (1 << coordinate_bits))
        return MIMHID_ERR_PARAM;
    mimhid_touchscreen_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.report_id = report_id_of(usb);
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
    return mimhid_spec_create_touchscreen(&options, spec);
}

std::string describe(const mimhid_result result) {
    const mimhid_error_detail* detail = mimhid_last_error();
    // Copy the borrowed strings before mimhid_result_name() replaces them.
    const std::string field_name =
        detail != nullptr && detail->field != nullptr ? detail->field : "no field";
    const std::string reason =
        detail != nullptr && detail->reason != nullptr ? detail->reason : "no detail";
    return std::string(mimhid_result_name(result)) + " (" + field_name + ": " + reason + ")";
}

// The values a board cannot be opened without, which the user gives.
std::string missing_values(const MimOptions& mim, const bool cloned) {
    std::string list;
    const auto need = [&](const bool given, const char* name) {
        if (given)
            return;
        if (!list.empty())
            list += ", ";
        list += name;
    };
    need(mim.baud != 0, "baud");
    // A clone brings its own ids and strings, and a device may have none of the strings.
    if (!cloned) {
        need(mim.vendor_id != 0, "vendor_id");
        need(mim.product_id != 0, "product_id");
        need(!mim.manufacturer.empty(), "manufacturer");
        need(!mim.product.empty(), "product");
    }
    return list;
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
    MimPort(mimhid_device* device, std::string path, const unsigned buttons,
            std::shared_ptr<Registry> registry)
        : device_(device),
          path_(std::move(path)),
          buttons_(buttons),
          registry_(std::move(registry)) {
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
        return ok(mimhid_mouse_move(nodes_[node_mouse], dx, dy));
    }
    [[nodiscard]] bool mouse_scroll(const int32_t wheel, const int32_t pan) override {
        if (clone_) {
            move_wheel_ += wheel;
            move_pan_ += pan;
            return true;
        }
        return ok(mimhid_mouse_scroll(nodes_[node_mouse], wheel, pan));
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
        return ok(mimhid_mouse_button(nodes_[node_mouse], button, down ? 1U : 0U));
    }
    [[nodiscard]] bool key(const uint16_t usage, const bool down) override {
        if (clone_) {
            if (usage <= 0xFF)
                queue(node_keyboard, reports_.key(static_cast<uint8_t>(usage), down));
            return true;
        }
        return ok(mimhid_keyboard_key(nodes_[node_keyboard], usage, down ? 1U : 0U));
    }
    [[nodiscard]] bool media(const uint16_t usage, const bool down) override {
        if (clone_) {
            queue(node_media, reports_.consumer(usage, down));
            return true;
        }
        return ok(mimhid_toggle_usage(nodes_[node_media], usage, down ? 1U : 0U));
    }
    [[nodiscard]] bool touch(const unsigned contact, const bool down, const int32_t x,
                             const int32_t y) override {
        if (clone_) {
            error_ = "a clone has no touchscreen";
            return false;
        }
        return ok(mimhid_touchscreen_contact(nodes_[node_touch], contact, down ? 1U : 0U, x, y,
                                             nullptr));
    }

    [[nodiscard]] bool flush(const uint8_t nodes) override {
        if (clone_)
            return flush_clone(nodes);
        // Queue every Node's report before waiting on any, so they overlap on
        // the wire instead of running one after another.
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            if ((nodes & node_bit(static_cast<NodeKind>(kind))) != 0 &&
                !ok(mimhid_node_submit(nodes_[kind])))
                return false;
        }
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            // Also sends the fragments a large move or a touch frame still owes.
            if ((nodes & node_bit(static_cast<NodeKind>(kind))) != 0 &&
                !ok(mimhid_node_submit_blocking(nodes_[kind], drain_deadline_ms)))
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
        mimhid_device_info info{};
        if (!ok(mimhid_device_board_info(device_, &info)))
            return false;
        if (const std::string problem = interface_limit_error(used, info.max_nodes, info.max_interfaces);
            !problem.empty()) {
            error_ = problem;
            return false;
        }

        if (!ok(mimhid_device_batch_begin(device_)))
            return false;
        if (!close_nodes())
            return false;
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            if ((wanted & node_bit(static_cast<NodeKind>(kind))) == 0)
                continue;
            if (!open_node(static_cast<NodeKind>(kind), numbers[kind], options))
                return false;
        }
        if (!ok(mimhid_device_batch_end(device_)))
            return false;
        if (wanted != 0 && !ok(mimhid_device_wait_ready(device_, ready_timeout_ms)))
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
                                              [](const mimhid_node* n) { return n != nullptr; });
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
        mimhid_device_info info{};
        if (!ok(mimhid_device_board_info(device_, &info)))
            return false;
        const std::vector<CloneReports::Node>& nodes = built.nodes();
        size_t bytes = 0;
        for (const CloneReports::Node& each : nodes)
            bytes += each.descriptor.size();
        if (nodes.size() > info.max_nodes || nodes.size() > info.max_interfaces) {
            error_ = "the clone presents " + std::to_string(nodes.size()) +
                     " interfaces; the board holds " +
                     std::to_string(std::min(info.max_nodes, info.max_interfaces));
            return false;
        }
        if (bytes > info.max_descriptor_bytes) {
            error_ = "the clone's descriptors are " + std::to_string(bytes) +
                     " bytes; the board has room for " + std::to_string(info.max_descriptor_bytes);
            return false;
        }

        if (!ok(mimhid_device_batch_begin(device_)))
            return false;
        if (!close_nodes())
            return false;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const CloneInterface& itf = shape.device.interfaces[nodes[i].source];
            mimhid_node_options node{};
            node.struct_size = static_cast<uint32_t>(sizeof(node));
            // The board numbers its interfaces without gaps, so the number is the place
            // among those presented.
            node.interface_number = static_cast<uint32_t>(i);
            node.poll_interval_ms = itf.interval_ms;
            node.ep_in_size = itf.ep_in_size;
            node.ep_out_size = itf.ep_out_size;
            node.boot_protocol = MIMHID_BOOT_NONE;
            if (itf.boot_subclass == 1 && (itf.boot_protocol == 1 || itf.boot_protocol == 2))
                node.boot_protocol = itf.boot_protocol;
            node.bcd_hid = itf.bcd_hid;
            node.country_code = itf.country_code;
            node.interface_name = itf.name.empty() ? nullptr : itf.name.c_str();
            mimhid_node* opened = nullptr;
            if (!ok(mimhid_node_open_raw(device_, nodes[i].descriptor.data(),
                                         nodes[i].descriptor.size(), &node, &opened)))
                return false;
            clone_nodes_.push_back(opened);
        }
        if (!ok(mimhid_device_batch_end(device_)))
            return false;
        if (!nodes.empty() && !ok(mimhid_device_wait_ready(device_, ready_timeout_ms)))
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
            static_cast<void>(mimhid_device_close(device_));
        device_ = nullptr;
        nodes_ = {};
        clone_nodes_.clear();
    }

    [[nodiscard]] std::string error() const override { return error_; }

    // The serial port going away is only learned of by asking.
    [[nodiscard]] bool healthy() override {
        return device_ == nullptr || ok(mimhid_device_latched_error(device_));
    }

  private:
    [[nodiscard]] bool ok(const mimhid_result result) {
        if (result == MIMHID_OK)
            return true;
        error_ = describe(result);
        return false;
    }

    // Closes every Node, of the profiles and of a clone, inside a batch the caller opened.
    [[nodiscard]] bool close_nodes() {
        for (mimhid_node*& node : nodes_) {
            if (node == nullptr)
                continue;
            if (!ok(mimhid_node_close(node)))
                return false;
            node = nullptr;
        }
        for (mimhid_node*& node : clone_nodes_) {
            if (node == nullptr)
                continue;
            if (!ok(mimhid_node_close(node)))
                return false;
            node = nullptr;
        }
        clone_nodes_.clear();
        clone_.reset();
        return true;
    }

    // One report of the clone: an Input report goes out as it is; a Feature report is what
    // the board answers a read with, which it holds up to 32 bytes of.
    [[nodiscard]] bool send(const CloneOut& out) {
        mimhid_node* node = clone_nodes_[out.node];
        if (out.feature) {
            if (out.bytes.size() > 32)
                return true;
            return ok(mimhid_node_set_feature_response(node, out.report_id, out.bytes.data(),
                                                       out.bytes.size()));
        }
        return ok(mimhid_report_send(node, out.bytes.data(), out.bytes.size()));
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

    [[nodiscard]] bool open_node(const NodeKind kind, const uint32_t interface,
                                 const LinkOptions& options) {
        // Each Spec is created in its own statement: as a call argument it would
        // be read before the factory beside it had filled it in.
        mimhid_spec* spec = nullptr;
        mimhid_result result = MIMHID_OK;
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
        mimhid_node_options node{};
        node.struct_size = static_cast<uint32_t>(sizeof(node));
        node.interface_number = interface;
        const NodeUsb& usb = options.node_usb[kind];
        node.poll_interval_ms = usb.poll_interval_ms.value_or(1U);
        node.ep_in_size = usb.ep_in_size.value_or(64U);
        node.ep_out_size = usb.ep_out_size.value_or(0U);
        node.boot_protocol = MIMHID_BOOT_NONE;
        node.bcd_hid = usb.bcd_hid ? static_cast<uint16_t>(*usb.bcd_hid) : hid_bcd;
        node.country_code = static_cast<uint8_t>(usb.country_code.value_or(0U));
        node.interface_name = usb.interface_name.empty() ? nullptr : usb.interface_name.c_str();
        result = mimhid_node_open(device_, spec, &node, &nodes_[kind]);
        // The Node keeps its own reference to the Spec.
        mimhid_spec_release(spec);
        if (!ok(result)) {
            nodes_[kind] = nullptr;
            return false;
        }
        return true;
    }

    mimhid_device* device_;
    std::string path_;
    unsigned buttons_;
    std::shared_ptr<Registry> registry_;
    std::array<mimhid_node*, node_kinds> nodes_{};
    // The size and descriptor values the touch Node was built for.
    Size touch_size_{};
    TouchHid touch_hid_;
    // The interface values the Nodes were built with.
    std::array<NodeUsb, usb_nodes> node_usb_{};
    // The clone presented, without its battery level, and what it was built under.
    std::optional<CloneSnapshot> clone_;
    CloneRoles clone_roles_;
    CloneReports reports_;
    std::vector<mimhid_node*> clone_nodes_;
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
        // libmimhid opens a port for one user at a time, so the search passes
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
        const std::string missing = missing_values(options.mim, options.clone.has_value());
        if (!missing.empty()) {
            error = "not set: " + missing;
            return nullptr;
        }
        const MimOptions& mim = options.mim;

        mimhid_device_options device{};
        device.struct_size = static_cast<uint32_t>(sizeof(device));
        device.baud_rate = mim.baud;
        device.control_timeout_ms = link_timeout_ms;
        device.send_timeout_ms = link_timeout_ms;
        device.reconnect_timeout_ms = reconnect_timeout_ms;
        // Zero here is the board's own limit.
        device.maximum_report_bytes = 0U;
        device.close_drain_timeout_ms = close_drain_timeout_ms;
        // The Linux HID parser limits listed in libaoahid's docs/LIMITS.md.
        device.linux_descriptor_policy_bytes = descriptor_policy_bytes;
        device.linux_hid_fields_per_report_policy = 256U;
        device.linux_hid_global_stack_depth_policy = 4U;
        device.linux_hid_usages_policy = 12288U;
        device.linux_hid_report_data_policy_bits = 65528U;
        device.linux_hid_report_size_policy_bits = 256U;
        // The Nodes are fixed and range-checked by the profile factories.
        device.validate_reports = 0U;
        // The board detaches from its target when this program closes it.
        device.keep_attached = 0U;
        device.vendor_id = mim.vendor_id;
        device.product_id = mim.product_id;
        device.bcd_device = mim.bcd_device != 0 ? mim.bcd_device : usb_bcd_device;
        device.language_id = mim.language_id != 0 ? mim.language_id : usb_language_id;
        device.manufacturer = mim.manufacturer.c_str();
        device.product = mim.product.c_str();
        device.serial = mim.serial.empty() ? nullptr : mim.serial.c_str();
        device.configuration = mim.configuration.empty() ? nullptr : mim.configuration.c_str();
        device.serial_from_chip_id = mim.serial_from_chip_id ? 1U : 0U;
        device.self_powered = mim.self_powered ? 1U : 0U;
        device.remote_wakeup = mim.remote_wakeup ? 1U : 0U;
        device.max_power_ma = mim.max_power_ma != 0 ? mim.max_power_ma : usb_max_power_ma;
        device.log_level = MIMHID_LOG_DISABLED;

        mimhid_device* opened = nullptr;
        const mimhid_result result = mimhid_device_open(path.c_str(), &device, &opened);
        if (result != MIMHID_OK) {
            error = "the board could not be opened: " + describe(result);
            return nullptr;
        }
        auto port = std::make_unique<MimPort>(opened, path, options.buttons, registry_);
        uint8_t added = 0;
        if (!port->set_nodes(options, added)) {
            error = "the input devices could not be registered: " + port->error();
            return nullptr;
        }
        return port;
    }

  private:
    struct Listed {
        std::string path;
        std::string description;
        uint32_t baud{};
    };

    // Sends a message to every serial port that could be a board and keeps the
    // ones that answer.
    void probe(std::string& error) {
        found_.clear();
        mimhid_discovery* discovery = nullptr;
        const mimhid_result result = mimhid_discover(discover_timeout_ms, &discovery);
        if (result != MIMHID_OK) {
            error = "the search for boards failed: " + describe(result);
            return;
        }
        const size_t count = mimhid_discovery_count(discovery);
        for (size_t index = 0; index < count; ++index) {
            const mimhid_device_info* info = mimhid_discovery_get(discovery, index);
            if (info == nullptr || info->port_name[0] == '\0')
                continue;
            std::string text = info->description;
            text += " (firmware " + std::to_string(info->version_major) + "." +
                    std::to_string(info->version_minor) + "." +
                    std::to_string(info->version_patch) + ")";
            // It answers, but cannot be opened until it is flashed again.
            if (info->protocol_version != MIMHID_PROTOCOL_VERSION) {
                text += ": flash the libmimhid " + std::to_string(MIMHID_VERSION_MAJOR) + "." +
                        std::to_string(MIMHID_VERSION_MINOR) + "." +
                        std::to_string(MIMHID_VERSION_PATCH) + " firmware";
                found_.push_back({info->port_name, std::move(text), 0U});
                continue;
            }
            found_.push_back({info->port_name, std::move(text), info->baud_rate});
        }
        mimhid_discovery_destroy(discovery);
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
