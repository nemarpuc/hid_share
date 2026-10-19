// SPDX-License-Identifier: MIT
// Clone mode: a board presents a snapshot of a plugged-in USB HID device. This header reads
// a HID report descriptor into collections and fields, which is all the rest of the mode
// needs to find the keyboard, mouse, media and battery parts and to build their reports.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hids {

// One Input or Feature main item. Usages carry their page in the upper 16 bits.
struct HidField {
    uint8_t report_id{};
    bool feature{};
    bool variable{};
    bool constant{};
    uint32_t bit_offset{}; // from the first bit after the Report ID byte
    uint32_t bit_size{};
    uint32_t count{};
    uint32_t usage_page{};
    std::vector<uint32_t> usages;
    bool ranged{};
    uint32_t usage_min{};
    uint32_t usage_max{};
    int32_t logical_min{};
    int32_t logical_max{};

    // The usage of the i-th element of the field; 0 when the field names none.
    uint32_t usage_at(uint32_t i) const;
};

// A top-level collection: its bytes in the descriptor (from the first item after the
// previous collection up to its End Collection) and every field inside it.
struct HidCollection {
    size_t first_byte{};
    size_t last_byte{}; // one past the End Collection item
    uint32_t usage_page{};
    uint32_t usage{};
    std::vector<HidField> fields;
};

struct HidReportSize {
    uint8_t report_id{};
    bool feature{};
    uint32_t bits{};
};

struct HidDescriptor {
    std::vector<HidCollection> collections;
    std::vector<HidReportSize> reports;
};

// Reads a report descriptor. Returns an empty string, or what is wrong with it (a
// truncated item, a long item, an End Collection without a Collection, an unclosed one).
std::string parse_descriptor(std::span<const uint8_t> bytes, HidDescriptor& out);

constexpr uint32_t usage_page_generic_desktop = 0x01;
constexpr uint32_t usage_page_keyboard = 0x07;
constexpr uint32_t usage_page_button = 0x09;
constexpr uint32_t usage_page_consumer = 0x0C;
constexpr uint32_t usage_page_generic_device = 0x06;

bool is_keyboard(const HidCollection&) noexcept;
bool is_mouse(const HidCollection&) noexcept;
bool is_consumer(const HidCollection&) noexcept;

// Where a Battery Strength field (Generic Device Controls 0x06, usage 0x20) sits.
struct BatteryAt {
    size_t collection{};
    size_t field{};
};
std::optional<BatteryAt> find_battery(const HidDescriptor&) noexcept;

// What the board is told about a plugged-in device: every HID interface with the values its
// descriptors carry, and the limits of the copy that apply to this device.
struct CloneInterface {
    uint8_t number{};
    uint8_t boot_subclass{};
    uint8_t boot_protocol{};
    uint16_t bcd_hid{};
    uint8_t country_code{};
    std::string name;
    uint32_t ep_in_size{};
    uint32_t ep_out_size{}; // 0: no OUT endpoint
    uint32_t interval_ms{};
    std::vector<uint8_t> descriptor;

    [[nodiscard]] bool operator==(const CloneInterface&) const = default;
};

struct CloneDevice {
    uint16_t vendor_id{};
    uint16_t product_id{};
    uint16_t bcd_device{};
    std::string manufacturer;
    std::string product;
    std::string serial;
    bool self_powered{};
    bool remote_wakeup{};
    uint32_t max_power_ma{};
    std::vector<CloneInterface> interfaces;
    std::vector<std::string> notes;

    [[nodiscard]] bool operator==(const CloneDevice&) const = default;
};

// A device as it was read, kept in the device file ([clone]) so the board presents it
// whether or not the original is plugged in.
struct CloneSnapshot {
    std::string source; // what it was called when read, for the window
    CloneDevice device;
    uint32_t battery{100}; // percent, for a device that has a battery

    [[nodiscard]] bool operator==(const CloneSnapshot&) const = default;
};

// A plugged-in USB device that has a HID interface, as the kernel lists it.
struct CloneSource {
    std::string id; // the sysfs name, "5-2"
    uint16_t vendor_id{};
    uint16_t product_id{};
    std::string manufacturer;
    std::string product;
    unsigned hid_interfaces{};
};

// Where the kernel lists USB devices.
inline const char* const sysfs_usb_devices = "/sys/bus/usb/devices";

