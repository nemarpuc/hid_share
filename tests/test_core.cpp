// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "third_party/doctest.h"

#include "hid_share/accel_model.hpp"
#include "hid_share/config.hpp"
#include "hid_share/geometry.hpp"
#include "hid_share/keymap.hpp"
#include "hid_share/position_cell.hpp"
#include "hid_share/probe.hpp"
#include "hid_share/session.hpp"
#include "hid_share/tracker.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using namespace hids;

namespace {

// Two monitors side by side, the second lower and shorter than the first.
std::vector<Monitor> two_monitors() {
    return {{"A", {0, 0, 1920, 1080}, 527.0, 296.0}, {"B", {1920, 142, 1423, 800}, 600.0, 340.0}};
}

// What Android does with a relative mouse: scale by a gain, clamp to the
// display, in the axes the mouse is shown in.
struct FakeAndroid final : Sink {
    Size logical{};
    Size view{};
    double x{};
    double y{};
    double gain_lo{1.0};
    double gain_hi{1.0};
    std::mt19937 random{7};
    std::vector<uint16_t> keys_down;
    unsigned buttons_down{};
    int jumps{};

    void move(const Delta counts) override {
        const double gain = std::uniform_real_distribution<double>(gain_lo, gain_hi)(random);
        x = std::clamp(x + gain * counts.x, 0.0, logical.w - 1.0);
        y = std::clamp(y + gain * counts.y, 0.0, logical.h - 1.0);
    }
    void jump(const Delta counts) override {
        ++jumps;
        move(counts);
    }
    void scroll(const int wheel, const int pan) override { wheels.emplace_back(wheel, pan); }
    void button(const unsigned button, const bool down) override {
        if (down)
            buttons_down |= 1U << button;
        else
            buttons_down &= ~(1U << button);
    }
    void key(const uint16_t usage, const bool down) override {
        if (down)
            keys_down.push_back(usage);
        else
            std::erase(keys_down, usage);
    }
    void media(uint16_t, bool) override {}
    struct TouchCall {
        unsigned contact;
        int x;
        int y;
        bool down;
    };
    std::vector<TouchCall> touches;
    std::vector<std::pair<int, int>> wheels;
    void touch(const unsigned contact, const int x, const int y, const bool down) override {
        touches.push_back({contact, x, y, down});
    }
    std::vector<SwipePlan> swipes;
    void swipe(const SwipePlan& plan) override { swipes.push_back(plan); }

    [[nodiscard]] double view_x() const { return x; }
    [[nodiscard]] double view_y() const { return y; }
};

bool inside(const Interval& range, const double value) {
    return value >= range.lo - 1e-6 && value <= range.hi + 1e-6;
}

// Distance of the real cursor from the edge it shares with the PC.
double real_edge_distance(const FakeAndroid& phone, const Side side) {
    switch (side) {
    case Side::right:
        return phone.view_x();
    case Side::left:
        return phone.view.w - 1.0 - phone.view_x();
    case Side::bottom:
        return phone.view_y();
    case Side::top:
        return phone.view.h - 1.0 - phone.view_y();
    }
    return 0.0;
}

} // namespace

TEST_CASE("a probe axis totals its presses") {
    ProbeAxis axis;
    axis.press(100);
    axis.press(100);
    axis.press(10);
    CHECK(axis.total() == 210);
    CHECK(axis.last() == 10);
    axis.clear();
    CHECK(axis.total() == 0);
    CHECK(axis.last() == 0);
}

TEST_CASE("finish takes out the press that moved nothing") {
    // The wall is 1909 counts away.
    ProbeAxis axis;
    for (int i = 0; i < 19; ++i)
        axis.press(100);
    for (int i = 0; i < 9; ++i)
        axis.press(1);
    CHECK(axis.total() == 1909);
    // One more single count that moved nothing.
    axis.press(1);
    CHECK(axis.finish(1) == 1909);
}

TEST_CASE("finish is refused unless the latest press is the fine step") {
    ProbeAxis axis;
    axis.press(1);
    CHECK(axis.finish(1) == 0);
    axis.clear();
    axis.press(100);
    axis.press(10);
    CHECK(axis.finish(1) == 0);
    axis.clear();
    axis.press(100);
    axis.press(1);
    CHECK(axis.finish(1) == 100);
}

TEST_CASE("a board that gives no ids announces the test id") {
    DeviceConfig board;
    board.link = LinkKind::mim;
    const MimDevice plain = mim_of(board);
    CHECK(plain.vendor_id == 0x1209);
    CHECK(plain.product_id == 0x0001);
    CHECK(plain.manufacturer == "Generic");
    CHECK(plain.product == "USB Input Device");
    CHECK(uses_test_id(plain));

    // A string given alone leaves the ids at the test id.
    board.mim.manufacturer = "maker";
    CHECK(mim_of(board).manufacturer == "maker");
    CHECK(mim_of(board).vendor_id == 0x1209);

    // The two ids are one unit: one given alone is not completed.
    DeviceConfig half;
    half.mim.vendor_id = 0x046d;
    CHECK(mim_of(half).vendor_id == 0x046d);
    CHECK(mim_of(half).product_id == 0);
    CHECK_FALSE(uses_test_id(mim_of(half)));

    DeviceConfig given;
    given.mim.vendor_id = 0x1234;
    given.mim.product_id = 0x5678;
    given.mim.manufacturer = "m";
    given.mim.product = "p";
    const MimDevice own = mim_of(given);
    CHECK(own.vendor_id == 0x1234);
    CHECK(own.product_id == 0x5678);
    CHECK(own.manufacturer == "m");
    CHECK(own.product == "p");
    CHECK_FALSE(uses_test_id(own));
}

TEST_CASE("language_id is read, written and never zero") {
    DeviceConfig board;
    board.serial = "mim-x";
    board.link = LinkKind::mim;
    board.mim.language_id = 0x0407;
    DeviceConfig back;
    REQUIRE(parse_device(format_device(board), back).empty());
    CHECK(back.mim.language_id == 0x0407);
    DeviceConfig fresh;
    CHECK_FALSE(parse_device("[usb]\nlanguage_id = 0\n", fresh).empty());
    REQUIRE(parse_device("[usb]\nlanguage_id =\n", fresh).empty());
    CHECK(fresh.mim.language_id == 0);
    CHECK(mim_of(fresh).language_id == 0x0409);
    CHECK(mim_of(back).language_id == 0x0407);
}

TEST_CASE("interface numbers keep pinned ones and fill the rest from the lowest") {
    std::array<NodeUsb, usb_nodes> nodes{};
    const uint8_t mouse_touch = 0b1001;
    auto numbers = interface_numbers(nodes, mouse_touch);
    CHECK(numbers[0] == 0);
    CHECK(numbers[3] == 1);
    nodes[3].interface_number = 0;
    numbers = interface_numbers(nodes, mouse_touch);
    CHECK(numbers[3] == 0);
    CHECK(numbers[0] == 1);
}

TEST_CASE("interface numbers are checked against the board's limits") {
    const std::array<uint32_t, 2> two = {0, 1};
    const std::array<uint32_t, 3> three = {0, 1, 2};
    const std::array<uint32_t, 1> far = {5};
    CHECK(interface_limit_error(two, 4, 4).empty());
    const std::string nodes = interface_limit_error(three, 2, 4);
    CHECK(nodes.find('2') != std::string::npos);
    const std::string number = interface_limit_error(far, 4, 4);
    CHECK(number.find('4') != std::string::npos);
    CHECK(number.find('5') != std::string::npos);
}

TEST_CASE("Nodes may share an interface when each has its own Report ID") {
    std::array<NodeUsb, usb_nodes> nodes{};
    const uint8_t mouse_keyboard = 0b0011;
    // Apart, nothing is asked of the Report IDs.
    CHECK(shared_interface_error(nodes, mouse_keyboard).empty());
    nodes[0].interface_number = 0;
    nodes[1].interface_number = 0;
    // Together, both need one.
    CHECK_FALSE(shared_interface_error(nodes, mouse_keyboard).empty());
    nodes[0].report_id = 1;
    CHECK_FALSE(shared_interface_error(nodes, mouse_keyboard).empty());
    nodes[1].report_id = 1;
    CHECK_FALSE(shared_interface_error(nodes, mouse_keyboard).empty());
    nodes[1].report_id = 2;
    CHECK(shared_interface_error(nodes, mouse_keyboard).empty());
    // A Node that is not wanted does not count.
    nodes[1].report_id.reset();
    CHECK(shared_interface_error(nodes, 0b0001).empty());
    // The Nodes that were not pinned go to the lowest numbers left, apart from these.
    const auto numbers = interface_numbers(nodes, 0b0111);
    CHECK(numbers[0] == 0);
    CHECK(numbers[1] == 0);
    CHECK(numbers[2] == 1);
}

