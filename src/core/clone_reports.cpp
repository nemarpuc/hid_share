// SPDX-License-Identifier: MIT
#include "hid_share/clone.hpp"

#include <algorithm>
#include <set>

namespace hids {

namespace {

constexpr uint32_t usage_x = 0x0001'0030;
constexpr uint32_t usage_y = 0x0001'0031;
constexpr uint32_t usage_wheel = 0x0001'0038;
constexpr uint32_t usage_pan = 0x000C'0238;

bool same_meaning(const HidField& a, const HidField& b) {
    return a.report_id == b.report_id && a.feature == b.feature && a.constant == b.constant &&
           a.variable == b.variable && a.bit_size == b.bit_size && a.count == b.count &&
           a.usage_page == b.usage_page && a.usages == b.usages && a.ranged == b.ranged &&
           a.usage_min == b.usage_min && a.usage_max == b.usage_max &&
           a.logical_min == b.logical_min && a.logical_max == b.logical_max;
}

// Writes the low `size` bits of `value` at bit `offset` of report data starting at `base`.
void put_bits(std::vector<uint8_t>& report, const size_t base, const uint32_t offset,
              const uint32_t size, const uint32_t value) {
    for (uint32_t b = 0; b < size && b < 32; ++b) {
        if (((value >> b) & 1U) == 0)
            continue;
        const size_t bit = offset + b;
        const size_t at = base + bit / 8;
        if (at < report.size())
            report[at] = static_cast<uint8_t>(report[at] | (1U << (bit % 8)));
    }
}

uint32_t bits_of_report(const HidDescriptor& d, const uint8_t id, const bool feature) {
    for (const HidReportSize& r : d.reports)
        if (r.report_id == id && r.feature == feature)
            return r.bits;
    return 0;
}

// A blank report: the Report ID byte when the interface has them, then the data.
std::vector<uint8_t> blank_report(const CloneReports::Node& n, const uint8_t id,
                                  const bool feature) {
    const size_t data = (bits_of_report(n.parsed, id, feature) + 7) / 8;
    std::vector<uint8_t> r(data + (n.has_ids && !feature ? 1 : 0), 0);
    if (n.has_ids && !feature)
        r[0] = id;
    return r;
}

size_t data_base(const CloneReports::Node& n, const bool feature) {
    return n.has_ids && !feature ? 1 : 0;
}

std::set<uint8_t> input_ids(const HidCollection& c) {
    std::set<uint8_t> ids;
    for (const HidField& f : c.fields)
        if (!f.feature && !f.constant)
            ids.insert(f.report_id);
    return ids;
}

uint32_t field_mask(const uint32_t size) { return size >= 32 ? 0xFFFF'FFFFu : (1u << size) - 1; }

// How many keys a keyboard collection can report at once: each bit of a bitmap or list of
// usages, each slot of an array.
uint32_t key_capacity(const HidCollection& c) {
    uint32_t total = 0;
    for (const HidField& f : c.fields)
        if (!f.feature && !f.constant)
            total += f.count;
    return total;
}

} // namespace

std::string present_descriptor(const std::span<const uint8_t> original, const HidDescriptor& parsed,
                               const std::vector<bool>& keep, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<size_t> kept;
    for (size_t i = 0; i < parsed.collections.size(); ++i) {
        if (i < keep.size() && keep[i]) {
            const HidCollection& c = parsed.collections[i];
            out.insert(out.end(), original.begin() + static_cast<std::ptrdiff_t>(c.first_byte),
                       original.begin() + static_cast<std::ptrdiff_t>(c.last_byte));
            kept.push_back(i);
        }
    }
    if (kept.empty())
        return {};

    HidDescriptor again;
    if (const std::string e = parse_descriptor(out, again); !e.empty()) {
        out.clear();
        return e;
    }
    bool same = again.collections.size() == kept.size();
    for (size_t k = 0; same && k < kept.size(); ++k) {
        const auto& a = parsed.collections[kept[k]];
        const auto& b = again.collections[k];
        same = a.usage_page == b.usage_page && a.usage == b.usage &&
               a.fields.size() == b.fields.size();
        for (size_t f = 0; same && f < a.fields.size(); ++f)
            same = same_meaning(a.fields[f], b.fields[f]);
    }
    if (!same) {
        out.clear();
        return "a collection that is kept relies on an item set inside one that is dropped";
    }
    return {};
}

std::string CloneReports::build(const CloneDevice& device, const CloneRoles roles,
                                CloneReports& out) {
    out = CloneReports{};
    uint32_t best_keys = 0;
    for (size_t i = 0; i < device.interfaces.size(); ++i) {
        const CloneInterface& src = device.interfaces[i];
        const std::string where = "interface " + std::to_string(src.number);
        HidDescriptor full;
        if (const std::string e = parse_descriptor(src.descriptor, full); !e.empty())
            return where + ": " + e;

        std::vector<bool> keep;
        for (const HidCollection& c : full.collections) {
            bool on = true;
            if (is_keyboard(c))
                on = roles.keyboard;
            else if (is_mouse(c))
                on = roles.mouse;
            else if (is_consumer(c))
                on = roles.media;
            keep.push_back(on);
        }
        Node node;
        node.source = i;
        if (const std::string e = present_descriptor(src.descriptor, full, keep, node.descriptor);
            !e.empty())
            return where + ": " + e;
        if (node.descriptor.empty())
            continue;
        if (const std::string e = parse_descriptor(node.descriptor, node.parsed); !e.empty())
            return where + ": " + e;
        for (const HidReportSize& r : node.parsed.reports)
            node.has_ids = node.has_ids || r.report_id != 0;

        const size_t index = out.nodes_.size();
        for (size_t c = 0; c < node.parsed.collections.size(); ++c) {
            const HidCollection& col = node.parsed.collections[c];
            if (is_keyboard(col)) {
                const uint32_t keys = key_capacity(col);
                if (!out.keyboard_.valid() || keys > best_keys) {
                    out.keyboard_ = {index, c};
                    best_keys = keys;
                }
            } else if (is_mouse(col) && !out.mouse_.valid()) {
                out.mouse_ = {index, c};
            } else if (is_consumer(col) && !out.consumer_.valid()) {
                out.consumer_ = {index, c};
            }
        }
        if (!out.battery_.valid())
            if (const auto b = find_battery(node.parsed)) {
                out.battery_ = {index, b->collection};
                out.battery_field_ = b->field;
                out.battery_feature_ = node.parsed.collections[b->collection].fields[b->field].feature;
            }
        out.nodes_.push_back(std::move(node));
    }
    return {};
}

std::vector<CloneOut> CloneReports::keyboard_reports() const {
    std::vector<CloneOut> out;
    if (!keyboard_.valid())
        return out;
    const Node& node = nodes_[keyboard_.node];
    const HidCollection& col = node.parsed.collections[keyboard_.collection];

    std::set<uint32_t> in_bits; // keys a bitmap or a list of usages carries
    for (const HidField& f : col.fields)
        if (!f.feature && !f.constant && f.variable)
            for (uint32_t i = 0; i < f.count; ++i)
                if ((f.usage_at(i) >> 16) == usage_page_keyboard)
                    in_bits.insert(f.usage_at(i) & 0xFFFF);
    std::vector<uint8_t> listed;
    for (const uint8_t k : pressed_keys_)
        if (!in_bits.contains(k))
            listed.push_back(k);

    for (const uint8_t id : input_ids(col)) {
        CloneOut o;
        o.node = keyboard_.node;
        o.report_id = id;
        o.bytes = blank_report(node, id, false);
        const size_t base = data_base(node, false);
        for (const HidField& f : col.fields) {
            if (f.feature || f.constant || f.report_id != id)
                continue;
            if (f.variable) {
                for (uint32_t i = 0; i < f.count; ++i)
                    if ((f.usage_at(i) >> 16) == usage_page_keyboard &&
                        std::find(pressed_keys_.begin(), pressed_keys_.end(),
                                  static_cast<uint8_t>(f.usage_at(i) & 0xFF)) !=
                            pressed_keys_.end())
                        put_bits(o.bytes, base, f.bit_offset + i * f.bit_size, f.bit_size, 1);
            } else {
                for (uint32_t j = 0; j < f.count && j < listed.size(); ++j)
                    put_bits(o.bytes, base, f.bit_offset + j * f.bit_size, f.bit_size, listed[j]);
            }
        }
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<CloneOut> CloneReports::key(const uint8_t usage, const bool down) {
    if (!keyboard_.valid())
        return {};
    const auto it = std::find(pressed_keys_.begin(), pressed_keys_.end(), usage);
    if (down && it == pressed_keys_.end())
        pressed_keys_.push_back(usage);
    else if (!down && it != pressed_keys_.end())
        pressed_keys_.erase(it);
    return keyboard_reports();
}

std::vector<CloneOut> CloneReports::consumer_reports() const {
    std::vector<CloneOut> out;
    const Node& node = nodes_[consumer_.node];
    const HidCollection& col = node.parsed.collections[consumer_.collection];
    for (const uint8_t id : input_ids(col)) {
        CloneOut o;
        o.node = consumer_.node;
        o.report_id = id;
        o.bytes = blank_report(node, id, false);
        const size_t base = data_base(node, false);
        size_t slot = 0;
        for (const HidField& f : col.fields) {
            if (f.feature || f.constant || f.report_id != id)
                continue;
            if (f.variable) {
                for (uint32_t i = 0; i < f.count; ++i)
                    if (std::find(pressed_media_.begin(), pressed_media_.end(),
                                  static_cast<uint16_t>(f.usage_at(i) & 0xFFFF)) !=
                        pressed_media_.end())
                        put_bits(o.bytes, base, f.bit_offset + i * f.bit_size, f.bit_size, 1);
            } else {
                for (uint32_t j = 0; j < f.count && slot < pressed_media_.size(); ++j, ++slot)
                    put_bits(o.bytes, base, f.bit_offset + j * f.bit_size, f.bit_size,
                             pressed_media_[slot]);
            }
        }
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<CloneOut> CloneReports::consumer(const uint16_t usage, const bool down) {
    if (!consumer_.valid())
        return {};
    const HidCollection& col = nodes_[consumer_.node].parsed.collections[consumer_.collection];
    bool carried = false;
    for (const HidField& f : col.fields) {
        if (f.feature || f.constant)
            continue;
        if (f.variable) {
            for (uint32_t i = 0; i < f.count; ++i)
                carried = carried || (f.usage_at(i) & 0xFFFF) == usage;
        } else {
            carried = carried || (usage >= f.logical_min && usage <= f.logical_max);
        }
    }
    if (!carried)
        return {};
    const auto it = std::find(pressed_media_.begin(), pressed_media_.end(), usage);
    if (down && it == pressed_media_.end())
        pressed_media_.push_back(usage);
    else if (!down && it != pressed_media_.end())
        pressed_media_.erase(it);
    return consumer_reports();
}

std::vector<CloneOut> CloneReports::mouse(int dx, int dy, int wheel, int pan,
                                          const uint32_t buttons) {
    std::vector<CloneOut> out;
    if (!mouse_.valid())
        return out;
    const Node& node = nodes_[mouse_.node];
    const HidCollection& col = node.parsed.collections[mouse_.collection];

    // The reach of each axis in one report; an axis the device lacks (or one that cannot
    // move) is dropped.
    struct Axis {
        uint32_t usage;
        int* amount;
        int lo{0};
        int hi{0};
    };
    Axis axes[] = {{usage_x, &dx}, {usage_y, &dy}, {usage_wheel, &wheel}, {usage_pan, &pan}};
    for (const uint8_t id : input_ids(col)) {
        for (Axis& a : axes) {
            a.lo = 0;
            a.hi = 0;
            for (const HidField& f : col.fields)
                if (!f.feature && !f.constant && f.variable && f.report_id == id)
                    for (uint32_t i = 0; i < f.count; ++i)
                        if (f.usage_at(i) == a.usage) {
                            a.lo = f.logical_min;
                            a.hi = f.logical_max;
                        }
            if (a.hi <= 0 || a.lo >= 0)
                *a.amount = 0; // cannot move both ways: no such axis here
        }
    }

    for (unsigned guard = 0; guard < 100000; ++guard) {
        for (const uint8_t id : input_ids(col)) {
            CloneOut o;
            o.node = mouse_.node;
            o.report_id = id;
            o.bytes = blank_report(node, id, false);
            const size_t base = data_base(node, false);
            int step[4] = {};
            for (size_t k = 0; k < 4; ++k)
                step[k] = std::clamp(*axes[k].amount, axes[k].lo, axes[k].hi);
            for (const HidField& f : col.fields) {
                if (f.feature || f.constant || f.report_id != id || !f.variable)
                    continue;
                for (uint32_t i = 0; i < f.count; ++i) {
                    const uint32_t u = f.usage_at(i);
                    uint32_t value = 0;
                    bool set = false;
                    if ((u >> 16) == usage_page_button) {
                        const uint32_t n = (u & 0xFFFF);
                        set = n >= 1 && n <= 32;
                        value = set ? (buttons >> (n - 1)) & 1U : 0;
                    } else {
                        for (size_t k = 0; k < 4; ++k)
                            if (axes[k].usage == u) {
                                set = true;
                                value = static_cast<uint32_t>(step[k]) & field_mask(f.bit_size);
                            }
                    }
                    if (set)
                        put_bits(o.bytes, base, f.bit_offset + i * f.bit_size, f.bit_size, value);
                }
            }
            out.push_back(std::move(o));
        }
        bool more = false;
        for (size_t k = 0; k < 4; ++k) {
            *axes[k].amount -= std::clamp(*axes[k].amount, axes[k].lo, axes[k].hi);
            more = more || *axes[k].amount != 0;
        }
        if (!more)
            break;
    }
    return out;
}

std::vector<CloneOut> CloneReports::neutral() {
    pressed_keys_.clear();
    pressed_media_.clear();
    std::vector<CloneOut> out;
    if (keyboard_.valid())
        out = keyboard_reports();
    if (consumer_.valid())
        for (CloneOut& o : consumer_reports())
            out.push_back(std::move(o));
    if (mouse_.valid())
        for (CloneOut& o : mouse(0, 0, 0, 0, 0))
            out.push_back(std::move(o));
    return out;
}

std::optional<CloneOut> CloneReports::battery(const int percent) const {
    if (!battery_.valid())
        return std::nullopt;
    const Node& node = nodes_[battery_.node];
    const HidField& f = node.parsed.collections[battery_.collection].fields[battery_field_];
    const int p = std::clamp(percent, 0, 100);
    const int64_t span = static_cast<int64_t>(f.logical_max) - f.logical_min;
    const auto value = static_cast<uint32_t>(f.logical_min + (p * span + 50) / 100) &
                       field_mask(f.bit_size);
    CloneOut o;
    o.node = battery_.node;
    o.report_id = f.report_id;
    o.feature = f.feature;
    o.bytes = blank_report(node, f.report_id, f.feature);
    put_bits(o.bytes, data_base(node, f.feature), f.bit_offset, f.bit_size, value);
    return o;
}

} // namespace hids
