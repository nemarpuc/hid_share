// SPDX-License-Identifier: MIT
//
// Where a device sits: the picture to drag it in, and the few controls that
// say the same in words.
#pragma once

#include "hid_share/config.hpp"

namespace gui {

struct App;

void draw_placement(App& app, hids::DeviceConfig& device);

} // namespace gui