TEST_CASE("a Node's USB interface values are read, written and checked") {
    DeviceConfig board;
    board.serial = "mim-x";
    board.link = LinkKind::mim;
    board.mim.node[3].interface_number = 3;
    board.mim.node[3].report_id = 5;
    board.mim.node[3].interface_name = "Pad";
    board.mim.node[3].ep_in_size = 32;
    const std::string text = format_device(board);
    CHECK(text.find("[usb.touch]") != std::string::npos);
    CHECK(text.find("[usb.mouse]") == std::string::npos);
    DeviceConfig back;
    REQUIRE(parse_device(text, back).empty());
    CHECK(back.mim.node[3] == board.mim.node[3]);

    const auto refused = [](const char* section, const char* line) {
        DeviceConfig d;
        return !parse_device(std::string("[") + section + "]\n" + line + "\n", d).empty();
    };
    CHECK(refused("usb.mouse", "poll_interval_ms = 0"));
    CHECK(refused("usb.mouse", "poll_interval_ms = 256"));
    CHECK(refused("usb.mouse", "ep_in_size = 65"));
    CHECK(refused("usb.mouse", "ep_in_size = 0"));
    CHECK_FALSE(refused("usb.mouse", "ep_in_size = 6"));
    CHECK_FALSE(refused("usb.mouse", "ep_out_size = 5"));
    CHECK(refused("usb.mouse", "ep_out_size = 65"));
    CHECK(refused("usb.mouse", "report_id = 0"));
    CHECK(refused("usb.mouse", "report_id = 256"));
    CHECK(refused("usb.mouse", "boot_protocol = 1"));
    CHECK(refused("usb.mouse", "interface_name = 0123456789012345678901234567890x"));
    CHECK(refused("usb.pen", "report_id = 1"));
    CHECK_FALSE(refused("usb.mouse", "ep_out_size = 0"));

    DeviceConfig same;
    REQUIRE(parse_device("[usb.mouse]\nreport_id = 7\n[usb.keyboard]\nreport_id = 7\n", same)
                .empty());
    CHECK(same.mim.node[0].report_id == 7U);
    CHECK(same.mim.node[1].report_id == 7U);
}

TEST_CASE("the touchscreen's HID values are read, written and checked") {
    DeviceConfig device;
    device.serial = "ABC1";
    REQUIRE(parse_device("[touch_hid]\n", device).empty());
    CHECK(device.touch_hid == TouchHid());

    device.touch_hid.maximum_contacts = 5;
    device.touch_hid.pressure_max = 255;
    const std::string text = format_device(device);
    CHECK(text.find("[touch_hid]") != std::string::npos);
    DeviceConfig back;
    REQUIRE(parse_device(text, back).empty());
    CHECK(back.touch_hid == device.touch_hid);

    const auto refused = [](const char* line) {
        DeviceConfig d;
        return !parse_device(std::string("[touch_hid]\n") + line + "\n", d).empty();
    };
    CHECK(refused("maximum_contacts = 0"));
    CHECK(refused("maximum_contacts = 17"));
    CHECK(refused("coordinate_bits = 7"));
    CHECK(refused("contact_id_bits = 9"));
    CHECK(refused("pressure_bits = 8"));
    CHECK(refused("scan_time_unit_100us = 0"));

    // The report carries no more contacts than the screen has.
    Config config;
    config.devices.push_back(DeviceConfig());
    config.devices[0].serial = "ABC1";
    config.devices[0].touch_hid.maximum_contacts = 5;
    config.devices[0].touch_hid.contacts_per_report = 6;
    CHECK_FALSE(validate_config(config).empty());
    config.devices[0].touch_hid.contacts_per_report = 5;
    CHECK(validate_config(config).empty());
}

TEST_CASE("a placement start may be negative, in pixels or as a percent") {
    const auto start_of = [](const char* value, DeviceConfig& d) {
        return parse_device(std::string("[placement]\nstart = ") + value + "\n", d);
    };
    DeviceConfig d;
    CHECK(start_of("-50", d).empty());
    CHECK(start_of("-10%", d).empty());
    REQUIRE(d.start_ratio.has_value());
    CHECK(*d.start_ratio == doctest::Approx(-0.1));
    CHECK(start_of("-100%", d).empty());
    CHECK_FALSE(start_of("-101%", d).empty());
    CHECK_FALSE(start_of("-", d).empty());
    CHECK_FALSE(start_of("--5", d).empty());
}

TEST_CASE("outer spans leave out what the neighbour covers") {
    const auto monitors = two_monitors();
    const auto right = outer_spans(monitors, 0, Side::right);
    REQUIRE(right.size() == 2);
    CHECK(right[0].lo == 0);
    CHECK(right[0].hi == 141);
    CHECK(right[1].lo == 942);
    CHECK(right[1].hi == 1079);

    CHECK(outer_spans(monitors, 1, Side::left).empty());
    const auto far_right = outer_spans(monitors, 1, Side::right);
    REQUIRE(far_right.size() == 1);
    CHECK(far_right[0].lo == 142);
    CHECK(far_right[0].hi == 941);
    CHECK(outer_spans(monitors, 0, Side::top).size() == 1);
    CHECK(outer_spans(monitors, 1, Side::bottom).size() == 1);
}

TEST_CASE("a portal keeps to the outer edge and maps both ways") {
    const auto monitors = two_monitors();
    Portal portal;
    PortalRequest request;
    request.monitor = 1;
    request.side = Side::right;
    request.anchor_length = 500;
    request.android_length = 2560;
    REQUIRE(resolve_portal(monitors, request, portal).empty());
    CHECK(portal.segment.lo >= 142 + 40);
    CHECK(portal.segment.hi <= 941 - 40);
    CHECK(portal.to_android(portal.anchor.lo) == doctest::Approx(0.0));
    CHECK(portal.to_android(portal.anchor.hi) == doctest::Approx(2559.0));
    for (int t = portal.segment.lo; t <= portal.segment.hi; t += 37)
        CHECK(portal.to_pc(portal.to_android(t)) == t);

    // Entirely on the part the second monitor covers: nothing to cross at.
    request.monitor = 0;
    request.centered = false;
    request.anchor_start = 300;
    request.anchor_length = 400;
    CHECK_FALSE(resolve_portal(monitors, request, portal).empty());

    // Reaching past the covered part keeps the longer outer piece.
    request.anchor_start = 800;
    request.anchor_length = 250;
    request.corner_margin = 0.0;
    REQUIRE(resolve_portal(monitors, request, portal).empty());
    CHECK(portal.segment.lo == 942);
    CHECK(portal.segment.hi == 1049);
}

TEST_CASE("the acceleration curve joins up and never falls below its base") {
    // AOSP's constants leave a step of about 0.05% where two segments meet.
    const double edges[] = {32.002, 52.83, 119.124};
    for (const double edge : edges) {
        CHECK(curve_gain(0, edge - 1e-6) ==
              doctest::Approx(curve_gain(0, edge + 1e-6)).epsilon(1e-3));
    }
    CHECK(curve_gain(0, 0.0) == doctest::Approx(0.64 * 3.19));
    double last = 0.0;
    for (double speed = 1.0; speed < 2000.0; speed *= 1.3) {
        const double gain = curve_gain(0, speed);
        CHECK(gain >= last);
        CHECK(gain >= curve_gain(0, 0.0) - 0.002);
        last = gain;
    }
    CHECK(curve_gain(7, 10.0) == doctest::Approx(2.0 * curve_gain(0, 10.0)));

    const AccelModel exact(AccelMode::exact, 0, 0.2, 10.0);
    CHECK(exact.range(100.0, 0.01).lo == 1.0);
    CHECK(exact.floor() == 1.0);
    const AccelModel curve(AccelMode::curve, 0, 0.2, 10.0);
    const GainRange range = curve.range(40.0, 0.008);
    CHECK(range.lo <= range.hi);
    CHECK(range.lo >= curve.floor());
}

TEST_CASE("round trips keep the estimate around the real cursor") {
    const Side sides[] = {Side::left, Side::right, Side::top, Side::bottom};
    const double sensitivities[] = {0.3, 1.0, 2.7};
    std::mt19937 random(11);

    for (const Side side : sides) {
        for (const int variant : {0, 1, 2, 3}) {
            const bool exact = variant < 2;
            const bool park = variant % 2 != 0;
            for (const double sensitivity : sensitivities) {
                CAPTURE(park);
                CAPTURE(static_cast<int>(side));
                CAPTURE(exact);
                CAPTURE(sensitivity);

                FakeAndroid phone;
                phone.view = phone.logical = {1600, 2560};
                phone.x = 123.0;
                phone.y = 456.0;
                if (!exact) {
                    phone.gain_lo = 2.04;
                    phone.gain_hi = 9.6;
                }

                Portal portal;
                portal.side = side;
                portal.anchor = {200, 900};
                portal.segment = {200, 900};
                portal.android_length = vertical(side) ? phone.view.h : phone.view.w;

                MotionConfig motion;
                motion.scale_x = motion.scale_y = sensitivity;
                motion.park = park;
                const AccelModel accel(exact ? AccelMode::exact : AccelMode::unknown, 0,
                                       2.04, 9.6);
                Session session(phone);
                session.configure(portal, phone.view, motion, accel);

                        double now = 1.0;
                        for (int trip = 0; trip < 20; ++trip) {
                            const int t = std::uniform_int_distribution<int>(200, 900)(random);
                            REQUIRE(session.edge_hit(t, 1.0, false, now));
                            REQUIRE(session.remote());
                            REQUIRE(inside(session.tracker().x(), phone.view_x()));
                            REQUIRE(inside(session.tracker().y(), phone.view_y()));
                            if (exact) {
                                const double along =
                                    vertical(side) ? phone.view_y() : phone.view_x();
                                CHECK(std::fabs(along - portal.to_android(t)) <= 0.5 + 1e-9);
                                CHECK(real_edge_distance(phone, side) == 0.0);
                            }

                            // Wander without going back.
                            for (int step = 0; step < 40; ++step) {
                                now += 0.004;
                                const double dx =
                                    std::uniform_int_distribution<int>(-40, 40)(random);
                                const double dy =
                                    std::uniform_int_distribution<int>(-40, 40)(random);
                                if (session.motion(dx, dy, now)) {
                                    // Only a real arrival at the shared edge may end the visit.
                                    REQUIRE(real_edge_distance(phone, side) == 0.0);
                                    break;
                                }
                                REQUIRE(inside(session.tracker().x(), phone.view_x()));
                                REQUIRE(inside(session.tracker().y(), phone.view_y()));
                            }

                            // Push back toward the PC until it lets go.
                            const double push = 30.0;
                            double dx = 0.0;
                            double dy = 0.0;
                            switch (side) {
                            case Side::right:
                                dx = -push;
                                break;
                            case Side::left:
                                dx = push;
                                break;
                            case Side::bottom:
                                dy = -push;
                                break;
                            case Side::top:
                                dy = push;
                                break;
                            }
                            int pushes = 0;
                            while (session.remote()) {
                                now += 0.004;
                                if (session.motion(dx, dy, now)) {
                                    REQUIRE(real_edge_distance(phone, side) == 0.0);
                                    const int back = session.leave();
                                    CHECK(back >= portal.segment.lo);
                                    CHECK(back <= portal.segment.hi);
                                    if (park) {
                                        // Android's own bottom right, whatever the mount.
                                        CHECK(phone.x == phone.logical.w - 1.0);
                                        CHECK(phone.y == phone.logical.h - 1.0);
                                    }
                                }
                                REQUIRE(++pushes < 5000);
                            }
                            now += 1.0;
                        }
                    }
                }
            }
        }

