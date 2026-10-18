// SPDX-License-Identifier: MIT
#include "adb.hpp"

#include "process.hpp"

namespace hids {
namespace {

constexpr int command_timeout_ms = 4000;

} // namespace

Adb::Adb(std::string path) : path_(path.empty() ? std::string("adb") : std::move(path)) {}

bool Adb::kill_server() const {
    const RunResult result = run_process({path_, "kill-server"}, command_timeout_ms);
    return result.started && result.exit_code == 0;
}

} // namespace hids
