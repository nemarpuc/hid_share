// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "third_party/doctest.h"

#include "hid_share/clone.hpp"
#include "hid_share/config.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace hids;

namespace {

// The five report descriptors of a real keyboard (SteelSeries Apex Pro TKL Gen 3), as
// usbhid-dump printed them, by interface number.
const std::vector<uint8_t> if0 = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01, 0x05, 0x07,
    0x19, 0x00, 0x2A, 0xFF, 0x00, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81,
    0x00, 0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x91, 0x02,
    0x95, 0x05, 0x91, 0x01, 0xC0};
const std::vector<uint8_t> if1 = {
    0x06, 0xC0, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x06, 0xC1, 0xFF, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x09, 0xF0, 0x95, 0x40, 0x81, 0x02, 0x09, 0xF1, 0x95, 0x40, 0x91,
    0x02, 0x09, 0xF2, 0x96, 0x84, 0x02, 0xB1, 0x02, 0xC0};
const std::vector<uint8_t> if2 = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x96, 0x00, 0x01, 0x81, 0x02, 0x05, 0x08, 0x19, 0x01, 0x29, 0x03,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x91, 0x02, 0x95, 0x05, 0x91, 0x01, 0xC0};
const std::vector<uint8_t> if3 = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x0C, 0x19, 0x00, 0x2A, 0xFF, 0x0F,
    0x15, 0x00, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09,
    0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0xA1, 0x02, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x05, 0x01, 0x09,
    0x30, 0x09, 0x31, 0x15, 0x80, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38,
    0x15, 0x80, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xA1, 0x02, 0x05, 0x0C,
    0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
    0xC0};
const std::vector<uint8_t> if4 = {0x06, 0xC1, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x09, 0xF0, 0x15, 0x00,
                                  0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02, 0xC0};

HidDescriptor parsed(const std::vector<uint8_t>& bytes) {
    HidDescriptor d;
    const std::string error = parse_descriptor(bytes, d);
    REQUIRE_MESSAGE(error.empty(), error);
    return d;
}

uint32_t input_bits(const HidDescriptor& d, const uint8_t id) {
    for (const HidReportSize& r : d.reports)
        if (r.report_id == id && !r.feature)
            return r.bits;
    return 0;
}

} // namespace

