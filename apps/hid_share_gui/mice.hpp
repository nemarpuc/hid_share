// SPDX-License-Identifier: MIT
//
// The USB mice and touchscreens this computer has, and what each says it is,
// so that a board can be given the same identity. Linux only; elsewhere there are none.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gui {

enum class PointerKind { mouse, touchscreen };

struct Mouse {
    PointerKind kind{PointerKind::mouse};
    // The name the kernel gives the input device.
    std::string name;
    uint16_t vendor_id{};
    uint16_t product_id{};
    // The strings the mouse announces; empty when it announces none.
    std::string manufacturer;
    std::string product;
    std::string serial;
    std::string configuration;
    // Zero when the device does not say.
    uint16_t bcd_device{};
    uint16_t max_power_ma{};
    bool self_powered{};
    bool remote_wakeup{};
};

// Reads the mice and touchscreens that are plugged in now: pointing devices on
// a USB port, once each.
[[nodiscard]] std::vector<Mouse> list_mice();

} // namespace gui
