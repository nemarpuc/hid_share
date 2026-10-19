// SPDX-License-Identifier: MIT
#pragma once

#include "hid_share/config.hpp"
#include "hid_share/geometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hids {

// How a target is reached: a phone over Android Open Accessory (libaoahid), or
// a microcontroller board on a serial port (libmimhid).
enum class Transport : uint8_t { aoa, mim };

struct TargetInfo {
    Transport transport{Transport::aoa};
    // What the target is called in the config: the USB serial of a phone.
    std::string serial;
    std::string product;
    uint16_t vendor_id{};
    uint16_t product_id{};
    // Where it is reached, when the serial does not say: a board's serial
    // port. Empty for a phone.
    std::string detail;
    // The fastest line rate the search saw a board and its serial adapter
    // carry; 0 when it was not measured.
    uint32_t baud{};
};

// What a board (MIM) announces to its target and how its serial link is run.
// Every value is the user's; none has a fallback, so open() names the ones
// that are missing.
struct MimOptions {
    uint32_t baud{};
    uint16_t vendor_id{};
    uint16_t product_id{};
    // At most 31 printable ASCII characters each.
    std::string manufacturer;
    std::string product;
    // Empty leaves the serial number out unless serial_from_chip_id is set.
    std::string serial;
    bool serial_from_chip_id{};
    // 0: the constants of the MIM backend.
    uint16_t bcd_device{};
    uint16_t max_power_ma{};
    // 0: the constant of the MIM backend.
    uint16_t language_id{};
    bool self_powered{};
    bool remote_wakeup{};
    std::string configuration;

    [[nodiscard]] bool operator==(const MimOptions&) const = default;
};

// `buttons` is fixed when the target is opened; the three Nodes can be added
// and removed on an open target (Hub::set_nodes).
struct LinkOptions {
    unsigned buttons{5};
    bool mouse{true};
    bool keyboard{true};
    bool media{true};
    // The touchscreen Node, built for the device's natural size.
    bool touch{};
    Size touch_size{};
    // The touchscreen's descriptor values; those left out take the link's own.
    TouchHid touch_hid;
    // Each Node's USB interface values, in NodeKind order; a board only.
    std::array<NodeUsb, usb_nodes> node_usb;
    // Used by a board only.
    MimOptions mim;
    // A board only: the plugged-in device it presents instead of the Nodes above (those
    // switches then say which of its parts are presented; touch is not available).
    std::optional<CloneSnapshot> clone;
};

// What a scan may look at. A board is found by sending a message to every
// serial port that could be one, which can upset other equipment on them and
// the board that is already open, so it happens only as the daemon starts and
// when the user asks.
struct ScanRequest {
    bool probe_boards{};
    // Serial ports the user named; they are listed without being probed.
    std::vector<std::string> board_ports;
};

// The inputs a target is given, in the order the link keeps them.
enum NodeKind : uint8_t { node_mouse, node_keyboard, node_media, node_touch };
inline constexpr size_t node_kinds = 4;
// A set of NodeKind, one bit each.
[[nodiscard]] constexpr uint8_t node_bit(const NodeKind kind) noexcept {
    return static_cast<uint8_t>(1U << kind);
}

// The smallest bit width that holds 0..maximum.
[[nodiscard]] constexpr uint32_t bits_to_hold(const int64_t maximum) noexcept {
    uint32_t bits = 1;
    while (bits < 32 && (int64_t{1} << bits) - 1 < maximum)
        ++bits;
    return bits;
}

// What Hub::start_proxy returns when a target has no ADB interface to serve or
// is not open. The same value as the proxy library's ERR_ARGUMENT.
inline constexpr int proxy_refused = -1;

// One open target. Every call is made by one thread at a time, with the
// link's lock held; a Port keeps no lock of its own. A call that returns
// false has failed, and error() says why.
class Port {
  public:
    Port() = default;
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;
    virtual ~Port() = default;

    [[nodiscard]] virtual bool has(NodeKind kind) const noexcept = 0;

    [[nodiscard]] virtual bool mouse_move(int32_t dx, int32_t dy) = 0;
    [[nodiscard]] virtual bool mouse_scroll(int32_t wheel, int32_t pan) = 0;
    [[nodiscard]] virtual bool button(unsigned button, bool down) = 0;
    [[nodiscard]] virtual bool key(uint16_t usage, bool down) = 0;
    [[nodiscard]] virtual bool media(uint16_t usage, bool down) = 0;
    [[nodiscard]] virtual bool touch(unsigned contact, bool down, int32_t x, int32_t y) = 0;
    // Sends the reports the calls above set on the Nodes in `nodes` (a set of
    // node_bit), all queued before any is waited on, and returns once they are
    // out.
    [[nodiscard]] virtual bool flush(uint8_t nodes) = 0;

    // Registers the Nodes `options` names and unregisters the others. `added`
    // gets the set of Nodes registered now (a touch Node rebuilt for another
    // size counts).
    [[nodiscard]] virtual bool set_nodes(const LinkOptions& options, uint8_t& added) = 0;
    // How long a Node added to an open target waits before it takes input.
    [[nodiscard]] virtual int64_t settle_ns() const noexcept = 0;

    // Serves the target's ADB interface on 127.0.0.1:port. Returns the proxy's
    // code (0 on success); held is set when another program holds the
    // interface, and absent when the target exposes none.
    [[nodiscard]] virtual int start_proxy(uint16_t port, bool& held, bool& absent) = 0;
    virtual void stop_proxy() = 0;

    // Neutral reports, then the unregistration. The Port is not used after.
    virtual void close() = 0;

    [[nodiscard]] virtual std::string error() const = 0;

    // False when the link has failed without a call failing (a serial port
    // that was unplugged), with the reason in error().
    [[nodiscard]] virtual bool healthy() { return true; }
};

// One way of reaching targets: finds them, and opens one.
class Backend {
  public:
    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    virtual ~Backend() = default;

    // What its notes in a scan are headed with.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Empty on success.
    [[nodiscard]] virtual std::string start() = 0;
    // Every Port it opened is closed by now.
    virtual void stop() = 0;
    // Lists the targets. Not called while one has the input.
    [[nodiscard]] virtual std::vector<TargetInfo> scan(const ScanRequest& request,
                                                       std::string& error) = 0;
    // Opens the target at `index` of the last scan() on link slot `slot`, with
    // its Nodes registered as `options` says. Null, with the reason in
    // error, on failure.
    [[nodiscard]] virtual std::unique_ptr<Port> open(size_t index, int slot,
                                                     const LinkOptions& options,
                                                     std::string& error) = 0;
};

[[nodiscard]] std::unique_ptr<Backend> make_aoa_backend();
[[nodiscard]] std::unique_ptr<Backend> make_mim_backend();

} // namespace hids