TEST_CASE("the second entry aligns from where the cursor was left") {
    FakeAndroid phone;
    phone.view = phone.logical = {1600, 2560};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {0, 1079};
    portal.android_length = 2560;
    Session session(phone);
    session.configure(portal, phone.view, MotionConfig{}, AccelModel{});

    session.enter(100, 1.0);
    // The corner move is marked to go out on its own.
    CHECK(phone.jumps == 1);
    CHECK(phone.y == doctest::Approx(portal.to_android(100)).epsilon(0.001));
    session.motion(50.0, 300.0, 1.01);
    while (!session.motion(-40.0, 0.0, 1.02)) {
    }
    const double left_at = phone.y;
    session.leave();

    // No corner move this time: the cursor goes straight to the new height.
    phone.x = 0.0;
    session.enter(700, 2.0);
    CHECK(phone.jumps == 1);
    CHECK(phone.x == 0.0);
    CHECK(phone.y == doctest::Approx(portal.to_android(700)).epsilon(0.001));
    CHECK(phone.y != doctest::Approx(left_at));
}

TEST_CASE("a parked cursor enters with one report") {
    FakeAndroid phone;
    phone.view = phone.logical = {1600, 2560};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {0, 1079};
    portal.android_length = 2560;
    MotionConfig motion;
    motion.park = true;
    Session session(phone);
    session.configure(portal, phone.view, motion, AccelModel{});

    // First entry: the corner reference, then the height.
    session.enter(100, 1.0);
    CHECK(phone.jumps == 1);
    session.motion(50.0, 300.0, 1.01);
    while (!session.motion(-40.0, 0.0, 1.02)) {
    }
    session.leave();
    CHECK(phone.jumps == 2);
    CHECK(phone.x == 1599.0);
    CHECK(phone.y == 2559.0);

    // From the corner, one report reaches the shared edge at the new height.
    session.enter(700, 2.0);
    CHECK(phone.jumps == 3);
    CHECK(phone.x == 0.0);
    CHECK(phone.y == doctest::Approx(portal.to_android(700)).epsilon(0.001));
    CHECK(session.tracker().x().width() == 0.0);
    CHECK(inside(session.tracker().y(), phone.y));
}

TEST_CASE("an imported config keeps this computer's adb section") {
    Config imported;
    REQUIRE(parse_config("[adb]\nkill_server = false\npath = /tmp/other\nfirst_port = 7000\n"
                         "--- device ---\n[device]\nserial = A1\n[motion]\nsensitivity = 0.5\n"
                         "[adb]\nproxy = true\nport = 7100\n",
                         imported)
                .empty());
    Config local;
    local.adb_path = "/usr/bin/adb";
    CHECK(keep_local_adb(imported, local));
    CHECK(imported.adb_kill_server);
    CHECK(imported.adb_path == "/usr/bin/adb");
    CHECK(imported.adb_first_port == local.adb_first_port);
    // A device's proxy switch and port are this computer's too; its motion is not.
    CHECK(!imported.devices[0].adb_proxy);
    CHECK(imported.devices[0].adb_port == 0);
    CHECK(imported.devices[0].motion.sensitivity == 0.5);
    CHECK(!keep_local_adb(imported, local));
    // A device this computer has already keeps its own proxy.
    local.devices.resize(1);
    local.devices[0].serial = "A1";
    local.devices[0].adb_proxy = true;
    CHECK(keep_local_adb(imported, local));
    CHECK(imported.devices[0].adb_proxy);
    // A local device's name is not a serial: it passes its proxy on to no one.
    local.devices[0].serial = "B2";
    local.devices[0].name = "A1";
    imported.devices[0].adb_proxy = true;
    CHECK(keep_local_adb(imported, local));
    CHECK(!imported.devices[0].adb_proxy);

    // A board's serial port is this computer's too; an unknown board comes in off.
    Config boards;
    REQUIRE(parse_config("--- device ---\n[device]\nserial = mim-ttyACM0\nlink = mim\n"
                         "[mim]\nport = /dev/ttyACM0\n",
                         boards)
                .empty());
    CHECK(boards.devices[0].enabled);
    CHECK(keep_local_adb(boards, Config{}));
    CHECK(!boards.devices[0].enabled);
}

TEST_CASE("a value that was never given is written as nothing, and reads back so") {
    DeviceConfig device;
    REQUIRE(parse_device("[device]\nserial = S1\n", device).empty());
    const std::string file = format_device(device);
    CHECK(file.find("auto") == std::string::npos);
    // Nothing is written for what was not given, not even an empty value.
    CHECK(file.find("width") == std::string::npos);
    CHECK(file.find("[adb]") == std::string::npos);
    DeviceConfig back;
    REQUIRE(parse_device(file, back).empty());
    CHECK(back.width == 0);
    CHECK(back.rotation == -1);
    CHECK(back.accel == AccelSetting::automatic);
    CHECK(back.segment_start == centred_start);
}

TEST_CASE("the cursor crosses from one device to the one beside it") {
    // A stands upright; B sits at its right edge, touching rows 500 to 1500.
    FakeAndroid a;
    a.view = a.logical = {1600, 2560};
    FakeAndroid b;
    b.view = b.logical = {1080, 2400};
    Portal from_pc;
    from_pc.side = Side::right;
    from_pc.anchor = from_pc.segment = {0, 1079};
    from_pc.android_length = 2560;
    MotionConfig motion;
    motion.park = true;
    Session on_a(a);
    on_a.configure(from_pc, a.view, motion, AccelModel{}, {{Side::right, {500, 1500}, 7}});
    // B touches no monitor: it is only reached through A.
    Session on_b(b);
    on_b.configure(Portal{}, b.view, motion, AccelModel{}, {{Side::left, {0, 2399}, 3}}, false);
    CHECK(!on_b.edge_hit(100, 5.0, false, 1.0));

    on_a.enter(100, 1.0);
    // Above B's part of the edge the right edge is a wall.
    on_a.motion(0.0, -400.0, 1.01);
    for (int step = 0; step < 100; ++step)
        CHECK(!on_a.motion(40.0, 0.0, 1.02 + step * 0.004));
    CHECK(a.x == 1599.0);
    CHECK(a.y < 500.0);

    // Down into B's part, a push to the right leaves for B.
    on_a.motion(-200.0, 900.0, 2.0);
    bool left = false;
    for (int step = 0; step < 200 && !left; ++step)
        left = on_a.motion(40.0, 0.0, 2.1 + step * 0.004);
    REQUIRE(left);
    CHECK(on_a.exit() == 7);
    const double at = on_a.along(Side::right);
    CHECK(at == doctest::Approx(a.y));
    CHECK(at >= 500.0);
    CHECK(at <= 1500.0);
    on_a.leave();
    // A's cursor is parked again.
    CHECK(a.x == 1599.0);
    CHECK(a.y == 2559.0);

    // B takes it at its left edge, at the height the caller mapped to.
    on_b.enter_at(Side::left, 1200.0, 3.0);
    CHECK(on_b.remote());
    CHECK(b.x == 0.0);
    CHECK(b.y == doctest::Approx(1200.0).epsilon(0.001));
    // Pushing back left returns to A, not to the PC.
    bool back = false;
    for (int step = 0; step < 200 && !back; ++step)
        back = on_b.motion(-40.0, 0.0, 3.1 + step * 0.004);
    REQUIRE(back);
    CHECK(on_b.exit() == 3);
    CHECK(on_b.along(Side::left) == doctest::Approx(b.y));
}

TEST_CASE("crossing waits for the push, the segment and a free button") {
    FakeAndroid phone;
    phone.view = phone.logical = {1000, 1000};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {100, 500};
    portal.android_length = 1000;
    MotionConfig motion;
    motion.enter_push = 30.0;
    Session session(phone);
    session.configure(portal, phone.view, motion, AccelModel{});

    CHECK_FALSE(session.edge_hit(50, 100.0, false, 1.0));
    CHECK_FALSE(session.edge_hit(300, 100.0, true, 1.0));
    CHECK_FALSE(session.edge_hit(300, 20.0, false, 1.0));
    CHECK_FALSE(session.edge_hit(300, 20.0, false, 2.0));
    CHECK(session.edge_hit(300, 20.0, false, 2.1));
}

