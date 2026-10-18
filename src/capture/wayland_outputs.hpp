// SPDX-License-Identifier: MIT
#pragma once

#include "hid_share/geometry.hpp"

#include <vector>

namespace hids {

// The compositor's outputs with their logical rectangle, connector name and
// physical size, from wl_output and xdg-output. Empty when there is no
// Wayland display or it lacks xdg-output.
[[nodiscard]] std::vector<Monitor> query_wayland_outputs();

} // namespace hids
