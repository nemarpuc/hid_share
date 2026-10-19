// SPDX-License-Identifier: MIT
#include "hid_share/clone.hpp"

#include <algorithm>

namespace hids {

namespace {

constexpr uint8_t type_main = 0;
constexpr uint8_t type_global = 1;
constexpr uint8_t type_local = 2;

struct Globals {
    uint32_t usage_page{};
    int32_t logical_min{};
    int32_t logical_max_signed{};
    uint32_t logical_max_unsigned{};
    uint32_t report_size{};
    uint32_t report_count{};
    uint8_t report_id{};
};

struct Locals {
    std::vector<uint32_t> usages;
    bool ranged{};
    uint32_t usage_min{};
    uint32_t usage_max{};
};

uint32_t read_unsigned(std::span<const uint8_t> bytes) {
    uint32_t v = 0;
    for (size_t i = 0; i < bytes.size(); ++i)
        v |= static_cast<uint32_t>(bytes[i]) << (8 * i);
    return v;
}

int32_t read_signed(std::span<const uint8_t> bytes) {
    const uint32_t v = read_unsigned(bytes);
    if (bytes.empty() || bytes.size() >= 4)
        return static_cast<int32_t>(v);
    const uint32_t sign = 1u << (8 * bytes.size() - 1);
    return static_cast<int32_t>((v ^ sign) - sign);
}

// A usage item of 4 bytes names its own page; shorter ones take the current page.
uint32_t full_usage(std::span<const uint8_t> bytes, const uint32_t page) {
    const uint32_t v = read_unsigned(bytes);
    return bytes.size() == 4 ? v : (page << 16) | v;
}

uint32_t& bits_of(std::vector<HidReportSize>& reports, const uint8_t id, const bool feature) {
    for (HidReportSize& r : reports)
        if (r.report_id == id && r.feature == feature)
            return r.bits;
    reports.push_back({id, feature, 0});
    return reports.back().bits;
}

} // namespace

uint32_t HidField::usage_at(const uint32_t i) const {
    if (ranged)
        return std::min(usage_min + i, usage_max);
    if (usages.empty())
        return 0;
    return usages[std::min<size_t>(i, usages.size() - 1)];
}

std::string parse_descriptor(const std::span<const uint8_t> bytes, HidDescriptor& out) {
    out = HidDescriptor{};
    Globals g;
    std::vector<Globals> stack;
    Locals l;
    unsigned depth = 0;
    size_t previous_end = 0;
    size_t pos = 0;

    while (pos < bytes.size()) {
        const uint8_t prefix = bytes[pos];
        const size_t item_start = pos;
        if (prefix == 0xFE)
            return "a long item at byte " + std::to_string(pos) + " is not supported";
        const size_t size = (prefix & 3) == 3 ? 4 : (prefix & 3);
        if (pos + 1 + size > bytes.size())
            return "an item at byte " + std::to_string(pos) + " is cut off";
        const auto data = bytes.subspan(pos + 1, size);
        pos += 1 + size;
        const uint8_t type = (prefix >> 2) & 3;
        const uint8_t tag = prefix >> 4;

        if (type == type_global) {
            switch (tag) {
            case 0x0: g.usage_page = read_unsigned(data); break;
            case 0x1: g.logical_min = read_signed(data); break;
            case 0x2:
                g.logical_max_signed = read_signed(data);
                g.logical_max_unsigned = read_unsigned(data);
                break;
            case 0x7: g.report_size = read_unsigned(data); break;
            case 0x8: g.report_id = static_cast<uint8_t>(read_unsigned(data)); break;
            case 0x9: g.report_count = read_unsigned(data); break;
            case 0xA: stack.push_back(g); break;
            case 0xB:
                if (stack.empty())
                    return "a Pop at byte " + std::to_string(item_start) + " has no Push";
                g = stack.back();
                stack.pop_back();
                break;
            default: break;
            }
        } else if (type == type_local) {
            switch (tag) {
            case 0x0: l.usages.push_back(full_usage(data, g.usage_page)); break;
            case 0x1:
                l.ranged = true;
                l.usage_min = full_usage(data, g.usage_page);
                break;
            case 0x2: l.usage_max = full_usage(data, g.usage_page); break;
            default: break;
            }
        } else if (type == type_main) {
            if (tag == 0xA) {
                if (depth == 0) {
                    HidCollection c;
                    c.first_byte = previous_end;
                    const uint32_t u = l.usages.empty() ? 0 : l.usages.front();
                    c.usage_page = u >> 16;
                    c.usage = u & 0xFFFF;
                    out.collections.push_back(std::move(c));
                }
                ++depth;
            } else if (tag == 0xC) {
                if (depth == 0)
                    return "an End Collection at byte " + std::to_string(item_start) +
                           " has no Collection";
                if (--depth == 0) {
                    out.collections.back().last_byte = pos;
                    previous_end = pos;
                }
            } else if (tag == 0x8 || tag == 0xB) {
                if (depth == 0)
                    return "an Input or Feature item at byte " + std::to_string(item_start) +
                           " is outside any collection";
                const bool feature = tag == 0xB;
                const uint32_t flags = read_unsigned(data);
                HidField f;
                f.report_id = g.report_id;
                f.feature = feature;
                f.constant = (flags & 1) != 0;
                f.variable = (flags & 2) != 0;
                f.bit_size = g.report_size;
                f.count = g.report_count;
                f.usage_page = g.usage_page;
                f.usages = l.usages;
                f.ranged = l.ranged;
                f.usage_min = l.usage_min;
                f.usage_max = l.usage_max;
                f.logical_min = g.logical_min;
                f.logical_max = g.logical_min >= 0 ? static_cast<int32_t>(g.logical_max_unsigned)
                                                   : g.logical_max_signed;
                uint32_t& bits = bits_of(out.reports, g.report_id, feature);
                f.bit_offset = bits;
                bits += g.report_size * g.report_count;
                out.collections.back().fields.push_back(std::move(f));
            }
            l = Locals{};
        }
    }
    if (depth != 0)
        return "a collection is not closed";
    return {};
}

bool is_keyboard(const HidCollection& c) noexcept {
    return c.usage_page == usage_page_generic_desktop && c.usage == 0x06;
}

bool is_mouse(const HidCollection& c) noexcept {
    return c.usage_page == usage_page_generic_desktop && c.usage == 0x02;
}

bool is_consumer(const HidCollection& c) noexcept {
    return c.usage_page == usage_page_consumer && c.usage == 0x01;
}

std::optional<BatteryAt> find_battery(const HidDescriptor& d) noexcept {
    constexpr uint32_t battery_strength = (usage_page_generic_device << 16) | 0x20;
    for (size_t c = 0; c < d.collections.size(); ++c) {
        const auto& fields = d.collections[c].fields;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].constant)
                continue;
            for (uint32_t k = 0; k < fields[i].count; ++k)
                if (fields[i].usage_at(k) == battery_strength)
                    return BatteryAt{c, i};
        }
    }
    return std::nullopt;
}

} // namespace hids
