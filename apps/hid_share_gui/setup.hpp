// SPDX-License-Identifier: MIT
//
// The Setup tab: what is left to do before a device works, in the order to do
// it, with a reason and the next action at each step. Whether a step is done
// is read from the settings and from what the daemon reports; nothing is kept
// here.
#pragma once

#include "hid_share/config.hpp"

#include <string>

namespace gui {

struct App;

// What a board cannot be opened without and has not been given, as a list; empty
// when it has all of it.
[[nodiscard]] std::string board_missing(const hids::DeviceConfig& device);

// Gives the board the identity of a mouse or touchscreen that is plugged in
// here: its ids and everything else it announces.
void draw_copy_mouse(App& app, hids::DeviceConfig& device);

// True when every step of the device is done.
[[nodiscard]] bool setup_complete(const App& app, const hids::DeviceConfig& device);

void draw_setup(App& app, hids::DeviceConfig& device);

// The page of the Computer when no device is set up yet: what to do first.
void draw_getting_started(App& app);

} // namespace gui