TEST_CASE("leaving releases what was held") {
    FakeAndroid phone;
    phone.view = phone.logical = {1000, 1000};
    Portal portal;
    portal.anchor = portal.segment = {0, 999};
    portal.android_length = 1000;
    Session session(phone);
    session.configure(portal, phone.view, MotionConfig{}, AccelModel{});

    session.key(0x04, true);
    CHECK(phone.keys_down.empty());

    session.enter(10, 1.0);
    session.key(0x04, true);
    session.key(0x04, true);
    session.key(0xE0, true);
    session.button(1, true);
    session.button(9, true);
    CHECK(phone.keys_down.size() == 2);
    CHECK(phone.buttons_down == 2U);
    session.leave();
    CHECK(phone.keys_down.empty());
    CHECK(phone.buttons_down == 0U);
    CHECK_FALSE(session.remote());
}

TEST_CASE("a locked session keeps the input until it is unlocked") {
    FakeAndroid phone;
    phone.view = phone.logical = {1600, 2560};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {0, 1079};
    portal.android_length = 2560;
    Session session(phone);
    session.configure(portal, phone.view, MotionConfig{}, AccelModel{});

    session.enter(100, 1.0);
    session.motion(50.0, 300.0, 1.01);
    session.set_locked(true);
    CHECK(session.locked());
    // Pushed hard against the edge that faces the PC: nothing gives.
    for (int step = 0; step < 200; ++step)
        CHECK_FALSE(session.motion(-40.0, 0.0, 1.02 + step * 0.004));
    CHECK(session.remote());
    CHECK(phone.x == 0.0);
    CHECK(inside(session.tracker().x(), phone.view_x()));
    CHECK(inside(session.tracker().y(), phone.view_y()));

    // What was pushed while locked does not count: unlocking alone, and a
    // move away from the edge, leave the input where it is.
    session.set_locked(false);
    CHECK_FALSE(session.motion(40.0, 0.0, 2.0));
    CHECK(session.remote());
    // From here it returns as it always did.
    bool back = false;
    for (int step = 0; step < 200 && !back; ++step)
        back = session.motion(-40.0, 0.0, 2.01 + step * 0.004);
    CHECK(back);
    CHECK(session.exit() == Session::to_pc);

    // Leaving unlocks: the next visit is not held.
    session.set_locked(true);
    session.leave();
    CHECK_FALSE(session.locked());
}

TEST_CASE("a locked session does not hand the cursor to a neighbour") {
    FakeAndroid a;
    a.view = a.logical = {1600, 2560};
    Portal from_pc;
    from_pc.side = Side::right;
    from_pc.anchor = from_pc.segment = {0, 1079};
    from_pc.android_length = 2560;
    Session on_a(a);
    on_a.configure(from_pc, a.view, MotionConfig{}, AccelModel{},
                   {{Side::right, {0, 2559}, 7}});
    on_a.enter(100, 1.0);
    on_a.set_locked(true);
    for (int step = 0; step < 200; ++step)
        CHECK_FALSE(on_a.motion(40.0, 0.0, 1.02 + step * 0.004));
    CHECK(a.x == 1599.0);
}

TEST_CASE("key codes and hotkeys") {
    HidKey key;
    REQUIRE(evdev_to_hid(30, key));
    CHECK(key.usage == 0x04);
    CHECK_FALSE(key.media);
    REQUIRE(evdev_to_hid(115, key));
    CHECK(key.usage == 0x00E9);
    CHECK(key.media);
    REQUIRE(evdev_to_hid(124, key));
    CHECK(key.usage == 0x89);
    CHECK_FALSE(evdev_to_hid(700, key));

    unsigned button = 0;
    REQUIRE(evdev_to_button(0x112, button));
    CHECK(button == 3);
    CHECK_FALSE(evdev_to_button(0x120, button));

    Hotkey hotkey;
    REQUIRE(parse_hotkey("Ctrl + Alt+S", hotkey));
    CHECK(hotkey.modifiers == (mod_ctrl | mod_alt));
    CHECK(hotkey.usage == 0x16);
    REQUIRE(parse_hotkey("ctrl+alt+shift+escape", hotkey));
    CHECK(hotkey.usage == 0x29);
    REQUIRE(parse_hotkey("f13", hotkey));
    CHECK(hotkey.usage == 0x68);
    REQUIRE(parse_hotkey("", hotkey));
    CHECK_FALSE(hotkey.set());
    CHECK_FALSE(parse_hotkey("ctrl+alt", hotkey));
    CHECK_FALSE(parse_hotkey("ctrl+nosuchkey", hotkey));
    CHECK_FALSE(parse_hotkey("a+b", hotkey));
}

TEST_CASE("config.ini round-trips and rejects what it does not know") {
    Config config;
    const char* text = "[daemon]\n"
                       "backend = portal\n"
                       "toggle_hotkey = ctrl+alt+k ; comment\n"
                       "[mouse]\n"
                       "buttons = 7\n"
                       "button_map = 1,3,2\n"
                       "[motion]\n"
                       "sensitivity = 2.5\n"
                       "report_rate_hz = 125\n"
                       "entry = corner\n"
                       "[media]\n"
                       "target = Tab\n"
                       "play_pause = ctrl+alt+p\n"
                       "[adb]\n"
                       "kill_server = false\n"
                       "first_port = 7100\n";
    REQUIRE(parse_globals(text, config).empty());
    CHECK(config.backend == "portal");
    // The 0.3.4 hotkeys are read and dropped: they live in each device's file now.
    CHECK(format_globals(config).find("hotkey") == std::string::npos);
    CHECK(format_globals(config).find("target") == std::string::npos);
    CHECK(format_globals(config).find("play_pause") == std::string::npos);
    CHECK(config.mouse_buttons == 7);
    CHECK(config.button_map.size() == 3);
    CHECK(config.motion.sensitivity == 2.5);
    CHECK(config.motion.entry == EntryMode::corner);
    CHECK(config.report_rate_hz == 125);
    CHECK(!config.report_every);
    CHECK(!config.adb_kill_server);
    CHECK(config.adb_first_port == 7100);
    CHECK(config.devices.empty());

    Config again;
    REQUIRE(parse_globals(format_globals(config), again).empty());
    CHECK(format_globals(again) == format_globals(config));

    Config every;
    REQUIRE(parse_globals("[motion]\nreport_rate_hz = every\n", every).empty());
    CHECK(every.report_every);
    REQUIRE(parse_globals(format_globals(every), again).empty());
    CHECK(again.report_every);

    Config untouched;
    CHECK(parse_globals("[motion]\nspeed = 2\n", untouched).substr(0, 7) == "line 2:");
    CHECK(parse_globals("[nope]\n", untouched).substr(0, 7) == "line 1:");
    CHECK_FALSE(parse_globals("[motion]\nsensitivity = 0\n", untouched).empty());
    CHECK_FALSE(parse_globals("[motion]\nreport_rate_hz = 9000\n", untouched).empty());
    CHECK(parse_globals("[media]\nplay_pause = ctrl+\n", untouched).empty());
    CHECK(parse_globals("[daemon]\npause_hotkey = ctrl+\n", untouched).empty());
    CHECK_FALSE(parse_globals("[media]\nwarp = ctrl+w\n", untouched).empty());
    CHECK_FALSE(parse_globals("[adb]\nfirst_port = 5555\n", untouched).empty());
    CHECK(untouched.backend == "auto");
}

