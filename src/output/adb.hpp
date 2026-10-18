// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace hids {

// The adb program, run for one thing: stopping its server, which holds a
// phone's ADB interface that the proxy is to take.
class Adb {
  public:
    // An empty path looks adb up on PATH.
    explicit Adb(std::string path);

    // True when adb ran and stopped (or found no) server.
    bool kill_server() const;

  private:
    std::string path_;
};

} // namespace hids
