// SPDX-License-Identifier: MIT
//
// What the daemon reports, every key of it, grouped and explained from the
// status table. A key the table does not know is shown as it is.
#pragma once

#include "hid_share/config.hpp"

namespace gui {

struct App;

// One or two lines on how things stand: where the input is, or whether the
// device is ready and how long its reports take.
void draw_summary(App& app, const hids::DeviceConfig* device);

// The status block for a device, or for the computer when `device` is null.
void draw_status(App& app, const hids::DeviceConfig* device);

// The calls the last reports to a device carried, as the daemon records them.
void draw_reports(App& app, const hids::DeviceConfig& device);

} // namespace gui