TEST_CASE("boot keyboard: modifiers, padding and a six key array") {
    const HidDescriptor d = parsed(if0);
    REQUIRE(d.collections.size() == 1);
    CHECK(is_keyboard(d.collections[0]));
    CHECK(d.collections[0].first_byte == 0);
    CHECK(d.collections[0].last_byte == if0.size());
    const auto& f = d.collections[0].fields;
    REQUIRE(f.size() == 3);
    CHECK(f[0].variable);
    CHECK(f[0].bit_offset == 0);
    CHECK(f[0].bit_size == 1);
    CHECK(f[0].count == 8);
    CHECK(f[0].usage_at(0) == 0x0007'00E0);
    CHECK(f[0].usage_at(7) == 0x0007'00E7);
    CHECK(f[1].constant);
    CHECK(f[2].bit_offset == 16);
    CHECK_FALSE(f[2].variable);
    CHECK(f[2].count == 6);
    CHECK(f[2].bit_size == 8);
    CHECK(f[2].logical_max == 255);
    CHECK(input_bits(d, 0) == 64);
}

TEST_CASE("NKRO keyboard: a 256 bit bitmap") {
    const HidDescriptor d = parsed(if2);
    REQUIRE(d.collections.size() == 1);
    CHECK(is_keyboard(d.collections[0]));
    const auto& f = d.collections[0].fields;
    REQUIRE(f.size() == 1);
    CHECK(f[0].variable);
    CHECK(f[0].count == 256);
    CHECK(f[0].bit_size == 1);
    CHECK(f[0].usage_at(4) == 0x0007'0004);
    CHECK(input_bits(d, 0) == 256);
}

TEST_CASE("consumer with Report ID 1 and a mouse with Report ID 2 share an interface") {
    const HidDescriptor d = parsed(if3);
    REQUIRE(d.collections.size() == 2);
    CHECK(is_consumer(d.collections[0]));
    CHECK(is_mouse(d.collections[1]));
    CHECK(d.collections[0].first_byte == 0);
    CHECK(d.collections[0].last_byte == 27);
    CHECK(d.collections[1].first_byte == 27);
    CHECK(d.collections[1].last_byte == if3.size());

    const auto& c = d.collections[0].fields;
    REQUIRE(c.size() == 1);
    CHECK(c[0].report_id == 1);
    CHECK(c[0].count == 2);
    CHECK(c[0].bit_size == 16);
    CHECK(c[0].usage_at(0) == 0x000C'0000);

    const auto& m = d.collections[1].fields;
    REQUIRE(m.size() == 4);
    CHECK(m[0].report_id == 2);
    CHECK(m[0].count == 8);
    CHECK(m[0].usage_at(0) == 0x0009'0001);
    CHECK(m[1].bit_offset == 8);
    CHECK(m[1].usage_at(0) == 0x0001'0030);
    CHECK(m[1].usage_at(1) == 0x0001'0031);
    CHECK(m[1].logical_min == -128);
    CHECK(m[2].bit_offset == 24);
    CHECK(m[2].usage_at(0) == 0x0001'0038);
    CHECK(m[3].bit_offset == 32);
    CHECK(m[3].usage_at(0) == 0x000C'0238);
    CHECK(input_bits(d, 1) == 32);
    CHECK(input_bits(d, 2) == 40);
}

TEST_CASE("vendor interfaces are collections of no role; the feature size is counted") {
    const HidDescriptor v1 = parsed(if1);
    REQUIRE(v1.collections.size() == 1);
    CHECK_FALSE(is_keyboard(v1.collections[0]));
    CHECK_FALSE(is_mouse(v1.collections[0]));
    CHECK_FALSE(is_consumer(v1.collections[0]));
    CHECK(v1.collections[0].usage_page == 0xFFC0);
    CHECK(input_bits(v1, 0) == 512);
    bool feature = false;
    for (const HidReportSize& r : v1.reports)
        if (r.feature) {
            feature = true;
            CHECK(r.bits == 644 * 8);
        }
    CHECK(feature);
    const HidDescriptor v4 = parsed(if4);
    CHECK(v4.collections[0].usage_page == 0xFFC1);
    CHECK(input_bits(v4, 0) == 512);
}

TEST_CASE("no battery in the example; a Battery Strength field is found in Input and Feature") {
    for (const auto* b : {&if0, &if1, &if2, &if3, &if4})
        CHECK_FALSE(find_battery(parsed(*b)).has_value());

    // Generic Device Controls page, Battery Strength, 8 bit, in a Feature item.
    const std::vector<uint8_t> in_feature = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
                                             0x05, 0x06, 0x09, 0x20, 0x15, 0x00, 0x26, 0x64,
                                             0x00, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0};
    const auto f = find_battery(parsed(in_feature));
    REQUIRE(f.has_value());
    CHECK(f->collection == 0);
    CHECK(f->field == 0);
    std::vector<uint8_t> in_input = in_feature;
    in_input[21] = 0x81;
    const HidDescriptor d = parsed(in_input);
    REQUIRE(find_battery(d).has_value());
    CHECK_FALSE(d.collections[0].fields[0].feature);
}

TEST_CASE("a bad descriptor is refused with a reason") {
    HidDescriptor d;
    CHECK_FALSE(parse_descriptor(std::vector<uint8_t>{0x05}, d).empty());       // cut item
    CHECK_FALSE(parse_descriptor(std::vector<uint8_t>{0xC0}, d).empty());       // stray End
    CHECK_FALSE(parse_descriptor(std::vector<uint8_t>{0xA1, 0x01}, d).empty()); // not closed
    CHECK_FALSE(parse_descriptor(std::vector<uint8_t>{0xFE, 0x01, 0x10, 0x00}, d).empty());
    std::vector<uint8_t> cut = if3;
    cut.pop_back();
    CHECK_FALSE(parse_descriptor(cut, d).empty());
}

namespace {

CloneDevice example() {
    CloneDevice d;
    const std::vector<uint8_t>* all[] = {&if0, &if1, &if2, &if3, &if4};
    for (uint8_t i = 0; i < 5; ++i) {
        CloneInterface f;
        f.number = i;
        f.bcd_hid = 0x0111;
        f.ep_in_size = 64;
        f.interval_ms = 1;
        f.descriptor = *all[i];
        d.interfaces.push_back(f);
    }
    return d;
}

CloneReports reports_for(const CloneDevice& d, const CloneRoles roles = {}) {
    CloneReports r;
    const std::string error = CloneReports::build(d, roles, r);
    REQUIRE_MESSAGE(error.empty(), error);
    return r;
}

} // namespace

TEST_CASE("every role is fed where the real device has it") {
    CloneReports r = reports_for(example());
    CHECK(r.nodes().size() == 5);
    CHECK(r.has_keyboard());
    CHECK(r.has_mouse());
    CHECK(r.has_media());
    CHECK_FALSE(r.has_battery());

    // The NKRO interface (2) is the keyboard, not the boot one (0): a key must arrive once.
    auto k = r.key(0x04, true);
    REQUIRE(k.size() == 1);
    CHECK(k[0].node == 2);
    REQUIRE(k[0].bytes.size() == 32);
    CHECK(k[0].bytes[0] == 0x10);
    k = r.key(0xE0, true);
    REQUIRE(k.size() == 1);
    CHECK(k[0].bytes[28] == 0x01);
    CHECK(k[0].bytes[0] == 0x10);
    k = r.key(0x04, false);
    CHECK(k[0].bytes[0] == 0x00);
    CHECK(k[0].bytes[28] == 0x01);

    auto c = r.consumer(0xCD, true);
    REQUIRE(c.size() == 1);
    CHECK(c[0].node == 3);
    CHECK(c[0].bytes == std::vector<uint8_t>{0x01, 0xCD, 0x00, 0x00, 0x00});
    c = r.consumer(0xCD, false);
    CHECK(c[0].bytes == std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00, 0x00});
    CHECK(r.consumer(0x1000, true).empty()); // above the field's range

    auto m = r.mouse(5, -3, 0, 0, 0b101);
    REQUIRE(m.size() == 1);
    CHECK(m[0].node == 3);
    CHECK(m[0].report_id == 2);
    CHECK(m[0].bytes == std::vector<uint8_t>{0x02, 0x05, 0x05, 0xFD, 0x00, 0x00});
    m = r.mouse(300, 0, -2, 1, 0);
    REQUIRE(m.size() == 3);
    CHECK(m[0].bytes == std::vector<uint8_t>{0x02, 0x00, 0x7F, 0x00, 0xFE, 0x01});
    CHECK(m[1].bytes == std::vector<uint8_t>{0x02, 0x00, 0x7F, 0x00, 0x00, 0x00});
    CHECK(m[2].bytes == std::vector<uint8_t>{0x02, 0x00, 0x2E, 0x00, 0x00, 0x00});
}

TEST_CASE("a boot keyboard alone takes keys in its six key array, modifiers in its bits") {
    CloneDevice d = example();
    d.interfaces.erase(d.interfaces.begin() + 2);
    CloneReports r = reports_for(d);
    auto k = r.key(0x04, true);
    k = r.key(0x05, true);
    k = r.key(0xE1, true);
    REQUIRE(k.size() == 1);
    CHECK(k[0].node == 0);
    CHECK(k[0].bytes == std::vector<uint8_t>{0x02, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00});
    k = r.key(0x04, false);
    CHECK(k[0].bytes == std::vector<uint8_t>{0x02, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("a role that is off leaves the target's descriptors without it") {
    CloneReports r = reports_for(example(), {.mouse = true, .keyboard = true, .media = false});
    REQUIRE(r.nodes().size() == 5);
    CHECK(r.nodes()[3].descriptor == std::vector<uint8_t>(if3.begin() + 27, if3.end()));
    CHECK_FALSE(r.has_media());
    CHECK(r.consumer(0xCD, true).empty());
    const auto m = r.mouse(1, 0, 0, 0, 0);
    REQUIRE(m.size() == 1);
    CHECK(m[0].bytes[0] == 0x02);

    r = reports_for(example(), {.mouse = false, .keyboard = true, .media = false});
    CHECK(r.nodes().size() == 4); // interface 3 has nothing left and is not presented
    CHECK(r.mouse(1, 0, 0, 0, 0).empty());

    r = reports_for(example(), {.mouse = true, .keyboard = false, .media = true});
    CHECK(r.nodes().size() == 3); // the vendor interfaces stay, and interface 3
    CHECK(r.key(0x04, true).empty());
    CHECK_FALSE(r.has_keyboard());
}

TEST_CASE("a cut that would change what a kept collection means is refused") {
    // The second collection takes its usage page and sizes from the first.
    const std::vector<uint8_t> d = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x26, 0xFF,
                                    0x0F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x00, 0xC0, 0x09, 0x02,
                                    0xA1, 0x01, 0x81, 0x00, 0xC0};
    const HidDescriptor p = parsed(d);
    REQUIRE(p.collections.size() == 2);
    std::vector<uint8_t> out;
    CHECK_FALSE(present_descriptor(d, p, {false, true}, out).empty());
    CHECK(present_descriptor(d, p, {true, false}, out).empty());
    CHECK(out == std::vector<uint8_t>(d.begin(), d.begin() + 18));
    CHECK(present_descriptor(d, p, {false, false}, out).empty());
    CHECK(out.empty());
}

TEST_CASE("the battery level goes where the device has it") {
    CloneDevice d;
    CloneInterface f;
    f.number = 0;
    f.bcd_hid = 0x0111;
    f.ep_in_size = 8;
    f.interval_ms = 1;
    // Consumer collection with Report ID 3 and a Battery Strength Input field of 0..100.
    f.descriptor = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x05, 0x06, 0x09, 0x20, 0x15,
                    0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0};
    d.interfaces.push_back(f);
    CloneReports r = reports_for(d);
    REQUIRE(r.has_battery());
    auto b = r.battery(50);
    REQUIRE(b.has_value());
    CHECK_FALSE(b->feature);
    CHECK(b->bytes == std::vector<uint8_t>{0x03, 50});

    d.interfaces[0].descriptor[20] = 0xB1;
    r = reports_for(d);
    b = r.battery(100);
    REQUIRE(b.has_value());
    CHECK(b->feature);
    CHECK(b->report_id == 3);
    CHECK(b->bytes == std::vector<uint8_t>{100});

    CHECK_FALSE(reports_for(example()).battery(10).has_value());
}

TEST_CASE("a bad descriptor in a device names its interface") {
    CloneDevice d = example();
    d.interfaces[1].descriptor.pop_back();
    CloneReports r;
    const std::string error = CloneReports::build(d, {}, r);
    CHECK(error.find("interface 1") != std::string::npos);
}

namespace {

CloneSnapshot snapshot() {
    CloneSnapshot s;
    s.source = "Apex Pro TKL Gen 3";
    s.device = example();
    s.device.vendor_id = 0x1038;
    s.device.product_id = 0x1642;
    s.device.bcd_device = 0x0119;
    s.device.manufacturer = "SteelSeries";
    s.device.product = "Apex Pro TKL Gen 3";
    s.device.remote_wakeup = true;
    s.device.max_power_ma = 500;
    s.device.interfaces[1].name = "Config";
    s.device.interfaces[3].ep_in_size = 8;
    s.device.interfaces[0].boot_subclass = 1;
    s.device.interfaces[0].boot_protocol = 1;
    s.device.notes = {"interface 3 endpoint size 6 became 8", "vendor interfaces stay silent"};
    s.battery = 80;
    return s;
}

} // namespace

TEST_CASE("a clone is kept in the device file and read back whole") {
    DeviceConfig d;
    d.serial = "mim-1";
    d.link = LinkKind::mim;
    d.clone = snapshot();
    const std::string text = format_device(d);
    CHECK(text.find("[clone]") != std::string::npos);
    DeviceConfig back;
    const std::string error = parse_device(text, back);
    REQUIRE_MESSAGE(error.empty(), error);
    REQUIRE(back.clone.has_value());
    CHECK(*back.clone == *d.clone);
    CHECK(format_device(back) == text);

    DeviceConfig none;
    none.serial = "mim-2";
    none.link = LinkKind::mim;
    CHECK(format_device(none).find("[clone]") == std::string::npos);
}

TEST_CASE("a clone that cannot be presented is refused when read") {
    DeviceConfig d;
    d.serial = "mim-1";
    d.link = LinkKind::mim;
    d.clone = snapshot();
    const std::string good = format_device(d);
    const auto replaced = [&](const std::string& from, const std::string& to) {
        std::string t = good;
        const size_t at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        DeviceConfig out;
        return parse_device(t, out);
    };
    CHECK_FALSE(replaced("battery = 80", "battery = 101").empty());
    CHECK_FALSE(replaced("vendor_id = 0x1038", "vendor_id = 0").empty());
    CHECK_FALSE(replaced("max_power_ma = 500", "max_power_ma = 600").empty());
    CHECK_FALSE(replaced("interface_0 = ", "interface_9 = ").empty());
    CHECK_FALSE(replaced("interface_0 = 0 ", "interface_0 = 0 5 ").empty()); // a field too many
    CHECK_FALSE(replaced("interface_1 = 1 0 0 0x0111 0 64 0 1 436F6E666967 ",
                         "interface_1 = 1 0 0 0x0111 0 64 0 1 436F6E666967 0").empty()); // odd hex
    CHECK_FALSE(replaced("interface_4 = 4 0 0 0x0111 0 64 0 1 - ", "interface_4 = 4 0 0 0x0111 0 64 0 1 - 05")
                    .empty()); // a cut descriptor

    // A [clone] with no interface at all presents nothing.
    DeviceConfig bare;
    CHECK_FALSE(parse_device("[device]\nserial = mim-1\nlink = mim\n[clone]\nvendor_id = 0x1038\n"
                             "product_id = 0x1642\n",
                             bare)
                    .empty());
}

// sysfs file names carry a colon, which Windows does not allow; the reader is Linux only.
#ifndef _WIN32
namespace {

namespace fs = std::filesystem;

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string bytes_text(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

// The kernel's `descriptors` file of the example keyboard: the device, one configuration
// and its five HID interfaces with an endpoint each.
std::string example_descriptors(const uint16_t last_endpoint = 64) {
    std::vector<uint8_t> d = {0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x38,
                              0x10, 0x42, 0x16, 0x19, 0x01, 0x01, 0x02, 0x00, 0x01,
                              0x09, 0x02, 0x86, 0x00, 0x05, 0x01, 0x00, 0xA0, 0xFA};
    const uint8_t lengths[5] = {65, 37, 43, 106, 21};
    const uint16_t sizes[5] = {8, 64, 32, 6, last_endpoint};
    for (uint8_t i = 0; i < 5; ++i) {
        const std::vector<uint8_t> part = {
            0x09, 0x04, i, 0x00, 0x01, 0x03, static_cast<uint8_t>(i == 0 ? 1 : 0),
            static_cast<uint8_t>(i == 0 ? 1 : 0), 0x00,
            0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, lengths[i], 0x00,
            0x07, 0x05, static_cast<uint8_t>(0x81 + i), 0x03,
            static_cast<uint8_t>(sizes[i] & 0xFF), static_cast<uint8_t>(sizes[i] >> 8), 0x01};
        d.insert(d.end(), part.begin(), part.end());
    }
    return bytes_text(d);
}

fs::path fake_sysfs() {
    const fs::path root = fs::temp_directory_path() / "hid_share_clone_sysfs";
    fs::remove_all(root);
    const fs::path dev = root / "5-2";
    write_file(dev / "idVendor", "1038\n");
    write_file(dev / "idProduct", "1642\n");
    write_file(dev / "manufacturer", "SteelSeries\n");
    write_file(dev / "product", "Apex Pro TKL Gen 3\n");
    write_file(dev / "speed", "12\n");
    write_file(dev / "descriptors", example_descriptors());
    const std::vector<uint8_t>* all[] = {&if0, &if1, &if2, &if3, &if4};
    for (int i = 0; i < 5; ++i) {
        const fs::path itf = root / ("5-2:1." + std::to_string(i));
        write_file(itf / "bInterfaceClass", "03\n");
        write_file(itf / ("0003:1038:1642.000" + std::to_string(i)) / "report_descriptor",
                   bytes_text(*all[i]));
    }
    // A hub, which has no HID interface, and a mouse with one.
    write_file(root / "usb5" / "idVendor", "1d6b\n");
    write_file(root / "usb5" / "idProduct", "0002\n");
    write_file(root / "usb5:1.0" / "bInterfaceClass", "09\n");
    write_file(root / "5-3" / "idVendor", "046d\n");
    write_file(root / "5-3" / "idProduct", "c077\n");
    write_file(root / "5-3:1.0" / "bInterfaceClass", "03\n");
    return root;
}

} // namespace

TEST_CASE("the plugged-in devices with a HID interface are listed") {
    const fs::path root = fake_sysfs();
    const auto list = list_clone_sources(root);
    REQUIRE(list.size() == 2);
    CHECK(list[0].id == "5-2");
    CHECK(list[0].vendor_id == 0x1038);
    CHECK(list[0].product == "Apex Pro TKL Gen 3");
    CHECK(list[0].hid_interfaces == 5);
    CHECK(list[1].id == "5-3");
    CHECK(list[1].hid_interfaces == 1);
    CHECK(list_clone_sources(root / "missing").empty());
    fs::remove_all(root);
}

TEST_CASE("a device is read whole from the kernel's files") {
    const fs::path root = fake_sysfs();
    CloneDevice d;
    const std::string error = read_clone_device(list_clone_sources(root)[0], root, d);
    REQUIRE_MESSAGE(error.empty(), error);
    CHECK(d.vendor_id == 0x1038);
    CHECK(d.product_id == 0x1642);
    CHECK(d.bcd_device == 0x0119);
    CHECK(d.manufacturer == "SteelSeries");
    CHECK(d.product == "Apex Pro TKL Gen 3");
    CHECK(d.serial.empty());
    CHECK(d.remote_wakeup);
    CHECK_FALSE(d.self_powered);
    CHECK(d.max_power_ma == 500);
    REQUIRE(d.interfaces.size() == 5);
    CHECK(d.interfaces[0].boot_subclass == 1);
    CHECK(d.interfaces[0].boot_protocol == 1);
    CHECK(d.interfaces[1].boot_subclass == 0);
    CHECK(d.interfaces[0].bcd_hid == 0x0111);
    CHECK(d.interfaces[0].interval_ms == 1);
    CHECK(d.interfaces[0].descriptor == if0);
    CHECK(d.interfaces[3].descriptor == if3);
    CHECK(d.interfaces[1].ep_in_size == 64);
    CHECK(d.interfaces[2].ep_in_size == 32);
    CHECK(d.interfaces[3].ep_in_size == 6); // taken as it is, not rounded
    CHECK(d.interfaces[4].number == 4);

    const auto noted = [&](const std::string& part) {
        for (const std::string& n : d.notes)
            if (n.find(part) != std::string::npos)
                return true;
        return false;
    };
    CHECK_FALSE(noted("endpoint"));
    CHECK_FALSE(noted("Feature report")); // the board decides what it can answer
    CHECK(noted("interface 1 is vendor defined"));
    CHECK(noted("interface 4 is vendor defined"));
    CHECK_FALSE(noted("interface 0 is vendor"));
    fs::remove_all(root);
}

TEST_CASE("an interface whose report descriptor cannot be read is left out, with a note") {
    const fs::path root = fake_sysfs();
    fs::remove_all(root / "5-2:1.2" / "0003:1038:1642.0002");
    CloneDevice d;
    REQUIRE(read_clone_device(list_clone_sources(root)[0], root, d).empty());
    CHECK(d.interfaces.size() == 4);
    bool said = false;
    for (const std::string& n : d.notes)
        said = said || n.find("interface 2") != std::string::npos;
    CHECK(said);
    CHECK_FALSE(read_clone_device({"nope", 1, 1, "", "", 1}, root, d).empty());
    fs::remove_all(root);
}

#endif

TEST_CASE("letting everything go sends the reports that say so") {
    CloneReports r = reports_for(example());
    static_cast<void>(r.key(0x04, true));
    static_cast<void>(r.consumer(0xCD, true));
    const auto out = r.neutral();
    REQUIRE(out.size() == 3);
    CHECK(out[0].node == 2);
    CHECK(out[0].bytes == std::vector<uint8_t>(32, 0));
    CHECK(out[1].bytes == std::vector<uint8_t>{0x01, 0, 0, 0, 0});
    CHECK(out[2].bytes == std::vector<uint8_t>{0x02, 0, 0, 0, 0, 0});
}

TEST_CASE("with every role off and no vendor interface, nothing is presented") {
    CloneDevice d = example();
    d.interfaces.erase(d.interfaces.begin() + 4);
    d.interfaces.erase(d.interfaces.begin() + 1);
    const CloneReports r = reports_for(d, {.mouse = false, .keyboard = false, .media = false});
    CHECK(r.nodes().empty());
    CHECK_FALSE(r.has_keyboard());
    CHECK_FALSE(r.has_mouse());
    CHECK_FALSE(r.has_media());
}

#ifndef _WIN32
TEST_CASE("an endpoint larger than any board's 1024 bytes is cut to 1024, with a note") {
    const fs::path root = fake_sysfs();
    write_file(root / "5-2" / "descriptors", example_descriptors(1500));
    CloneDevice d;
    REQUIRE(read_clone_device(list_clone_sources(root)[0], root, d).empty());
    CHECK(d.interfaces[4].ep_in_size == 1024);
    bool said = false;
    for (const std::string& n : d.notes)
        said = said || n.find("interface 4: the endpoint of 1500 bytes becomes 1024") != std::string::npos;
    CHECK(said);
    fs::remove_all(root);
}
#endif