// The devices under `root` with at least one HID interface, read without any privilege.
[[nodiscard]] std::vector<CloneSource>
list_clone_sources(const std::filesystem::path& root = sysfs_usb_devices);

// Reads one of them whole: the device and configuration values, and every HID interface
// with its report descriptor. What cannot be copied is told in `out.notes`. Returns what
// stopped it, or empty.
[[nodiscard]] std::string read_clone_device(const CloneSource& source,
                                            const std::filesystem::path& root,
                                            CloneDevice& out);

// The most interfaces a clone holds, as many as the board has Nodes.
constexpr size_t clone_max_interfaces = 8;

// The text of the [clone] section: key and value lines, in the order they are written.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
clone_lines(const CloneSnapshot& snapshot);
// Takes one line of it. Returns what is wrong, or empty.
[[nodiscard]] std::string set_clone_key(CloneSnapshot& snapshot, std::string_view key,
                                        std::string_view value);
// What is missing or cannot be presented in a whole snapshot, or empty.
[[nodiscard]] std::string clone_problem(const CloneSnapshot& snapshot);

// The roles the other computer is told about. A role that is off is left out of the
// descriptors it sees.
struct CloneRoles {
    bool mouse{true};
    bool keyboard{true};
    bool media{true};

    [[nodiscard]] bool operator==(const CloneRoles&) const = default;
};

// Cuts the collections of `original` that `keep` (one flag per top-level collection) leaves
// out. `out` stays empty when none is kept. Fails when what remains no longer reads the
// same, as when a kept collection relied on a global item set inside a dropped one.
std::string present_descriptor(std::span<const uint8_t> original, const HidDescriptor& parsed,
                               const std::vector<bool>& keep, std::vector<uint8_t>& out);

// One report to send. `bytes` starts with the Report ID when the interface has Report IDs;
// for a Feature report it holds the data alone, as the board answers a GET_REPORT with it.
struct CloneOut {
    size_t node{}; // index into CloneReports::nodes()
    uint8_t report_id{};
    bool feature{};
    std::vector<uint8_t> bytes;
};

// The interfaces the other computer sees under the roles, and the reports of the
// keyboard, mouse, media keys and battery in their own format. One keyboard, one mouse and
// one consumer collection are fed (the one with the most keys wins a tie among keyboards,
// the earliest otherwise), so a key never arrives twice.
class CloneReports {
public:
    struct Node {
        size_t source{}; // index into CloneDevice::interfaces
        std::vector<uint8_t> descriptor;
        HidDescriptor parsed;
        bool has_ids{};
    };

    static std::string build(const CloneDevice&, CloneRoles, CloneReports& out);

    const std::vector<Node>& nodes() const noexcept { return nodes_; }
    bool has_keyboard() const noexcept { return keyboard_.valid(); }
    bool has_mouse() const noexcept { return mouse_.valid(); }
    bool has_media() const noexcept { return consumer_.valid(); }
    bool has_battery() const noexcept { return battery_.valid(); }
    // Where the battery is: the index in nodes() and whether it is a Feature report.
    size_t battery_node() const noexcept { return battery_.node; }
    bool battery_is_feature() const noexcept { return battery_feature_; }

    // `usage` is a Keyboard/Keypad page usage id. Empty when there is no keyboard.
    std::vector<CloneOut> key(uint8_t usage, bool down);
    // `usage` is a Consumer page usage. Empty when no field of the device can carry it.
    std::vector<CloneOut> consumer(uint16_t usage, bool down);
    // Bit n of `buttons` is button n + 1. A move too big for one report is split.
    std::vector<CloneOut> mouse(int dx, int dy, int wheel, int pan, uint32_t buttons);
    // `percent` is 0 to 100, scaled to the field's range.
    std::optional<CloneOut> battery(int percent) const;
    // Every key, media key and button let go: the reports that say so.
    std::vector<CloneOut> neutral();

private:
    struct Target {
        size_t node{static_cast<size_t>(-1)};
        size_t collection{};
        bool valid() const noexcept { return node != static_cast<size_t>(-1); }
    };

    std::vector<CloneOut> keyboard_reports() const;
    std::vector<CloneOut> consumer_reports() const;

    std::vector<Node> nodes_;
    Target keyboard_;
    Target mouse_;
    Target consumer_;
    Target battery_;
    size_t battery_field_{};
    bool battery_feature_{};
    std::vector<uint8_t> pressed_keys_;
    std::vector<uint16_t> pressed_media_;
};

} // namespace hids
