// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "third_party/doctest.h"

#include "adb.hpp"
#include "paths.hpp"
#include "store.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace hids;

namespace {

// A stand-in for adb: a shell script that answers the few commands the
// program sends, from the text given for each.
std::string write_fake_adb(const std::filesystem::path& folder, const std::string& body) {
    std::filesystem::create_directories(folder);
    const std::filesystem::path script = folder / "adb";
    std::ofstream(script) << "#!/bin/sh\n" << body;
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    return script.string();
}

// The settings folder is worked out once per run; whichever test asks first
// fixes it, so each one names the temporary folder itself and checks it got
// that, before anything is removed.
std::filesystem::path test_settings_folder() {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "hid_share_test_settings";
    setenv("HID_SHARE_SETTINGS_DIR", folder.c_str(), 1);
    REQUIRE(settings_dir() == folder.string());
    return folder;
}

} // namespace

// The first test to ask for the settings folder: the answer is kept for the
// whole run.
TEST_CASE("the settings are in one folder, the devices in device/ under it") {
    const std::filesystem::path folder = test_settings_folder();
    std::filesystem::remove_all(folder);
    CHECK(settings_dir() == folder.string());
    CHECK(config_file_path() == (folder / "config.ini").string());
    CHECK(state_file_path() == (folder / "state.ini").string());
    CHECK(devices_dir() == (folder / "device").string());
    CHECK(device_file_path("ABCD1234EFG") == (folder / "device" / "ABCD1234EFG.ini").string());

    // A first load writes config.ini and leaves device/ for the devices.
    Config config;
    CHECK(load_store(config).empty());
    CHECK(config.adb_kill_server);
    CHECK(std::filesystem::exists(folder / "config.ini"));
    DeviceConfig device;
    device.serial = "ABCD1234EFG";
    device.width = 1600;
    device.height = 2560;
    config.devices.push_back(device);
    CHECK(save_store(config));
    CHECK(std::filesystem::exists(folder / "device" / "ABCD1234EFG.ini"));
    Config again;
    CHECK(load_store(again).empty());
    REQUIRE(again.devices.size() == 1);
    CHECK(again.devices[0].width == 1600);
    // A device's file the config no longer holds is removed with it.
    config.devices.clear();
    CHECK(save_store(config));
    CHECK_FALSE(std::filesystem::exists(folder / "device" / "ABCD1234EFG.ini"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("the state text keeps one value per key") {
    std::string text;
    text = state_set(text, "restore_token", "abc");
    text = state_set(text, "accel.R5", "enhance_pointer_precision:1");
    CHECK(state_get(text, "restore_token") == "abc");
    CHECK(state_get(text, "accel.R5") == "enhance_pointer_precision:1");
    text = state_set(text, "restore_token", "def");
    CHECK(state_get(text, "restore_token") == "def");
    text = state_set(text, "accel.R5", "");
    CHECK(state_get(text, "accel.R5").empty());
    CHECK(state_get(text, "missing").empty());
}

TEST_CASE("kill_server says whether adb ran and stopped its server") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "hid_share_test_adb_kill";
    std::filesystem::remove_all(folder);
    const Adb stopped(write_fake_adb(folder, "[ \"$1\" = kill-server ] && exit 0\nexit 1\n"));
    CHECK(stopped.kill_server());
    const Adb failing(write_fake_adb(folder, "exit 1\n"));
    CHECK_FALSE(failing.kill_server());
    const Adb missing((folder / "no_such_adb").string());
    CHECK_FALSE(missing.kill_server());
    std::filesystem::remove_all(folder);
}
