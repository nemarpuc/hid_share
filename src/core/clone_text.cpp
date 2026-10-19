// SPDX-License-Identifier: MIT
#include "hid_share/clone.hpp"

#include <charconv>
#include <cstdio>

namespace hids {

namespace {

bool to_number(const std::string_view text, const uint32_t low, const uint32_t high,
               uint32_t& out) noexcept {
    uint32_t v = 0;
    int base = 10;
    std::string_view digits = text;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
    if (ec != std::errc() || end != digits.data() + digits.size() || v < low || v > high)
        return false;
    out = v;
    return true;
}

bool to_bool(const std::string_view text, bool& out) noexcept {
    if (text == "true" || text == "yes" || text == "on") {
        out = true;
        return true;
    }
    if (text == "false" || text == "no" || text == "off") {
        out = false;
        return true;
    }
    return false;
}

std::string hex_of(const std::vector<uint8_t>& bytes) {
    std::string out;
    char pair[3];
    for (const uint8_t b : bytes) {
        std::snprintf(pair, sizeof pair, "%02X", static_cast<unsigned>(b));
        out += pair;
    }
    return out;
}

bool bytes_of(const std::string_view hex, std::vector<uint8_t>& out) {
    out.clear();
    if (hex.size() % 2 != 0)
        return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint32_t v = 0;
        const auto [end, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, v, 16);
        if (ec != std::errc() || end != hex.data() + i + 2)
            return false;
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

// A string the board announces: printable ASCII, at most 31 characters.
bool announced(const std::string_view text) noexcept {
    if (text.size() > 31)
        return false;
    for (const char c : text)
        if (c < 0x20 || c > 0x7E)
            return false;
    return true;
}

std::string id_text(const uint16_t id) {
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "0x%04X", static_cast<unsigned>(id));
    return buffer;
}

std::string yes_no(const bool on) { return on ? "yes" : "no"; }

std::string interface_text(const CloneInterface& f) {
    const std::vector<uint8_t> name(f.name.begin(), f.name.end());
    std::string out;
    for (const std::string& part :
         {std::to_string(f.number), std::to_string(f.boot_subclass),
          std::to_string(f.boot_protocol), id_text(f.bcd_hid), std::to_string(f.country_code),
          std::to_string(f.ep_in_size), std::to_string(f.ep_out_size),
          std::to_string(f.interval_ms), name.empty() ? std::string("-") : hex_of(name),
          hex_of(f.descriptor)}) {
        if (!out.empty())
            out += ' ';
        out += part;
    }
    return out;
}

std::string set_interface(CloneSnapshot& s, const size_t index, const std::string_view value) {
    const std::string at = "interface_" + std::to_string(index);
    std::vector<std::string_view> t;
    for (size_t from = 0;;) {
        const size_t end = value.find(' ', from);
        if (end == std::string_view::npos) {
            t.push_back(value.substr(from));
            break;
        }
        t.push_back(value.substr(from, end - from));
        from = end + 1;
    }
    if (t.size() != 10)
        return at + " needs 10 fields";
    CloneInterface f;
    uint32_t v[8] = {};
    const uint32_t high[8] = {255, 255, 255, 0xFFFF, 35, 64, 64, 255};
    for (size_t i = 0; i < 8; ++i)
        if (!to_number(t[i], 0, high[i], v[i]))
            return at + " field " + std::to_string(i + 1) + " is not a number in range";
    f.number = static_cast<uint8_t>(v[0]);
    f.boot_subclass = static_cast<uint8_t>(v[1]);
    f.boot_protocol = static_cast<uint8_t>(v[2]);
    f.bcd_hid = static_cast<uint16_t>(v[3]);
    f.country_code = static_cast<uint8_t>(v[4]);
    f.ep_in_size = v[5];
    f.ep_out_size = v[6];
    f.interval_ms = v[7];
    if (t[8] != "-") {
        std::vector<uint8_t> name;
        if (!bytes_of(t[8], name))
            return at + " has a name that is not hex";
        f.name.assign(name.begin(), name.end());
        if (!announced(f.name))
            return at + " has a name that is not printable";
    }
    if (!bytes_of(t[9], f.descriptor) || f.descriptor.empty())
        return at + " has a descriptor that is not hex";
    HidDescriptor parsed;
    if (const std::string e = parse_descriptor(f.descriptor, parsed); !e.empty())
        return at + ": " + e;
    // Lines come in order; an interface left out would shift the others.
    if (index != s.device.interfaces.size())
        return at + " is out of order";
    s.device.interfaces.push_back(std::move(f));
    return {};
}

} // namespace

std::vector<std::pair<std::string, std::string>> clone_lines(const CloneSnapshot& s) {
    const CloneDevice& d = s.device;
    std::string notes;
    for (const std::string& n : d.notes) {
        if (!notes.empty())
            notes += " | ";
        notes += n;
    }
    std::vector<std::pair<std::string, std::string>> lines = {
        {"source", s.source},
        {"vendor_id", id_text(d.vendor_id)},
        {"product_id", id_text(d.product_id)},
        {"bcd_device", id_text(d.bcd_device)},
        {"manufacturer", d.manufacturer},
        {"product", d.product},
        {"serial", d.serial},
        {"self_powered", yes_no(d.self_powered)},
        {"remote_wakeup", yes_no(d.remote_wakeup)},
        {"max_power_ma", std::to_string(d.max_power_ma)},
        {"battery", std::to_string(s.battery)},
        {"notes", notes},
    };
    for (size_t i = 0; i < d.interfaces.size(); ++i)
        lines.emplace_back("interface_" + std::to_string(i), interface_text(d.interfaces[i]));
    return lines;
}

std::string set_clone_key(CloneSnapshot& s, const std::string_view key,
                          const std::string_view value) {
    CloneDevice& d = s.device;
    uint32_t v = 0;
    if (key == "source") {
        if (value.size() > 80)
            return "source is longer than 80 characters";
        s.source = std::string(value);
        return {};
    }
    if (key == "vendor_id" || key == "product_id") {
        if (!to_number(value, 1, 0xFFFF, v))
            return std::string(key) + " is not 0x0001 to 0xFFFF";
        (key == "vendor_id" ? d.vendor_id : d.product_id) = static_cast<uint16_t>(v);
        return {};
    }
    if (key == "bcd_device") {
        if (!to_number(value, 0, 0xFFFF, v))
            return "bcd_device is not 0x0000 to 0xFFFF";
        d.bcd_device = static_cast<uint16_t>(v);
        return {};
    }
    if (key == "manufacturer" || key == "product" || key == "serial") {
        if (!announced(value))
            return std::string(key) + " is not printable ASCII of at most 31 characters";
        (key == "manufacturer" ? d.manufacturer : key == "product" ? d.product : d.serial) =
            std::string(value);
        return {};
    }
    if (key == "self_powered" || key == "remote_wakeup") {
        bool on = false;
        if (!to_bool(value, on))
            return std::string(key) + " is not yes or no";
        (key == "self_powered" ? d.self_powered : d.remote_wakeup) = on;
        return {};
    }
    if (key == "max_power_ma") {
        if (!to_number(value, 0, 510, v))
            return "max_power_ma is not 0 to 510";
        d.max_power_ma = v;
        return {};
    }
    if (key == "battery") {
        if (!to_number(value, 0, 100, v))
            return "battery is not 0 to 100";
        s.battery = v;
        return {};
    }
    if (key == "notes") {
        d.notes.clear();
        for (size_t at = 0; at < value.size();) {
            size_t end = value.find(" | ", at);
            if (end == std::string_view::npos)
                end = value.size();
            d.notes.emplace_back(value.substr(at, end - at));
            at = end + 3;
        }
        return {};
    }
    if (key.starts_with("interface_") && key.size() > 10) {
        uint32_t index = 0;
        if (!to_number(key.substr(10), 0, clone_max_interfaces - 1, index))
            return "there are interface_0 to interface_" + std::to_string(clone_max_interfaces - 1);
        return set_interface(s, index, value);
    }
    return "unknown key " + std::string(key);
}

std::string clone_problem(const CloneSnapshot& s) {
    if (s.device.vendor_id == 0 || s.device.product_id == 0)
        return "the clone has no vendor id and product id";
    if (s.device.interfaces.empty())
        return "the clone has no interface";
    return {};
}

} // namespace hids
