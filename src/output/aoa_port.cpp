// SPDX-License-Identifier: MIT
// A phone reached over Android Open Accessory, through libaoahid.
#include "port.hpp"

#include <aoahid.h>
#include <aoahid_adb_proxy.h>

#include <array>
#include <iterator>

namespace hids {
namespace {

// Every value below is this application's policy, stated explicitly: libaoahid
// has no defaults, and takes every wait as an argument of the call that waits.
constexpr uint32_t discover_timeout_ms = 500U;
constexpr uint32_t control_timeout_ms = 500U;
constexpr uint32_t send_timeout_ms = 500U;
constexpr uint32_t descriptor_policy_bytes = 4096U;
constexpr uint32_t report_policy_bytes = 1024U;
constexpr uint32_t pool_slots = 8U;
constexpr uint32_t close_drain_timeout_ms = 1000U;
constexpr uint32_t drain_deadline_ms = 600U;
// How long a Node added to an open phone waits for Android to register it.
constexpr int64_t node_settle_ns = 150'000'000;
constexpr uint32_t destroy_timeout_ms = 2000U;
constexpr int max_slots = 8;

static_assert(proxy_refused == AOAHID_ADB_PROXY_ERR_ARGUMENT);

// Consumer page usages the toggle Node declares; the key map only produces
// these. Same list and order as aoahid_player's media_usages.
constexpr uint16_t media_usages[] = {0x00B5, 0x00B6, 0x00B7, 0x00CD, 0x00E2, 0x00E9, 0x00EA,
                                     0x0223, 0x0224, 0x0238, 0x0201, 0x006F, 0x0070};

aoahid_device_options device_options() noexcept {
    aoahid_device_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.descriptor_fragment_bytes = descriptor_policy_bytes;
    options.transfer_pool_slots = pool_slots;
    options.maximum_report_bytes = report_policy_bytes;
    // The specs are fixed and range-checked by the profile factories.
    options.validate_reports = 0U;
    options.aoa_descriptor_wire_policy_bytes = descriptor_policy_bytes;
    options.linux_descriptor_policy_bytes = descriptor_policy_bytes;
    // The Linux HID parser limits listed in libaoahid's docs/LIMITS.md.
    options.linux_hid_fields_per_report_policy = 256U;
    options.linux_hid_global_stack_depth_policy = 4U;
    options.linux_hid_usages_policy = 12288U;
    options.linux_hid_report_data_policy_bits = 65528U;
    options.linux_hid_report_size_policy_bits = 256U;
    options.target_ep0_data_policy_bytes = descriptor_policy_bytes;
    options.host_control_buffer_policy_bytes = descriptor_policy_bytes;
    options.interface_claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
    options.interface_number = -1;
    return options;
}

aoahid_node_options node_options() noexcept {
    aoahid_node_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // One report in flight per Node; a reserved slot keeps the three Nodes
    // from competing for the pool.
    options.has_reserved_slots = 1U;
    options.reserved_slots = 1U;
    return options;
}

aoahid_integer_field field(const int32_t minimum, const int32_t maximum,
                           const uint32_t bits) noexcept {
    aoahid_integer_field value{};
    value.logical_minimum = minimum;
    value.logical_maximum = maximum;
    value.bit_width = bits;
    return value;
}

aoahid_result create_mouse(const unsigned buttons, aoahid_spec** spec) noexcept {
    aoahid_mouse_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.button_count = buttons;
    // One report carries up to this much; libaoahid keeps the rest pending.
    options.x = field(-32767, 32767, 16);
    options.y = field(-32767, 32767, 16);
    options.enable_wheel = 1U;
    options.wheel = field(-127, 127, 8);
    options.enable_pan = 1U;
    options.pan = field(-127, 127, 8);
    return aoahid_spec_create_mouse(&options, spec);
}

aoahid_result create_keyboard(aoahid_spec** spec) noexcept {
    aoahid_keyboard_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // Through LANG and the keypad extras; the modifiers 0xE0-0xE7 are always
    // declared by the profile.
    options.usage_minimum = 0x04;
    options.usage_maximum = 0xDD;
    return aoahid_spec_create_keyboard(&options, spec);
}

aoahid_result create_toggle(aoahid_spec** spec) noexcept {
    aoahid_toggle_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.application_page = 0x0CU;  // Consumer
    options.application_usage = 0x01U; // Consumer Control
    options.field_page = 0x0CU;
    // In media_usages order: Next, Prev, Stop, Play/Pause, Mute, Vol Up,
    // Vol Down, AC Home, AC Back, AC Pan, AC New, Brightness Up, Brightness Down.
    static const aoahid_usage_semantic semantics[] = {
        AOAHID_USAGE_ONE_SHOT,  AOAHID_USAGE_ONE_SHOT,          AOAHID_USAGE_ONE_SHOT,
        AOAHID_USAGE_ONE_SHOT,  AOAHID_USAGE_ON_OFF_MAINTAINED, AOAHID_USAGE_RETRIGGER,
        AOAHID_USAGE_RETRIGGER, AOAHID_USAGE_ONE_SHOT,          AOAHID_USAGE_ONE_SHOT,
        AOAHID_USAGE_ONE_SHOT,  AOAHID_USAGE_SELECTOR_BITMAP,   AOAHID_USAGE_RETRIGGER,
        AOAHID_USAGE_RETRIGGER};
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
    return aoahid_spec_create_toggle(&options, spec);
}

// Declares an optional field when its maximum is given.
void optional_field(const std::optional<uint32_t>& maximum, const std::optional<uint32_t>& bits,
                    uint32_t& enable, aoahid_integer_field& out) noexcept {
    if (!maximum)
        return;
    enable = 1U;
    out = field(0, static_cast<int32_t>(*maximum), bits.value_or(bits_to_hold(*maximum)));
}

aoahid_result create_touchscreen(const Size size, const TouchHid& hid,
                                 aoahid_spec** spec) noexcept {
    aoahid_touchscreen_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // Contact 0 is the click, contact 1 the scroll; one report carries both.
    options.maximum_contacts = hid.maximum_contacts.value_or(2U);
    options.contacts_per_report = hid.contacts_per_report.value_or(2U);
    options.contact_id = field(0, static_cast<int32_t>(hid.contact_id_max.value_or(15U)),
                               hid.contact_id_bits.value_or(4U));
    const auto coordinate_bits = [&](const int32_t extent) {
        return hid.coordinate_bits.value_or(bits_to_hold(extent - 1));
    };
    options.x = field(0, size.w - 1, coordinate_bits(size.w));
    options.y = field(0, size.h - 1, coordinate_bits(size.h));
    options.contact_count = field(0, static_cast<int32_t>(hid.contact_count_max.value_or(2U)),
                                  hid.contact_count_bits.value_or(2U));
    optional_field(hid.pressure_max, hid.pressure_bits, options.enable_pressure, options.pressure);
    optional_field(hid.width_max, hid.width_bits, options.enable_width, options.width);
    optional_field(hid.height_max, hid.height_bits, options.enable_height, options.height);
    optional_field(hid.azimuth_max, hid.azimuth_bits, options.enable_azimuth, options.azimuth);
    optional_field(hid.scan_time_max, hid.scan_time_bits, options.enable_scan_time,
                   options.scan_time);
    options.scan_time_unit_100us = hid.scan_time_unit_100us.value_or(0U);
    return aoahid_spec_create_touchscreen(&options, spec);
}

std::string describe(const aoahid_result result) {
    const aoahid_error_detail* detail = aoahid_last_error();
    // Copy the borrowed strings before aoahid_result_name() replaces them.
    const std::string field_name =
        detail != nullptr && detail->field != nullptr ? detail->field : "no field";
    const std::string reason =
        detail != nullptr && detail->reason != nullptr ? detail->reason : "no detail";
    return std::string(aoahid_result_name(result)) + " (" + field_name + ": " + reason + ")";
}

class AoaPort final : public Port {
  public:
    AoaPort(aoahid_context* context, const unsigned buttons) noexcept
        : context_(context), buttons_(buttons) {}
    ~AoaPort() override { close(); }

