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

// The board presents a plugged-in device as its own: pick one, read it again, remove it,
// and what the board will show of it.
void draw_clone(App& app, hids::DeviceConfig& device);

// True when the device is connected: every step of its Setup is done.
[[nodiscard]] bool setup_complete(const App& app, const hids::DeviceConfig& device);

// Whether something is still to be set for the device (its link, a board's
// identity, the screen's size and gain). Whether it is plugged in does not
// count: that is shown by its mark, and Connect is on the Tools tab.
[[nodiscard]] bool needs_setup(const hids::DeviceConfig& device);

// The state a device's mark in the list shows: the daemon's, except that a
// device that was asked for but could not be opened, or a board that lacks
// what it cannot be opened without, is an "error" ([!]).
[[nodiscard]] std::string mark_state(const App& app, const hids::DeviceConfig& device);
void draw_setup(App& app, hids::DeviceConfig& device);

// The phones and boards the last search found that have no file yet, each with
// a button that adds it and opens its Setup.
void draw_new_devices(App& app);

// The page of the Computer when no device is set up yet: what to do first.
void draw_getting_started(App& app);

} // namespace gui