TEST_CASE("a device's file round-trips and only replaces what it names") {
    DeviceConfig device;
    const char* text = "[device]\n"
                       "serial = ABCD1234EFG\n"
                       "name = Galaxy Tab S11\n"
                       "hotkey = ctrl+alt+1\n"
                       "[placement]\n"
                       "monitor = HDMI-A-1\n"
                       "side = top\n"
                       "start = -120\n"
                       "corner_margin = 10%\n"
                       "rotation = 90\n"
                       "turn_input = 180\n"
                       "[detected]\n"
                       "width = 1600\n"
                       "gain = 2.1692\n"
                       "accel = off\n"
                       "read_at = 2026-10-05\n"
                       "[motion]\n"
                       "sensitivity = 1.4\n"
                       "park = false\n"
                       "[keyboard]\n"
                       "enabled = false\n"
                       "[mouse]\n"
                       "enabled = false\n"
                       "[keys]\n"
                       "switch = ctrl+alt+1\n"
                       "lock = ctrl+alt+l\n"
                       "play_pause = ctrl+alt+p\n"
                       "volume_up = ctrl+alt+up\n"
                       "[adb]\n"
                       "port = 7000\n"
                       "proxy = true\n";
    REQUIRE(parse_device(text, device).empty());
    CHECK(device.keys.switch_key == "ctrl+alt+1");
    CHECK(device.keys.lock == "ctrl+alt+l");
    CHECK(device.keys.resync.empty());
    CHECK(device.keys.media[1] == "ctrl+alt+p");
    CHECK(device.keys.media[0].empty());
    REQUIRE(media_key_count == 7);
    CHECK(media_keys[5].name == "volume_up");
    CHECK(media_keys[5].usage == 0x00E9);
    CHECK(media_keys[6].name == "volume_down");
    CHECK(media_keys[6].usage == 0x00EA);
    CHECK(device.keys.media[5] == "ctrl+alt+up");
    // The 0.3.4 key is gone from what is written.
    CHECK(format_device(device).find("\nhotkey = ") == std::string::npos);
    DeviceConfig broken;
    CHECK_FALSE(parse_device("[keys]\nlock = ctrl+\n", broken).empty());
    CHECK_FALSE(parse_device("[keys]\nwarp = ctrl+w\n", broken).empty());
    CHECK(parse_device("[device]\nhotkey = ctrl+\n", broken).empty());
    CHECK(device.serial == "ABCD1234EFG");
    CHECK(device.name == "Galaxy Tab S11");
    CHECK(label_of(device) == "Galaxy Tab S11");
    CHECK(device.enabled);
    CHECK(device.side == Side::top);
    // A start before the edge's own: the device reaches past the corner.
    CHECK(device.segment_start == -120);
    CHECK(device.segment_length == -1);
    CHECK(device.corner_margin == doctest::Approx(0.10));
    CHECK(device.rotation == 90);
    CHECK(device.width == 1600);
    CHECK(device.height == 0);
    CHECK(device.accel == AccelSetting::off);
    CHECK(device.adb_port == 7000);
    CHECK(device.adb_proxy);

    DeviceConfig again;
    REQUIRE(parse_device(format_device(device), again).empty());
    CHECK(format_device(again) == format_device(device));

    // What the file leaves out follows config.ini; what it names replaces it.
    Config config;
    config.motion.sensitivity = 2.0;
    config.motion.sensitivity_y = 3.0;
    config.motion.scroll_sensitivity = 0.7;
    const Motion motion = motion_of(config, device);
    CHECK(motion.sensitivity == 1.4);
    // Its own sensitivity is for both axes, not the global vertical one.
    CHECK(motion.sensitivity_y == 0.0);
    CHECK(motion.scroll_sensitivity == 0.7);
    CHECK(motion.entry == EntryMode::aligned);
    CHECK(motion.no_cross_while_button);
    CHECK(!keyboard_of(config, device));
    CHECK(keyboard_of(config, DeviceConfig{}));
    CHECK(!mouse_of(config, device));
    CHECK(mouse_of(config, DeviceConfig{}));
    // Media is its own switch: it does not follow the keyboard.
    CHECK(media_of(config, device));
    config.media = false;
    CHECK(!media_of(config, DeviceConfig{}));

    // A serial names a file; a name is shown and typed. Both come from
    // outside, so only plain ones are let in.
    CHECK(valid_serial("ABCD1234EFG"));
    CHECK(!valid_serial(""));
    CHECK(!valid_serial("../x"));
    CHECK(!valid_serial(".hidden"));
    CHECK(!valid_serial("a b"));
    CHECK(!valid_serial("nul"));
    CHECK(!valid_serial("COM1.x"));
    CHECK(valid_name("Galaxy Tab S11"));
    CHECK(!valid_name("a=b"));
    CHECK(!valid_name("two\nlines"));
    // `;` and `#` would start a comment when the file is read back.
    CHECK(!valid_name("Tab #2"));
    CHECK(!valid_name("a ;b"));
    {
        // A name that is another device's serial could never be reached.
        Config clash;
        clash.devices.resize(2);
        clash.devices[0].serial = "A1";
        clash.devices[0].name = "B2";
        clash.devices[1].serial = "B2";
        clash.devices[1].name = "Other";
        CHECK_FALSE(validate_config(clash).empty());
        clash.devices[0].name = "Tab";
        CHECK(validate_config(clash).empty());
    }
    {
        // An editor's byte order mark does not spoil the first line.
        Config marked;
        CHECK(parse_globals("\xEF\xBB\xBF[daemon]\nbackend = x11\n", marked).empty());
        CHECK(marked.backend == "x11");
    }
    {
        // The proxy is off until it is switched on, for a device, by hand.
        const DeviceConfig fresh;
        CHECK(!fresh.adb_proxy);
        CHECK(Config{}.adb_kill_server);
    }

    DeviceConfig untouched;
    CHECK_FALSE(parse_device("[device]\nserial = a/b\n", untouched).empty());
    CHECK_FALSE(parse_device("[placement]\nrotation = 45\n", untouched).empty());
    CHECK_FALSE(parse_device("[adb]\nport = 5555\n", untouched).empty());
    CHECK_FALSE(parse_device("[motion]\nreport_rate_hz = 9000\n", untouched).empty());
    CHECK_FALSE(parse_device("[daemon]\nbackend = x11\n", untouched).empty());
    CHECK(untouched.serial.empty());
}

TEST_CASE("devices get their ports, and what clashes between them is refused") {
    Config config;
    config.adb_first_port = 6555;
    config.devices.resize(4);
    config.devices[0].serial = "A";
    config.devices[1].serial = "B";
    config.devices[1].adb_port = 6556;
    config.devices[2].serial = "C";
    config.devices[3].serial = "D";
    config.devices[3].name = "Phone";
    // The free ports upward from the first, around the one B took.
    CHECK(adb_port_of(config, 0) == 6555);
    CHECK(adb_port_of(config, 1) == 6556);
    CHECK(adb_port_of(config, 2) == 6557);
    CHECK(adb_port_of(config, 3) == 6558);
    CHECK(validate_config(config).empty());
    CHECK(find_device(config, "C") == 2);
    CHECK(find_device(config, "Phone") == 3);
    CHECK(find_device(config, "nobody") == 4);

    Config clash = config;
    clash.devices[2].serial = "A";
    CHECK_FALSE(validate_config(clash).empty());
    // On a file system that ignores case these would be one file.
    clash.devices[2].serial = "a";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    clash.devices[0].name = "Phone";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    // A switch or media key works wherever the input is: nothing else,
    // on any device, may be the same combination.
    clash.devices[0].keys.switch_key = "ctrl+alt+1";
    clash.devices[1].keys.switch_key = "alt+ctrl+1";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    clash.devices[0].keys.media[1] = "ctrl+alt+p";
    clash.devices[1].keys.media[1] = "ctrl+alt+p";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    clash.devices[0].keys.switch_key = "ctrl+alt+1";
    clash.devices[1].keys.lock = "ctrl+alt+1";
    CHECK_FALSE(validate_config(clash).empty());
    // Within one device every key is its own.
    clash = config;
    clash.devices[0].keys.lock = "ctrl+alt+l";
    clash.devices[0].keys.resync = "ctrl+alt+l";
    CHECK_FALSE(validate_config(clash).empty());
    // Lock and resync act on the device that has the input, so two devices
    // may use the same keys for them.
    Config shared = config;
    shared.devices[0].keys.switch_key = "ctrl+alt+1";
    shared.devices[1].keys.switch_key = "ctrl+alt+2";
    shared.devices[0].keys.lock = shared.devices[1].keys.lock = "ctrl+alt+l";
    shared.devices[0].keys.resync = shared.devices[1].keys.resync = "ctrl+alt+r";
    CHECK(validate_config(shared).empty());

    // Beside another device is fine; beside nothing or in a ring is not.
    Config chain = config;
    chain.devices[1].beside = "A";
    chain.devices[2].beside = "B";
    CHECK(validate_config(chain).empty());
    chain.devices[0].beside = "C";
    CHECK_FALSE(validate_config(chain).empty());
    chain = config;
    chain.devices[1].beside = "Z";
    CHECK_FALSE(validate_config(chain).empty());
}

TEST_CASE("everything in one text reads back as devices") {
    Config config;
    config.motion.sensitivity = 1.5;
    config.devices.resize(2);
    config.devices[0].serial = "A";
    config.devices[0].name = "Tab";
    config.devices[0].side = Side::left;
    config.devices[1].serial = "B";
    config.devices[1].beside = "A";
    config.devices[1].motion.scroll_sensitivity = 0.5;
    Config back;
    REQUIRE(parse_config(format_config(config), back).empty());
    CHECK(format_config(back) == format_config(config));
    REQUIRE(back.devices.size() == 2);
    CHECK(back.devices[1].beside == "A");
    CHECK(back.devices[1].motion.scroll_sensitivity == 0.5);

    // No more devices than the limit, however the text names them.
    std::string many = "[motion]\n";
    for (size_t index = 0; index <= max_devices; ++index)
        many += "--- device ---\n[device]\nserial = S" + std::to_string(index) + "\n";
    Config flood;
    CHECK_FALSE(parse_config(many, flood).empty());
}

TEST_CASE("a view point maps to the touch device's raw coordinates") {
    const Size view{1600, 2560};
    CHECK(to_touch_point(view, view, 0, 10.0, 20.0).x == 10);
    CHECK(to_touch_point(view, view, 0, 10.0, 20.0).y == 20);

    // A raw area of another size is scaled to.
    const TouchPoint s = to_touch_point(view, {800, 1280}, 0, 100.0, 200.0);
    CHECK(s.x == 50);
    CHECK(s.y == 100);

    // A point outside the view is clamped, never wrapped.
    const TouchPoint outside = to_touch_point(view, view, 0, 1e6, -1e6);
    CHECK(outside.x == view.w - 1);
    CHECK(outside.y == 0);
}

TEST_CASE("a quarter turn swaps the raw axes and the corners follow the turn") {
    // The display as shown is 2560 x 1600; its raw axes are 1600 x 2560.
    const Size view{2560, 1600};
    CHECK(touch_raw_size(view, 0).w == 2560);
    for (const int rotation : {90, 270}) {
        CHECK(touch_raw_size(view, rotation).w == 1600);
        CHECK(touch_raw_size(view, rotation).h == 2560);
    }
    const Size raw = touch_raw_size(view, 90);
    // Rotation 90: the top left of what is shown is the top right of the raw area.
    TouchPoint at = to_touch_point(view, raw, 90, 0.0, 0.0);
    CHECK(at.x == raw.w - 1);
    CHECK(at.y == 0);
    at = to_touch_point(view, raw, 90, view.w - 1, view.h - 1);
    CHECK(at.x == 0);
    CHECK(at.y == raw.h - 1);
    // Rotation 270 is the opposite turn: the top left is the raw bottom left.
    at = to_touch_point(view, raw, 270, 0.0, 0.0);
    CHECK(at.x == 0);
    CHECK(at.y == raw.h - 1);
    // Rotation 180 mirrors both.
    at = to_touch_point(view, view, 180, 0.0, 0.0);
    CHECK(at.x == view.w - 1);
    CHECK(at.y == view.h - 1);
}