    [[nodiscard]] aoahid_result open_device(const aoahid_device_info* info) {
        const aoahid_device_options options = device_options();
        const aoahid_result result = aoahid_device_open(context_, info, &options, &device_);
        if (result != AOAHID_OK)
            device_ = nullptr;
        return result;
    }

    [[nodiscard]] bool has(const NodeKind kind) const noexcept override {
        return nodes_[kind] != nullptr;
    }

    [[nodiscard]] bool mouse_move(const int32_t dx, const int32_t dy) override {
        return ok(aoahid_mouse_move(nodes_[node_mouse], dx, dy));
    }
    [[nodiscard]] bool mouse_scroll(const int32_t wheel, const int32_t pan) override {
        return ok(aoahid_mouse_scroll(nodes_[node_mouse], wheel, pan));
    }
    [[nodiscard]] bool button(const unsigned button, const bool down) override {
        return ok(aoahid_mouse_button(nodes_[node_mouse], button, down ? 1U : 0U));
    }
    [[nodiscard]] bool key(const uint16_t usage, const bool down) override {
        return ok(aoahid_keyboard_key(nodes_[node_keyboard], usage, down ? 1U : 0U));
    }
    [[nodiscard]] bool media(const uint16_t usage, const bool down) override {
        return ok(aoahid_toggle_usage(nodes_[node_media], usage, down ? 1U : 0U));
    }
    [[nodiscard]] bool touch(const unsigned contact, const bool down, const int32_t x,
                             const int32_t y) override {
        return ok(
            aoahid_touchscreen_contact(nodes_[node_touch], contact, down ? 1U : 0U, x, y, nullptr));
    }

