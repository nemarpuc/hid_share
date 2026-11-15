add_executable(hid_share_tests test_core.cpp)
target_include_directories(hid_share_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hid_share_tests PRIVATE hid_share_core)
add_test(NAME hid_share.core COMMAND hid_share_tests)

# The settings window's key tables and the ini text they are edited through.
add_executable(hid_share_keys_tests test_keys.cpp)
target_include_directories(hid_share_keys_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hid_share_keys_tests PRIVATE hid_share_core)
target_compile_definitions(hid_share_keys_tests PRIVATE
  HID_SHARE_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
add_test(NAME hid_share.keys COMMAND hid_share_keys_tests)

# Clone mode: report descriptors, report building and the plugged-in device reader.
add_executable(hid_share_clone_tests test_clone.cpp)
target_include_directories(hid_share_clone_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hid_share_clone_tests PRIVATE hid_share_core)
add_test(NAME hid_share.clone COMMAND hid_share_clone_tests)

# The adb program, and the settings folder and state file. This one links libaoahid, whose
# DLL is not next to the test on Windows, so it is built elsewhere only.
if(NOT WIN32)
  add_executable(hid_share_output_tests test_output.cpp)
  target_include_directories(hid_share_output_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(hid_share_output_tests PRIVATE hid_share_output
                                                          hid_share_control)
  add_test(NAME hid_share.output COMMAND hid_share_output_tests)
endif()

# The board path against libmimusb's simulated board: the real firmware core built for this PC,
# on a pseudo-terminal. It needs libmimusb's source tree and a TinyUSB checkout, named here;
# the test is left out without them.
set(HID_SHARE_MIMUSB_SOURCE_DIR "" CACHE PATH "libmimusb source tree, for the board test")
set(HID_SHARE_TINYUSB_DIR "" CACHE PATH "TinyUSB checkout, for the board test")
if(NOT WIN32 AND HID_SHARE_MIMUSB_SOURCE_DIR AND HID_SHARE_TINYUSB_DIR)
  set(m ${HID_SHARE_MIMUSB_SOURCE_DIR})
  add_library(hid_share_fake_board STATIC
    ${m}/tests/sim/fake_board.c ${m}/tests/sim/pty_baud.c
    ${m}/firmware/common/core.c ${m}/firmware/common/control.c ${m}/firmware/common/ep0.c
    ${m}/firmware/common/usb_descriptors.c ${m}/firmware/common/generic.c
    ${m}/firmware/common/tx.c)
  target_include_directories(hid_share_fake_board
    PUBLIC ${m}/tests/sim ${m}/firmware/ports/nrf52840/limits
    PRIVATE ${m}/firmware/tests ${m}/firmware/common ${m}/proto ${HID_SHARE_TINYUSB_DIR}/src)
  # The limits of a real port (nRF52840: endpoint numbers 1 to 7, 64-byte packets), full speed.
  target_compile_definitions(hid_share_fake_board PRIVATE
    SIM_ENDPOINTS=7 MIMUSB_MAX_ENDPOINT_NUMBER=7 MIMUSB_MAX_ENDPOINT_BYTES=64
    CFG_TUSB_OS=OPT_OS_NONE
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
    CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
    CFG_TUSB_CONFIG_FILE="tusb_config.h")
  target_link_libraries(hid_share_fake_board PUBLIC Threads::Threads)

  add_executable(hid_share_board_tests test_board.cpp)
  target_include_directories(hid_share_board_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(hid_share_board_tests PRIVATE hid_share_output hid_share_fake_board)
  add_test(NAME hid_share.board COMMAND hid_share_board_tests)
else()
  message(STATUS "hid_share.board is off: set HID_SHARE_MIMUSB_SOURCE_DIR and HID_SHARE_TINYUSB_DIR")
endif()