TEST_CASE("the touch settings are read, written back and overridden per device") {
    Config config;
    REQUIRE(parse_globals("[touch]\nenabled = true\nscroll = 120\nscroll_pan = -80\n"
                          "scroll_release_ms = 300\ntap_button = 2\n",
                          config)
                .empty());
    CHECK(config.touch.enabled);
    CHECK(config.touch.scroll == 120);
    CHECK(config.touch.scroll_pan == -80);
    CHECK(config.touch.release_ms == 300);
    CHECK(config.touch.button == 2);
    Config back;
    REQUIRE(parse_globals(format_globals(config), back).empty());
    CHECK(format_globals(back) == format_globals(config));

    DeviceConfig device;
    REQUIRE(parse_device("[device]\nserial = S1\n[touch]\nenabled = false\nscroll = 0\n", device)
                .empty());
    const Touch seen = touch_of(config, device);
    CHECK_FALSE(seen.enabled);
    CHECK(seen.scroll == 0);
    // What the file leaves out follows config.ini.
    CHECK(seen.scroll_pan == -80);
    CHECK(seen.release_ms == 300);
    CHECK(seen.button == 2);
    DeviceConfig again;
    REQUIRE(parse_device(format_device(device), again).empty());
    CHECK(format_device(again) == format_device(device));

    // Values out of range, and unknown keys, are refused.
    Config bad;
    CHECK_FALSE(parse_globals("[touch]\nscroll = 5000\n", bad).empty());
    CHECK_FALSE(parse_globals("[touch]\nscroll_release_ms = -1\n", bad).empty());
    CHECK_FALSE(parse_globals("[touch]\ntap_button = 0\n", bad).empty());
    CHECK_FALSE(parse_globals("[touch]\nnonsense = 1\n", bad).empty());
}

namespace {

struct TouchRig {
    FakeAndroid phone;
    Session session{phone};
    // The display as shown, and the touchscreen's raw size for the rotation.
    Size shown{1600, 2560};
    Size natural;
    int rotation;
    explicit TouchRig(const int rotation_ = 0) : natural(touch_raw_size(shown, rotation_)), rotation(rotation_) {
        phone.view = phone.logical = shown;
        Portal portal;
        portal.side = Side::right;
        portal.anchor = portal.segment = {0, 1079};
        portal.android_length = phone.view.h;
        session.configure(portal, phone.view, MotionConfig{}, AccelModel{});
        TouchSetup setup;
        setup.enabled = true;
        setup.natural = natural;
        setup.rotation = rotation;
        setup.button = 1;
        setup.scroll = 100;
        setup.scroll_pan = 100;
        setup.release_s = 0.2;
        session.set_touch(setup);
        session.enter(100, 1.0);
    }
    // Where the session's tracked cursor is, in the device's raw coordinates.
    [[nodiscard]] TouchPoint here() const {
        return to_touch_point(phone.view, natural, rotation, session.tracker().x().mid(),
                              session.tracker().y().mid());
    }
};

} // namespace

TEST_CASE("the left button taps at the tracked position and the others stay mouse buttons") {
    TouchRig rig;
    rig.session.button(1, true);
    REQUIRE(rig.phone.touches.size() == 1);
    CHECK(rig.phone.touches[0].contact == 0);
    CHECK(rig.phone.touches[0].down);
    CHECK(rig.phone.touches[0].x == rig.here().x);
    CHECK(rig.phone.touches[0].y == rig.here().y);
    CHECK(rig.phone.buttons_down == 0);
    rig.session.button(1, false);
    REQUIRE(rig.phone.touches.size() == 2);
    CHECK_FALSE(rig.phone.touches[1].down);
    rig.session.button(2, true);
    CHECK(rig.phone.buttons_down == (1U << 2));
    CHECK(rig.phone.touches.size() == 2);
}

TEST_CASE("moving while the tap is held drags contact 0") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.session.motion(30.0, 10.0, 1.1);
    REQUIRE(rig.phone.touches.size() == 2);
    CHECK(rig.phone.touches[1].contact == 0);
    CHECK(rig.phone.touches[1].down);
    CHECK(rig.phone.touches[1].x == rig.here().x);
    CHECK(rig.phone.touches[1].y == rig.here().y);
}

TEST_CASE("a wheel notch is a swipe of the second finger, independent of contact 0") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.phone.touches.clear();
    const TouchPoint start = rig.here();
    rig.session.scroll(1.0, 0.0);
    // A positive wheel scrolls the view up, so the content and the finger go
    // down. The sink does the putting down, the moves and the lift.
    REQUIRE(rig.phone.swipes.size() == 1);
    const SwipePlan& plan = rig.phone.swipes[0];
    CHECK(plan.x == start.x);
    CHECK(plan.y == start.y);
    CHECK(plan.dx == 0);
    CHECK(plan.dy == 100);
    CHECK(plan.area.w == rig.natural.w);
    CHECK(plan.area.h == rig.natural.h);
    CHECK(plan.steps == 4);
    CHECK_FALSE(plan.restart);
    // The next notch is another swipe from the cursor, not one notch further:
    // adding to a swipe that goes on is the sink's.
    rig.session.scroll(1.0, 0.0);
    REQUIRE(rig.phone.swipes.size() == 2);
    CHECK(rig.phone.swipes[1].y == start.y);
    CHECK(rig.phone.swipes[1].dy == 100);
    // The finger of the click was not touched, and the wheel was taken.
    CHECK(rig.phone.touches.empty());
    CHECK(rig.phone.wheels.empty());
}

TEST_CASE("the swipe settings reach the sink") {
    TouchRig rig;
    TouchSetup setup;
    setup.enabled = true;
    setup.natural = rig.natural;
    setup.scroll = 100;
    setup.steps = 7;
    setup.start_s = 0.002;
    setup.total_s = 0.03;
    setup.release_s = 0.0;
    setup.restart = true;
    rig.session.set_touch(setup);
    rig.session.scroll(1.0, 0.0);
    REQUIRE(rig.phone.swipes.size() == 1);
    CHECK(rig.phone.swipes[0].steps == 7);
    CHECK(rig.phone.swipes[0].start_s == 0.002);
    CHECK(rig.phone.swipes[0].total_s == 0.03);
    CHECK(rig.phone.swipes[0].release_s == 0.0);
    CHECK(rig.phone.swipes[0].restart);
}

TEST_CASE("a notch that cannot move the finger does not tap") {
    TouchRig rig;
    // Push the cursor against the top edge, then scroll up: the finger would
    // be put down at the edge and lifted without moving, which is a tap.
    rig.session.motion(0.0, -5000.0, 1.5);
    rig.phone.touches.clear();
    rig.session.scroll(-1.0, 0.0);
    CHECK(rig.phone.swipes.empty());
    // The other way it moves.
    rig.session.scroll(1.0, 0.0);
    CHECK_FALSE(rig.phone.swipes.empty());
}

TEST_CASE("changing the touch setup lifts a contact that is down") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.session.scroll(1.0, 0.0);
    rig.phone.touches.clear();
    TouchSetup off;
    off.natural = rig.natural;
    rig.session.set_touch(off);
    int lifts0 = 0;
    int lifts1 = 0;
    for (const auto& call : rig.phone.touches) {
        if (!call.down && call.contact == 0)
            ++lifts0;
        if (!call.down && call.contact == 1)
            ++lifts1;
    }
    CHECK(lifts0 == 1);
    CHECK(lifts1 == 1);
    // Switched on again, a movement places nothing: no button is held.
    TouchSetup on;
    on.enabled = true;
    on.natural = rig.natural;
    rig.session.set_touch(on);
    rig.phone.touches.clear();
    rig.session.motion(10.0, 10.0, 3.0);
    CHECK(rig.phone.touches.empty());
}

TEST_CASE("a tap button above the mouse's button count still taps") {
    TouchRig rig;
    TouchSetup setup;
    setup.enabled = true;
    setup.natural = rig.natural;
    setup.button = 7;
    rig.session.set_touch(setup);
    rig.session.button(7, true);
    REQUIRE(rig.phone.touches.size() == 1);
    CHECK(rig.phone.touches[0].contact == 0);
    CHECK(rig.phone.touches[0].down);
}

TEST_CASE("a swipe stays on the display") {
    TouchRig rig;
    for (int notch = 0; notch < 100; ++notch)
        rig.session.scroll(-1.0, 1.0);
    REQUIRE_FALSE(rig.phone.swipes.empty());
    for (const auto& plan : rig.phone.swipes) {
        CHECK(plan.x + plan.dx >= 0);
        CHECK(plan.x + plan.dx < rig.natural.w);
        CHECK(plan.y + plan.dy >= 0);
        CHECK(plan.y + plan.dy < rig.natural.h);
    }
}

TEST_CASE("a wheel stays a mouse wheel while touch scroll is 0") {
    TouchRig rig;
    TouchSetup setup;
    setup.enabled = true;
    setup.natural = rig.natural;
    rig.session.set_touch(setup);
    rig.session.scroll(2.0, 0.0);
    CHECK(rig.phone.touches.empty());
    REQUIRE(rig.phone.wheels.size() == 1);
    CHECK(rig.phone.wheels[0].first == 2);
}

