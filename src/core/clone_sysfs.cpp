// SPDX-License-Identifier: MIT
#include "hid_share/clone.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace hids {

namespace fs = std::filesystem;

namespace {

// The most any board holds: an endpoint packet of a high-speed chip. A board with less says so
// when the clone is presented.
constexpr uint32_t board_endpoint_bytes = 1024;

bool slurp(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// A one-line sysfs file without its line end; empty when it is absent.
std::string line_of(const fs::path& path) {
    std::ifstream in(path);
    std::string text;
    std::getline(in, text);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

bool hex16(const std::string& text, uint16_t& out) {
    unsigned v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
    if (text.empty() || ec != std::errc() || end != text.data() + text.size() || v > 0xFFFF)
        return false;
    out = static_cast<uint16_t>(v);
    return true;
}

std::vector<fs::path> interface_dirs(const fs::path& root, const std::string& id) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > id.size() + 1 && name.starts_with(id + ":"))
            dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

bool is_hid(const fs::path& interface_dir) {
    return line_of(interface_dir / "bInterfaceClass") == "03";
}

// The report descriptor the kernel keeps in a folder of the interface, whatever it is called.
bool report_descriptor_of(const fs::path& interface_dir, std::vector<uint8_t>& out) {
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(interface_dir, ec)) {
        if (e.is_symlink(ec) || !e.is_directory(ec))
            continue;
        if (slurp(e.path() / "report_descriptor", out) && !out.empty())
            return true;
    }
    return false;
}

std::string number_of(const CloneInterface& itf) { return std::to_string(itf.number); }

bool printable(const std::string& text) {
    return text.size() <= 31 && std::all_of(text.begin(), text.end(), [](const char c) {
               return c >= 0x20 && c <= 0x7E;
           });
}

} // namespace

std::vector<CloneSource> list_clone_sources(const fs::path& root) {
    std::vector<CloneSource> sources;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        const std::string id = e.path().filename().string();
        if (id.find(':') != std::string::npos)
            continue;
        CloneSource s;
        s.id = id;
        if (!hex16(line_of(e.path() / "idVendor"), s.vendor_id) ||
            !hex16(line_of(e.path() / "idProduct"), s.product_id))
            continue;
        for (const fs::path& dir : interface_dirs(root, id))
            if (is_hid(dir))
                ++s.hid_interfaces;
        if (s.hid_interfaces == 0)
            continue;
        s.manufacturer = line_of(e.path() / "manufacturer");
        s.product = line_of(e.path() / "product");
        sources.push_back(std::move(s));
    }
    std::sort(sources.begin(), sources.end(),
              [](const CloneSource& a, const CloneSource& b) { return a.id < b.id; });
    return sources;
}

