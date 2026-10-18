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

# The adb program, and the settings folder and state file. This one links libaoahid, whose
# DLL is not next to the test on Windows, so it is built elsewhere only.
if(NOT WIN32)
  add_executable(hid_share_output_tests test_output.cpp)
  target_include_directories(hid_share_output_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(hid_share_output_tests PRIVATE hid_share_output
                                                          hid_share_control)
  add_test(NAME hid_share.output COMMAND hid_share_output_tests)
endif()
