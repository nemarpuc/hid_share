// SPDX-License-Identifier: MIT
// The board path against libmimusb's simulated board (the firmware core, run on this PC, on a
// pseudo-terminal): the Nodes reach the target as reports, and a change of Nodes re-attaches once.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "third_party/doctest.h"

#include "hid_share/clone.hpp"
#include "port.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>

#include <vector>

extern "C" {
#include "fake_board.h"
}

using namespace hids;

namespace {

std::string g_pty;

bool start_board() {
    static const bool started = [] {
        const int master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
            return false;
        const char* name = ptsname(master);
        if (name == nullptr)
            return false;
        g_pty = name;
        termios tio{};
        tcgetattr(master, &tio);
        cfmakeraw(&tio);
        tcsetattr(master, TCSANOW, &tio);
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
        return fake_board_start(master) == 0;
    }();
    return started;
}

// Every value a board needs, as a file would give it.
LinkOptions board_options() {
    LinkOptions options;
    MimDevice& mim = options.mim;
    mim.port = g_pty;
    mim.baud = 3000000;
    mim.handshake_timeout_ms = 500;
    mim.link_timeout_ms = 1000;
    mim.reconnect_timeout_ms = 3000;
    mim.drain_timeout_ms = 1000;
    mim.ready_timeout_ms = 5000;
    mim.linux_hid = {4096, 256, 4, 32, 1024, 65536};
    mim.vendor_id = 0x1234;
    mim.product_id = 0x5678;
    mim.bcd_usb = 0x0200;
    mim.ep0_max_packet_size = 64;
    mim.manufacturer = "m";
    mim.product = "p";
    mim.bcd_device = 0x0100;
    mim.max_power_ma = 100;
    mim.language_id = 0x0409;
    for (NodeUsb& n : mim.node) {
        n.poll_interval_ms = 1;
        n.ep_in_size = 64;
        n.ep_out_size = 0;
        n.bcd_hid = 0x0111;
        n.country_code = 0;
    }
    options.node_usb = mim.node;
    options.touch_size = {1920, 1080};
    return options;
}

// A boot keyboard (HID 1.11 appendix B.1) and an interface with a consumer collection (Report ID 1)
// and a mouse (Report ID 2).
const std::vector<uint8_t> boot_keyboard = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01, 0x05, 0x07,
    0x19, 0x00, 0x2A, 0xFF, 0x00, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81,
    0x00, 0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x91, 0x02,
    0x95, 0x05, 0x91, 0x01, 0xC0};
const std::vector<uint8_t> consumer_and_mouse = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x0C, 0x19, 0x00, 0x2A, 0xFF, 0x0F,
    0x15, 0x00, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09,
    0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0xA1, 0x02, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x05, 0x01, 0x09,
    0x30, 0x09, 0x31, 0x15, 0x80, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38,
    0x15, 0x80, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xA1, 0x02, 0x05, 0x0C,
    0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
    0xC0};

CloneSnapshot clone_snapshot() {
    CloneSnapshot snapshot;
    snapshot.source = "test device";
    const std::vector<uint8_t>* descriptors[] = {&boot_keyboard, &consumer_and_mouse};
    for (uint8_t i = 0; i < 2; ++i) {
        CloneInterface itf;
        itf.number = i;
        itf.bcd_hid = 0x0111;
        itf.ep_in_size = 64;
        itf.interval_ms = 1;
        itf.descriptor = *descriptors[i];
        snapshot.device.interfaces.push_back(itf);
    }
    snapshot.device.interfaces[0].boot_subclass = 1;
    snapshot.device.interfaces[0].boot_protocol = 1;
    snapshot.device.vendor_id = 0x1038;
    snapshot.device.product_id = 0x1642;
    return snapshot;
}

std::vector<uint8_t> next(const uint8_t address) {
    fake_transfer_t transfer{};
    if (fake_board_wait_transfer(address, &transfer, 2000) == 0)
        return {};
    return {transfer.data, transfer.data + transfer.length};
}

bool any_set(const std::vector<uint8_t>& bytes) {
    for (const uint8_t b : bytes)
        if (b != 0)
            return true;
    return false;
}

} // namespace