    [[nodiscard]] bool flush(const uint8_t nodes) override {
        // Queue every Node's report before waiting on any, so they overlap on
        // the wire instead of running one after another.
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            if ((nodes & node_bit(static_cast<NodeKind>(kind))) != 0 &&
                !ok(aoahid_node_submit(nodes_[kind], send_timeout_ms)))
                return false;
        }
        for (size_t kind = 0; kind < node_kinds; ++kind) {
            // Also drains the fragments a large move still owes.
            if ((nodes & node_bit(static_cast<NodeKind>(kind))) != 0 &&
                !ok(aoahid_node_submit_blocking(nodes_[kind], drain_deadline_ms)))
                return false;
        }
        return true;
    }

    [[nodiscard]] bool set_nodes(const LinkOptions& options, uint8_t& added) override {
        added = 0;
        const aoahid_node_options node = node_options();
        // Each Spec is created in its own statement: as a call argument it would
        // be read before the factory beside it had filled it in.
        const auto sync = [&](const NodeKind kind, const bool wanted,
                              const auto create) -> aoahid_result {
            aoahid_node*& slot = nodes_[kind];
            if (wanted == (slot != nullptr))
                return AOAHID_OK;
            if (!wanted) {
                // Neutral reports first, then the unregister; an error leaves
                // the Node open.
                const aoahid_result closed =
                    aoahid_node_close(slot, control_timeout_ms, close_drain_timeout_ms);
                if (closed == AOAHID_OK)
                    slot = nullptr;
                return closed;
            }
            aoahid_spec* spec = nullptr;
            aoahid_result result = create(&spec);
            if (result != AOAHID_OK)
                return result;
            result = aoahid_node_open(device_, spec, &node, control_timeout_ms, &slot);
            // The Node keeps its own reference to the Spec.
            aoahid_spec_release(spec);
            if (result != AOAHID_OK) {
                slot = nullptr;
                return result;
            }
            added = static_cast<uint8_t>(added | node_bit(kind));
            return AOAHID_OK;
        };
        aoahid_result result = sync(node_mouse, options.mouse, [&](aoahid_spec** spec) {
            return create_mouse(buttons_, spec);
        });
        if (result == AOAHID_OK)
            result = sync(node_keyboard, options.keyboard, create_keyboard);
        if (result == AOAHID_OK)
            result = sync(node_media, options.media, create_toggle);
        // A size that changed means a new Spec: the old Node goes first.
        if (result == AOAHID_OK && nodes_[node_touch] != nullptr && options.touch &&
            (touch_size_.w != options.touch_size.w || touch_size_.h != options.touch_size.h ||
             !(touch_hid_ == options.touch_hid))) {
            result =
                aoahid_node_close(nodes_[node_touch], control_timeout_ms, close_drain_timeout_ms);
            if (result == AOAHID_OK)
                nodes_[node_touch] = nullptr;
        }
        if (result == AOAHID_OK) {
            result = sync(node_touch, options.touch, [&](aoahid_spec** spec) {
                return create_touchscreen(options.touch_size, options.touch_hid, spec);
            });
            if (result == AOAHID_OK) {
                touch_size_ = options.touch_size;
                touch_hid_ = options.touch_hid;
            }
        }
        return ok(result);
    }

