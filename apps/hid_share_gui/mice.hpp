// SPDX-License-Identifier: MIT
//
// The USB mice this computer has, and what each says it is, so that a board
// can be given the same identity. Linux only; elsewhere there are none.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gui {

struct Mouse {
    // The name the kernel gives the input device.
    std::string name;
    uint16_t vendor_id{};
    uint16_t product_id{};
    // The strings the mouse announces; empty when it announces none.
    std::string manufacturer;
    std::string product;
    std::string serial;
};

// Reads the mice that are plugged in now: pointing devices on a USB port,
// once each.
[[nodiscard]] std::vector<Mouse> list_mice();

} // namespace gui