std::string read_clone_device(const CloneSource& source, const fs::path& root, CloneDevice& out) {
    out = CloneDevice{};
    const fs::path dev = root / source.id;
    std::vector<uint8_t> d;
    if (!slurp(dev / "descriptors", d) || d.size() < 27)
        return "cannot read the descriptors of " + source.id;

    out.vendor_id = source.vendor_id;
    out.product_id = source.product_id;
    out.manufacturer = source.manufacturer;
    out.product = source.product;
    out.serial = line_of(dev / "serial");
    const bool high_speed = std::strtol(line_of(dev / "speed").c_str(), nullptr, 10) >= 480;

    // What the descriptors say of each interface of the first configuration.
    struct Found {
        CloneInterface itf;
        bool hid{};
        bool hid_descriptor_seen{};
        bool has_in{};
    };
    std::vector<Found> found;
    bool configuration_seen = false;
    bool alternate = false;
    for (size_t at = 0; at + 2 <= d.size();) {
        const size_t length = d[at];
        if (length < 2 || at + length > d.size())
            break;
        const uint8_t type = d[at + 1];
        const uint8_t* p = d.data() + at;
        if (type == 0x01 && length >= 18) {
            out.bcd_device = static_cast<uint16_t>(p[12] | (p[13] << 8));
            const unsigned bcd_usb = static_cast<unsigned>(p[2] | (p[3] << 8));
            if (bcd_usb != 0x0200)
                out.notes.push_back("the device is USB " + std::to_string(bcd_usb >> 8) + "." +
                                    std::to_string((bcd_usb >> 4) & 0xF) +
                                    "; the board announces 2.0");
            if (p[7] != 64)
                out.notes.push_back("the device's endpoint 0 holds " + std::to_string(p[7]) +
                                    " bytes; the board's holds 64");
        } else if (type == 0x02 && length >= 9) {
            if (configuration_seen)
                break;
            configuration_seen = true;
            out.self_powered = (p[7] & 0x40) != 0;
            out.remote_wakeup = (p[7] & 0x20) != 0;
            out.max_power_ma = static_cast<uint32_t>(p[8]) * 2;
        } else if (type == 0x04 && length >= 9 && configuration_seen) {
            alternate = p[3] != 0;
            if (!alternate) {
                Found f;
                f.itf.number = p[2];
                f.itf.boot_subclass = p[6];
                f.itf.boot_protocol = p[7];
                f.hid = p[5] == 0x03;
                if (!f.hid)
                    out.notes.push_back("interface " + number_of(f.itf) +
                                        " is not a HID interface and is left out");
                found.push_back(std::move(f));
            }
        } else if (!found.empty() && !alternate && found.back().hid) {
            Found& f = found.back();
            if (type == 0x21 && length >= 9 && !f.hid_descriptor_seen) {
                f.itf.bcd_hid = static_cast<uint16_t>(p[2] | (p[3] << 8));
                f.itf.country_code = p[4];
                f.hid_descriptor_seen = true;
            } else if (type == 0x05 && length >= 7 && (p[3] & 3) == 3) {
                const uint32_t size = static_cast<uint32_t>(p[4] | (p[5] << 8)) & 0x7FF;
                uint32_t interval = p[6];
                if (high_speed && interval > 0)
                    interval = std::max<uint32_t>(
                        1, ((1u << std::min<uint32_t>(interval - 1, 15)) + 4) / 8);
                if ((p[2] & 0x80) != 0 && !f.has_in) {
                    f.has_in = true;
                    f.itf.ep_in_size = size;
                    f.itf.interval_ms = std::max<uint32_t>(interval, 1);
                } else if ((p[2] & 0x80) == 0 && f.itf.ep_out_size == 0) {
                    f.itf.ep_out_size = std::min(size, board_endpoint_bytes);
                }
            }
        }
        at += length;
    }

    const std::vector<fs::path> dirs = interface_dirs(root, source.id);
    for (Found& f : found) {
        CloneInterface& itf = f.itf;
        if (!f.hid)
            continue;
        const std::string name = "interface " + number_of(itf);
        if (!f.has_in) {
            out.notes.push_back(name + " has no IN endpoint and is left out");
            continue;
        }
        fs::path dir;
        for (const fs::path& candidate : dirs)
            if (candidate.filename().string().ends_with("." + number_of(itf)))
                dir = candidate;
        if (dir.empty() || !report_descriptor_of(dir, itf.descriptor)) {
            out.notes.push_back(name + ": the report descriptor cannot be read; it is left out");
            continue;
        }
        HidDescriptor parsed;
        if (const std::string e = parse_descriptor(itf.descriptor, parsed); !e.empty()) {
            out.notes.push_back(name + ": " + e + "; it is left out");
            continue;
        }

        itf.name = line_of(dir / "interface");
        if (!printable(itf.name)) {
            out.notes.push_back(name + ": its name is not printable ASCII of at most 31 "
                                       "characters and is left out");
            itf.name.clear();
        }
        if (itf.ep_in_size > board_endpoint_bytes) {
            out.notes.push_back(name + ": the endpoint of " + std::to_string(itf.ep_in_size) +
                                " bytes becomes " + std::to_string(board_endpoint_bytes));
            itf.ep_in_size = board_endpoint_bytes;
        }
        bool vendor = !parsed.collections.empty();
        for (const HidCollection& c : parsed.collections)
            vendor = vendor && c.usage_page >= 0xFF00;
        if (vendor)
            out.notes.push_back(name + " is vendor defined: it is presented, but nothing answers it");
        out.interfaces.push_back(std::move(itf));
    }
    if (out.interfaces.empty())
        return source.id + " has no HID interface that can be read";
    return {};
}

} // namespace hids