    [[nodiscard]] int64_t settle_ns() const noexcept override { return node_settle_ns; }

    [[nodiscard]] int start_proxy(const uint16_t port, bool& held, bool& absent) override {
        held = absent = false;
        if (device_ == nullptr || proxy_ != nullptr)
            return proxy_refused;
        const int result = aoahid_adb_proxy_start(device_, port, close_drain_timeout_ms, &proxy_);
        if (result != AOAHID_ADB_PROXY_OK)
            proxy_ = nullptr;
        if (result == AOAHID_ADB_PROXY_ERR_INTERFACE) {
            // The proxy makes no libaoahid call after a failed channel open, so
            // this thread's last diagnostic is that failure.
            const aoahid_error_detail* detail = aoahid_last_error();
            const int code = detail != nullptr ? detail->code : 0;
            // LIBUSB_ERROR_NOT_SUPPORTED is a driver problem; stopping adb cannot fix it.
            const bool driver = detail != nullptr && detail->transport_status == -12;
            held = !driver && (code == AOAHID_ERR_BUSY || code == AOAHID_ERR_ACCESS);
            absent = !driver && code == AOAHID_ERR_UNSUPPORTED;
        }
        return result;
    }

    void stop_proxy() override {
        aoahid_adb_proxy_stop(proxy_, close_drain_timeout_ms);
        proxy_ = nullptr;
    }

    void close() override {
        // The proxy's threads and Channel must be gone before the Device closes.
        stop_proxy();
        // The first close consumes the Device and its Nodes for every result.
        if (device_ != nullptr)
            static_cast<void>(
                aoahid_device_close(device_, control_timeout_ms, close_drain_timeout_ms));
        device_ = nullptr;
        nodes_ = {};
    }

    [[nodiscard]] std::string error() const override { return error_; }

  private:
    [[nodiscard]] bool ok(const aoahid_result result) {
        if (result == AOAHID_OK)
            return true;
        error_ = describe(result);
        return false;
    }

    aoahid_context* context_;
    unsigned buttons_;
    aoahid_device* device_{};
    std::array<aoahid_node*, node_kinds> nodes_{};
    // The size the touch Node was built for.
    Size touch_size_{};
    TouchHid touch_hid_;
    aoahid_adb_proxy_context* proxy_{};
    std::string error_;
};

class AoaBackend final : public Backend {
  public:
    ~AoaBackend() override { stop(); }

    [[nodiscard]] const char* name() const noexcept override { return "phones (AOA)"; }

    [[nodiscard]] std::string start() override {
        aoahid_context_options options{};
        options.struct_size = static_cast<uint32_t>(sizeof(options));
        // The ADB proxy needs the internal event thread.
        options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
        options.log_level = AOAHID_LOG_DISABLED;
        const aoahid_result result = aoahid_context_create(&options, &scan_context_);
        if (result != AOAHID_OK) {
            scan_context_ = nullptr;
            return "libaoahid could not start: " + describe(result);
        }
        return {};
    }

