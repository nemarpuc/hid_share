// SPDX-License-Identifier: MIT
#include "mice.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace gui {
namespace {

namespace fs = std::filesystem;

std::string read_line(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

bool read_hex(const fs::path& path, unsigned long& value) {
    const std::string text = read_line(path);
    if (text.empty())
        return false;
    char* end = nullptr;
    value = std::strtoul(text.c_str(), &end, 16);
    return *end == '\0';
}

// A bit of a capability map such as "260800 0": words of the machine's long,
// the highest first.
bool map_has(const std::string& text, const unsigned bit) {
    std::vector<unsigned long> words;
    std::istringstream in(text);
    for (std::string word; in >> word;)
        words.push_back(std::strtoul(word.c_str(), nullptr, 16));
    constexpr unsigned width = sizeof(unsigned long) * 8;
    const size_t at = bit / width;
    return at < words.size() && ((words[words.size() - 1 - at] >> (bit % width)) & 1UL) != 0;
}

// A mouse has relative X and Y axes. A touchscreen has multi-touch X and Y
// and is marked as one that is touched directly, which a touchpad is not.
bool kind_of(const fs::path& input, PointerKind& kind) {
    const std::string rel = read_line(input / "capabilities" / "rel");
    if (map_has(rel, 0) && map_has(rel, 1)) {
        kind = PointerKind::mouse;
        return true;
    }
    constexpr unsigned abs_mt_position_x = 0x35;
    constexpr unsigned abs_mt_position_y = 0x36;
    constexpr unsigned input_prop_direct = 1;
    const std::string abs = read_line(input / "capabilities" / "abs");
    if (map_has(abs, abs_mt_position_x) && map_has(abs, abs_mt_position_y) &&
        map_has(read_line(input / "properties"), input_prop_direct)) {
        kind = PointerKind::touchscreen;
        return true;
    }
    return false;
}

// "480mA" -> 480; 0 when it is not that.
uint16_t milliamps(const std::string& text) {
    const unsigned long value = std::strtoul(text.c_str(), nullptr, 10);
    return value <= 0xFFFF ? static_cast<uint16_t>(value) : 0;
}

// The USB device an input device hangs from: the nearest parent that has ids.
fs::path usb_parent(fs::path path) {
    while (path.has_parent_path() && path != path.root_path()) {
        if (fs::exists(path / "idVendor"))
            return path;
        path = path.parent_path();
    }
    return {};
}

} // namespace

#ifdef __linux__
std::vector<Mouse> list_mice() {
    std::vector<Mouse> mice;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator("/sys/class/input", error)) {
        if (entry.path().filename().string().rfind("event", 0) != 0)
            continue;
        const fs::path input = fs::canonical(entry.path() / "device", error);
        PointerKind kind{};
        if (error || !kind_of(input, kind))
            continue;
        const fs::path usb = usb_parent(input);
        unsigned long vendor = 0;
        unsigned long product = 0;
        if (usb.empty() || !read_hex(usb / "idVendor", vendor) ||
            !read_hex(usb / "idProduct", product) || vendor == 0 || vendor > 0xFFFF ||
            product == 0 || product > 0xFFFF)
            continue;
        Mouse mouse;
        mouse.kind = kind;
        mouse.name = read_line(input / "name");
        mouse.vendor_id = static_cast<uint16_t>(vendor);
        mouse.product_id = static_cast<uint16_t>(product);
        mouse.manufacturer = read_line(usb / "manufacturer");
        mouse.product = read_line(usb / "product");
        mouse.serial = read_line(usb / "serial");
        mouse.configuration = read_line(usb / "configuration");
        unsigned long release = 0;
        if (read_hex(usb / "bcdDevice", release) && release <= 0xFFFF)
            mouse.bcd_device = static_cast<uint16_t>(release);
        mouse.max_power_ma = milliamps(read_line(usb / "bMaxPower"));
        // bmAttributes: bit 6 self powered, bit 5 remote wakeup.
        unsigned long attributes = 0;
        if (read_hex(usb / "bmAttributes", attributes)) {
            mouse.self_powered = (attributes & 0x40) != 0;
            mouse.remote_wakeup = (attributes & 0x20) != 0;
        }
        // One mouse shows up as several input devices.
        const bool seen = std::any_of(mice.begin(), mice.end(), [&](const Mouse& each) {
            return each.kind == mouse.kind && each.vendor_id == mouse.vendor_id &&
                   each.product_id == mouse.product_id && each.name == mouse.name;
        });
        if (!seen)
            mice.push_back(std::move(mouse));
    }
    return mice;
}
#else
std::vector<Mouse> list_mice() {
    return {};
}
#endif

} // namespace gui
