// SPDX-License-Identifier: MIT
#include "mice.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

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

// A pointing device has relative X and Y axes (bits 0 and 1 of the map).
bool is_pointer(const fs::path& input) {
    unsigned long rel = 0;
    return read_hex(input / "capabilities" / "rel", rel) && (rel & 3) == 3;
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
        if (error || !is_pointer(input))
            continue;
        const fs::path usb = usb_parent(input);
        unsigned long vendor = 0;
        unsigned long product = 0;
        if (usb.empty() || !read_hex(usb / "idVendor", vendor) ||
            !read_hex(usb / "idProduct", product) || vendor == 0 || vendor > 0xFFFF ||
            product == 0 || product > 0xFFFF)
            continue;
        Mouse mouse;
        mouse.name = read_line(input / "name");
        mouse.vendor_id = static_cast<uint16_t>(vendor);
        mouse.product_id = static_cast<uint16_t>(product);
        mouse.manufacturer = read_line(usb / "manufacturer");
        mouse.product = read_line(usb / "product");
        mouse.serial = read_line(usb / "serial");
        // One mouse shows up as several input devices.
        const bool seen = std::any_of(mice.begin(), mice.end(), [&](const Mouse& each) {
            return each.vendor_id == mouse.vendor_id && each.product_id == mouse.product_id &&
                   each.name == mouse.name;
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