TEST_CASE("leaving lifts the touch contacts") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.session.scroll(1.0, 0.0);
    rig.phone.touches.clear();
    rig.session.leave();
    int lifts0 = 0;
    int lifts1 = 0;
    for (const auto& call : rig.phone.touches) {
        if (!call.down && call.contact == 0)
            ++lifts0;
        if (!call.down && call.contact == 1)
            ++lifts1;
    }
    CHECK(lifts0 == 1);
    CHECK(lifts1 == 1);
}

TEST_CASE("the tap follows every rotation") {
    for (const int rotation : {0, 90, 180, 270}) {
        TouchRig rig(rotation);
        rig.session.button(1, true);
        REQUIRE(rig.phone.touches.size() == 1);
        CHECK(rig.phone.touches[0].x == rig.here().x);
        CHECK(rig.phone.touches[0].y == rig.here().y);
        CHECK(rig.phone.touches[0].x < rig.natural.w);
        CHECK(rig.phone.touches[0].y < rig.natural.h);
    }
}

TEST_CASE("the position cell hands over a whole range, never half of two") {
    PositionCell cell;
    Tracker tracker;
    tracker.reset({101, 51});
    cell.store(tracker);
    PositionCell::Value value = cell.load();
    CHECK(value.x_lo == 0.0);
    CHECK(value.x_hi == 100.0);
    CHECK(value.y_hi == 50.0);

    // The writer keeps all four numbers equal to one another plus a fixed
    // step; a reader that ever sees them out of step has seen a torn write.
    tracker.reset({1001, 1001});
    cell.store(tracker);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int step = 1; !stop.load(); ++step) {
            tracker.reset({step + 1000, step + 1000});
            cell.store(tracker);
        }
    });
    bool whole = true;
    for (int read = 0; read < 200000; ++read) {
        const PositionCell::Value seen = cell.load();
        whole = whole && seen.x_hi == seen.y_hi && seen.x_lo == 0.0 && seen.y_lo == 0.0;
    }
    stop.store(true);
    writer.join();
    CHECK(whole);
}

TEST_CASE("the gain follows Android's pointer speed and density") {
    // The Galaxy Tab S11 of docs/MATH.md: density 340, pointer speed 0 gave 2.169.
    CHECK(low_speed_gain(0, 340) == doctest::Approx(2.169).epsilon(0.001));
    CHECK(low_speed_gain(3, 340) == doctest::Approx(2.820).epsilon(0.001));
    CHECK(low_speed_gain(-4, 340) == doctest::Approx(1.301).epsilon(0.001));
    CHECK(low_speed_gain(0, 0) == 0.0);
    // A faster setting moves the cursor further for each count.
    CHECK(low_speed_gain(5, 340) > low_speed_gain(0, 340));

    DeviceConfig device;
    REQUIRE(parse_device("[detected]\ndensity = 340\npointer_speed = 3\n", device).empty());
    CHECK(device.density_dpi == 340);
    DeviceConfig back;
    REQUIRE(parse_device(format_device(device), back).empty());
    CHECK(back.density_dpi == 340);
    CHECK(back.pointer_speed == 3);
    DeviceConfig none;
    CHECK(format_device(none).find("density") == std::string::npos);
}

TEST_CASE("the gain follows the pointer speed unless one is typed") {
    DeviceConfig device;
    device.density_dpi = 340;
    device.pointer_speed = 0;
    CHECK(effective_gain(device) == doctest::Approx(2.169).epsilon(0.001));
    // Changing the pointer speed changes the gain at once.
    device.pointer_speed = 3;
    CHECK(effective_gain(device) == doctest::Approx(2.820).epsilon(0.001));
    // A typed (measured) gain wins.
    device.gain = 1.5;
    CHECK(effective_gain(device) == 1.5);
    // Without a density there is nothing to calculate.
    DeviceConfig bare;
    CHECK(effective_gain(bare) == 0.0);

    // A gain in a file that is only what the speed and density give (an earlier
    // version wrote it from adb) is not kept as a value of its own.
    DeviceConfig stale;
    REQUIRE(parse_device("[detected]\ndensity = 340\npointer_speed = 0\ngain = 2.169\n", stale)
                .empty());
    CHECK(stale.gain == 0.0);
    // One that differs is the user's own.
    DeviceConfig own;
    REQUIRE(
        parse_device("[detected]\ndensity = 340\npointer_speed = 0\ngain = 1.5\n", own).empty());
    CHECK(own.gain == 1.5);
}

TEST_CASE("the gain follows from the real size and the measured travel") {
    DeviceConfig device;
    // The wizard stopped at 1079 counts across and 2399 down; the screen is 1080 x 2400.
    device.width = 1080;
    device.height = 2400;
    CHECK(effective_gain(device) == 0.0);
    CHECK(gain_source(device) == GainSource::none);
    device.real_width = 1080;
    device.real_height = 2400;
    CHECK(gain_source(device) == GainSource::measured);
    CHECK(effective_gain(device) == doctest::Approx(1.0));

    // Pixels per count 2.169: both axes together.
    device.width = 498;  // 497 counts across
    device.height = 1107; // 1106 counts down
    CHECK(measured_gain(device) == doctest::Approx((1079.0 + 2399.0) / (497.0 + 1106.0)));
    CHECK(effective_gain(device) == doctest::Approx(2.1697).epsilon(0.001));

    // The real size is the screen the daemon works in; without it, the travel is.
    CHECK(view_width(device) == 1080);
    CHECK(view_height(device) == 2400);
    DeviceConfig legacy;
    legacy.width = 1600;
    legacy.height = 2560;
    CHECK_FALSE(has_real_size(legacy));
    CHECK(view_width(legacy) == 1600);
    CHECK(view_height(legacy) == 2560);
    legacy.real_width = 1080;
    CHECK_FALSE(has_real_size(legacy));
    CHECK(view_width(legacy) == 1600);
    CHECK(measured_gain(legacy) == 0.0);

    // A typed gain wins, then this, then the pointer speed and density.
    device.density_dpi = 340;
    CHECK(gain_source(device) == GainSource::measured);
    device.gain = 1.5;
    CHECK(gain_source(device) == GainSource::typed);
    CHECK(effective_gain(device) == 1.5);
    device.gain = 0.0;
    device.real_width = 0;
    CHECK(gain_source(device) == GainSource::android);
    CHECK(effective_gain(device) == doctest::Approx(2.169).epsilon(0.001));
}

TEST_CASE("a tap lands on the pixel the counts moved the cursor to") {
    // True gain 2: 540 counts across and 1200 down cross a 1081 x 2401 screen.
    DeviceConfig device;
    device.width = 541;
    device.height = 1201;
    device.real_width = 1081;
    device.real_height = 2401;
    const double gain = effective_gain(device);
    REQUIRE(gain == doctest::Approx(2.0));
    const Size view{view_width(device), view_height(device)};
    for (const int rotation : {0, 90, 180, 270}) {
        const Size raw = touch_raw_size(view, rotation);
        // The touchscreen is the real size, turned.
        CHECK(raw.w == (rotation % 180 == 0 ? 1081 : 2401));
        CHECK(raw.h == (rotation % 180 == 0 ? 2401 : 1081));
    }
    const Size raw = touch_raw_size(view, 0);
    TouchPoint at = to_touch_point(view, raw, 0, 100 * gain, 50 * gain);
    CHECK(at.x == 200);
    CHECK(at.y == 100);
    at = to_touch_point(view, raw, 0, 540 * gain, 1200 * gain);
    CHECK(at.x == 1080);
    CHECK(at.y == 2400);
    // A turned screen: the same pixel, in the touchscreen's own axes.
    at = to_touch_point(view, touch_raw_size(view, 90), 90, 0.0, 0.0);
    CHECK(at.x == 2400);
    CHECK(at.y == 0);
}

TEST_CASE("the real size is read, written and bounded") {
    DeviceConfig device;
    REQUIRE(parse_device("[detected]\nreal_width = 1080\nreal_height = 2400\n", device).empty());
    CHECK(device.real_width == 1080);
    CHECK(device.real_height == 2400);
    DeviceConfig back;
    REQUIRE(parse_device(format_device(device), back).empty());
    CHECK(back.real_width == 1080);
    CHECK(back.real_height == 2400);
    // Not set, not written.
    DeviceConfig none;
    CHECK(format_device(none).find("real_") == std::string::npos);
    DeviceConfig broken;
    CHECK_FALSE(parse_device("[detected]\nreal_width = 1\n", broken).empty());
    CHECK_FALSE(parse_device("[detected]\nreal_height = 70000\n", broken).empty());
}

TEST_CASE("how the cursor enters is one setting, and the old two keys still read") {
    Config config;
    REQUIRE(parse_globals("[motion]\nentry = aligned\n", config).empty());
    CHECK(config.motion.entry == EntryMode::aligned);
    Config back;
    REQUIRE(parse_globals(format_globals(config), back).empty());
    CHECK(back.motion.entry == EntryMode::aligned);
    CHECK(format_globals(config).find("park") == std::string::npos);

    // What earlier files said: park = true|false and entry = align|corner.
    Config old_parked;
    REQUIRE(parse_globals("[motion]\npark = true\nentry = align\n", old_parked).empty());
    CHECK(old_parked.motion.entry == EntryMode::parked);
    Config old_aligned;
    REQUIRE(parse_globals("[motion]\npark = false\nentry = align\n", old_aligned).empty());
    CHECK(old_aligned.motion.entry == EntryMode::aligned);
    Config old_corner;
    REQUIRE(parse_globals("[motion]\npark = true\nentry = corner\n", old_corner).empty());
    CHECK(old_corner.motion.entry == EntryMode::corner);
    CHECK_FALSE(parse_globals("[motion]\nentry = sideways\n", old_corner).empty());
}