    void stop() override {
        // A Context is kept per link slot after its phone is gone: it never
        // reuses a HID id, so a phone that comes back to the slot does not
        // meet its old registrations.
        for (aoahid_context*& context : slot_contexts_) {
            if (context != nullptr)
                static_cast<void>(aoahid_context_destroy_blocking(
                    context, control_timeout_ms, close_drain_timeout_ms, destroy_timeout_ms));
            context = nullptr;
        }
        if (discovery_ != nullptr)
            aoahid_discovery_destroy(discovery_);
        discovery_ = nullptr;
        if (scan_context_ != nullptr)
            static_cast<void>(aoahid_context_destroy_blocking(
                scan_context_, control_timeout_ms, close_drain_timeout_ms, destroy_timeout_ms));
        scan_context_ = nullptr;
    }

    [[nodiscard]] std::vector<TargetInfo> scan(const ScanRequest&, std::string& error) override {
        std::vector<TargetInfo> phones;
        if (scan_context_ == nullptr) {
            error = "libaoahid is not running";
            return phones;
        }
        aoahid_discovery* found = nullptr;
        const aoahid_result result = aoahid_discover(scan_context_, discover_timeout_ms, &found);
        if (result != AOAHID_OK) {
            error = "the USB scan failed: " + describe(result);
            return phones;
        }
        if (discovery_ != nullptr)
            aoahid_discovery_destroy(discovery_);
        discovery_ = found;
        const size_t count = aoahid_discovery_count(discovery_);
        for (size_t index = 0; index < count; ++index) {
            const aoahid_device_info* info = aoahid_discovery_get(discovery_, index);
            TargetInfo phone;
            if (info != nullptr) {
                phone.serial = info->serial != nullptr ? info->serial : "";
                phone.product = info->product != nullptr ? info->product : "";
                phone.vendor_id = info->vendor_id;
                phone.product_id = info->product_id;
            }
            phones.push_back(std::move(phone));
        }
        return phones;
    }

    [[nodiscard]] std::unique_ptr<Port> open(const size_t index, const int slot,
                                             const LinkOptions& options,
                                             std::string& error) override {
        // The entry of the last scan is borrowed for the whole call.
        const aoahid_device_info* info =
            discovery_ != nullptr ? aoahid_discovery_get(discovery_, index) : nullptr;
        if (info == nullptr) {
            error = "the phone is not in the last scan";
            return nullptr;
        }
        if (slot < 0 || slot >= max_slots) {
            error = "too many phones are open";
            return nullptr;
        }
        aoahid_context*& context = slot_contexts_[static_cast<size_t>(slot)];
        if (context == nullptr) {
            aoahid_context_options created{};
            created.struct_size = static_cast<uint32_t>(sizeof(created));
            created.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
            created.log_level = AOAHID_LOG_DISABLED;
            const aoahid_result result = aoahid_context_create(&created, &context);
            if (result != AOAHID_OK) {
                context = nullptr;
                error = "libaoahid could not start: " + describe(result);
                return nullptr;
            }
        }
        auto port = std::make_unique<AoaPort>(context, options.buttons);
        const aoahid_result opened = port->open_device(info);
        if (opened != AOAHID_OK) {
            error = "the phone could not be opened: " + describe(opened);
            return nullptr;
        }
        uint8_t added = 0;
        if (!port->set_nodes(options, added)) {
            error = "the input devices could not be registered: " + port->error();
            return nullptr;
        }
        return port;
    }

  private:
    // Discovery has a Context of its own: scanning never waits for a report
    // and no report waits for a scan. The snapshot is kept until the next
    // scan so open() can use an entry of it.
    aoahid_context* scan_context_{};
    aoahid_discovery* discovery_{};
    std::array<aoahid_context*, max_slots> slot_contexts_{};
};

} // namespace

std::unique_ptr<Backend> make_aoa_backend() { return std::make_unique<AoaBackend>(); }

} // namespace hids