TEST_CASE("the four Nodes reach the target as reports") {
    REQUIRE(start_board());
    const std::unique_ptr<Backend> backend = make_mim_backend();
    REQUIRE(backend->start().empty());
    ScanRequest request;
    request.board_ports = {g_pty};
    std::string error;
    REQUIRE(backend->scan(request, error).size() == 1);

    LinkOptions options = board_options();
    options.touch = true;
    std::unique_ptr<Port> port = backend->open(0, 0, options, error);
    INFO(error);
    REQUIRE(port != nullptr);
    CHECK(fake_board_mounted() == 1);
    CHECK(fake_board_interface_count() == 4);
    for (const NodeKind kind : {node_mouse, node_keyboard, node_media, node_touch})
        CHECK(port->has(kind));

    fake_board_clear_transfers();
    REQUIRE(port->mouse_move(5, -3));
    REQUIRE(port->flush(node_bit(node_mouse)));
    // Buttons, X and Y as 16-bit values, wheel, pan: no Report ID.
    CHECK(next(0x81) == std::vector<uint8_t>{0x00, 0x05, 0x00, 0xFD, 0xFF, 0x00, 0x00});

    REQUIRE(port->key(0x04, true));
    REQUIRE(port->flush(node_bit(node_keyboard)));
    CHECK(any_set(next(0x82)));

    REQUIRE(port->media(0xCD, true));
    REQUIRE(port->flush(node_bit(node_media)));
    CHECK(any_set(next(0x83)));

    REQUIRE(port->touch(0, true, 100, 200));
    REQUIRE(port->flush(node_bit(node_touch)));
    CHECK(any_set(next(0x84)));

    port->close();
    backend->stop();
}

TEST_CASE("removing a Node re-attaches the board once, and leaving the set alone does not") {
    REQUIRE(start_board());
    const std::unique_ptr<Backend> backend = make_mim_backend();
    REQUIRE(backend->start().empty());
    ScanRequest request;
    request.board_ports = {g_pty};
    std::string error;
    REQUIRE(backend->scan(request, error).size() == 1);
    LinkOptions options = board_options();
    std::unique_ptr<Port> port = backend->open(0, 0, options, error);
    INFO(error);
    REQUIRE(port != nullptr);

    uint8_t added = 0;
    const unsigned before = fake_board_connects();
    REQUIRE(port->set_nodes(options, added));
    CHECK(fake_board_connects() == before);

    options.touch = false;
    options.media = false;
    REQUIRE(port->set_nodes(options, added));
    CHECK(fake_board_connects() == before + 1);
    CHECK(fake_board_interface_count() == 2);
    CHECK(added == (node_bit(node_mouse) | node_bit(node_keyboard)));

    port->close();
    backend->stop();
}

TEST_CASE("a clone presents the plugged-in device's descriptors and carries its reports") {
    REQUIRE(start_board());
    const std::unique_ptr<Backend> backend = make_mim_backend();
    REQUIRE(backend->start().empty());
    ScanRequest request;
    request.board_ports = {g_pty};
    std::string error;
    REQUIRE(backend->scan(request, error).size() == 1);

    LinkOptions options = board_options();
    options.clone = clone_snapshot();
    std::unique_ptr<Port> port = backend->open(0, 0, options, error);
    INFO(error);
    REQUIRE(port != nullptr);
    CHECK(fake_board_interface_count() == 2);
    uint16_t length = 0;
    const uint8_t* descriptor = fake_board_report_descriptor(0, &length);
    REQUIRE(descriptor != nullptr);
    CHECK(std::vector<uint8_t>(descriptor, descriptor + length) == boot_keyboard);

    fake_board_clear_transfers();
    REQUIRE(port->key(0x04, true));
    REQUIRE(port->flush(node_bit(node_keyboard)));
    // Modifiers, a reserved byte, and the six key array.
    CHECK(next(0x81) == std::vector<uint8_t>{0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00});

    REQUIRE(port->mouse_move(5, -3));
    REQUIRE(port->flush(node_bit(node_mouse)));
    // Report ID 2, buttons, X, Y, wheel, pan.
    CHECK(next(0x82) == std::vector<uint8_t>{0x02, 0x00, 0x05, 0xFD, 0x00, 0x00});

    port->close();
    backend->stop();
}