TEST_CASE("a device either follows Settings' movement or has all of its own") {
    Config config;
    config.motion.sensitivity = 2.0;
    config.motion.sensitivity_y = 3.0;
    config.motion.entry = EntryMode::corner;
    DeviceConfig device;
    CHECK_FALSE(has_own_motion(device.motion));

    device.motion = motion_patch_of(config.motion);
    CHECK(has_own_motion(device.motion));
    // The same values, now the device's: a change in Settings does not reach it.
    config.motion.sensitivity = 5.0;
    const Motion own = motion_of(config, device);
    CHECK(own.sensitivity == 2.0);
    CHECK(own.sensitivity_y == 3.0);
    CHECK(own.entry == EntryMode::corner);

    DeviceConfig back;
    REQUIRE(parse_device(format_device(device), back).empty());
    CHECK(has_own_motion(back.motion));
    CHECK(motion_of(config, back).sensitivity == 2.0);
}

TEST_CASE("a board is written and read back, and its serial comes from its port") {
    CHECK(mim_serial_of("/dev/ttyUSB0") == "mim-ttyUSB0");
    CHECK(mim_serial_of("COM3") == "mim-COM3");
    CHECK(mim_serial_of("/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0") ==
          "mim-usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0");
    CHECK(mim_serial_of("/dev/a b").find(' ') == std::string::npos);
    CHECK(valid_serial(mim_serial_of("/dev/cu.usbserial-0001")));
    CHECK(mim_serial_of("").empty());

    DeviceConfig board;
    board.serial = "mim-ttyUSB0";
    board.link = LinkKind::mim;
    board.mim.port = "/dev/ttyUSB0";
    board.mim.baud = 3000000;
    board.mim.vendor_id = 0x1234;
    board.mim.product_id = 0x5678;
    board.mim.manufacturer = "maker";
    board.mim.product = "board";
    board.mim.serial_from_chip_id = true;
    DeviceConfig back;
    REQUIRE(parse_device(format_device(board), back).empty());
    CHECK(back.link == LinkKind::mim);
    CHECK(back.mim.port == board.mim.port);
    CHECK(back.mim.baud == 3000000);
    CHECK(back.mim.vendor_id == 0x1234);
    CHECK(back.mim.product_id == 0x5678);
    CHECK(back.mim.manufacturer == "maker");
    CHECK(back.mim.product == "board");
    CHECK(back.mim.serial.empty());
    CHECK(back.mim.serial_from_chip_id);

    // Nothing is assumed for what was not given: an older file has no link,
    // and a board's ids and strings are empty until they are typed.
    DeviceConfig older;
    REQUIRE(parse_device("[device]\nserial = S1\n", older).empty());
    CHECK(older.link == LinkKind::unset);
    CHECK(older.mim.vendor_id == 0);
    DeviceConfig fresh;
    REQUIRE(parse_device("[device]\nserial = mim-x\nlink = mim\n[usb]\nvendor_id =\n", fresh).empty());
    CHECK(fresh.mim.vendor_id == 0);
    CHECK_FALSE(parse_device("[device]\nlink = usb\n", fresh).empty());
    CHECK_FALSE(parse_device("[usb]\nvendor_id = 0x10000\n", fresh).empty());
    CHECK_FALSE(parse_device("[usb]\nvendor_id = 0\n", fresh).empty());
    CHECK_FALSE(parse_device("[usb]\nproduct = 0123456789012345678901234567890x\n", fresh).empty());

    // A board is not served by adb and is known by its port.
    Config config;
    config.devices = {board};
    CHECK(validate_config(config).empty());
    config.devices[0].adb_proxy = true;
    CHECK_FALSE(validate_config(config).empty());
    config.devices[0].adb_proxy = false;
    config.devices[0].serial = "other";
    CHECK_FALSE(validate_config(config).empty());
    config.devices = {board, board};
    config.devices[1].serial = "mim-ttyUSB0b";
    config.devices[1].name = "second";
    CHECK_FALSE(validate_config(config).empty());

    // The wait for a board is no longer a setting: an old line is read and dropped.
    Config globals;
    Config read;
    CHECK(parse_globals("[mim]\ntimeout_ms = 500\n", read).empty());
    CHECK(format_globals(read).find("timeout_ms") == std::string::npos);
    CHECK_FALSE(parse_globals("[mim]\nbaud = 3000000\n", read).empty());
    CHECK_FALSE(parse_globals("[mim]\ntimeout_ms = 60001\n", read).empty());
    CHECK(format_globals(globals).find("[mim]") == std::string::npos);
}

TEST_CASE("a device's start and length are pixels or a ratio of the edge") {
    DeviceConfig device;
    REQUIRE(parse_device("[device]\nserial = S1\n[placement]\nlength = 50%\nstart = 25%\n", device)
                .empty());
    CHECK(device.length_ratio.value_or(0.0) == doctest::Approx(0.5));
    CHECK(device.start_ratio.value_or(0.0) == doctest::Approx(0.25));
    CHECK(edge_length_of(device, 1080) == 540);
    CHECK(edge_start_given(device));
    CHECK(edge_start_of(device, 1080) == 270);
    // The file reads back as written.
    DeviceConfig back;
    REQUIRE(parse_device(format_device(device), back).empty());
    CHECK(format_device(back) == format_device(device));
    CHECK(format_device(device).find("length = 50%") != std::string::npos);

    // Pixels take the ratio's place, and no length is the whole edge, centred.
    REQUIRE(parse_device("[device]\nserial = S1\n[placement]\nlength = 300\n", device).empty());
    CHECK_FALSE(device.length_ratio.has_value());
    CHECK(edge_length_of(device, 1080) == 300);
    DeviceConfig whole;
    CHECK(edge_length_of(whole, 1080) == 1080);
    CHECK_FALSE(edge_start_given(whole));
    set_length_ratio(whole, 0.4);
    set_start_ratio(whole, 0.1);
    CHECK(edge_length_of(whole, 1000) == 400);
    CHECK(edge_start_of(whole, 1000) == 100);
    clear_length(whole);
    CHECK(edge_length_of(whole, 1000) == 1000);

    // A ratio is bounded, and the old physical sizes are read and dropped.
    CHECK_FALSE(parse_device("[device]\nserial = S1\n[placement]\nlength = 0%\n", device).empty());
    CHECK_FALSE(parse_device("[device]\nserial = S1\n[placement]\nlength = 5000%\n", device).empty());
    CHECK(parse_device("[device]\nserial = S1\n[placement]\nmonitor_diagonal_inch = 24\n"
                       "[detected]\ndiagonal_inch = 11\n",
                       device)
              .empty());
    CHECK(format_device(device).find("diagonal") == std::string::npos);
}

TEST_CASE("a tap lands where the real cursor is, on every side") {
    const Side sides[] = {Side::left, Side::right, Side::top, Side::bottom};
    const int angles[] = {0, 90, 180, 270};
    std::mt19937 random(5);
    for (const Side side : sides) {
        for (const bool park : {false, true}) {
            CAPTURE(static_cast<int>(side));
            CAPTURE(park);
            FakeAndroid phone;
            const Size natural{1600, 2560};
            phone.view = phone.logical = natural;
            phone.x = 123.0;
            phone.y = 456.0;
            Portal portal;
            portal.side = side;
            portal.anchor = portal.segment = {200, 900};
            portal.android_length = vertical(side) ? phone.view.h : phone.view.w;
            MotionConfig motion;
            motion.scale_x = motion.scale_y = 1.0;
            motion.park = park;
            Session session(phone);
            session.configure(portal, phone.view, motion,
                              AccelModel(AccelMode::exact, 0, 1.0, 1.0));
            TouchSetup setup;
            setup.enabled = true;
            setup.natural = natural;
            setup.rotation = 0;
            session.set_touch(setup);
                    double now = 1.0;
                    for (int trip = 0; trip < 6; ++trip) {
                        REQUIRE(session.edge_hit(std::uniform_int_distribution<int>(200, 900)(random),
                                                 1.0, false, now));
                        for (int step = 0; step < 15; ++step) {
                            now += 0.004;
                            const double dx = std::uniform_int_distribution<int>(-40, 40)(random);
                            const double dy = std::uniform_int_distribution<int>(-40, 40)(random);
                            if (session.motion(dx, dy, now))
                                break;
                            phone.touches.clear();
                            session.button(1, true);
                            REQUIRE(phone.touches.size() == 1);
                            const TouchPoint real = to_touch_point(phone.view, natural, 0,
                                                                   phone.view_x(), phone.view_y());
                            CHECK(std::abs(phone.touches[0].x - real.x) <= 1);
                            CHECK(std::abs(phone.touches[0].y - real.y) <= 1);
                            session.button(1, false);
                        }
                        while (session.remote()) {
                            now += 0.004;
                            const double push = 30.0;
                            const double dx = side == Side::right ? -push : side == Side::left ? push : 0.0;
                            const double dy = side == Side::bottom ? -push : side == Side::top ? push : 0.0;
                            if (session.motion(dx, dy, now))
                                session.leave();
                        }
                        now += 1.0;
                    }
                }
            }
        }
