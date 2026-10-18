add_executable(hid_share_daemon hid_share_daemon/main.cpp)
target_link_libraries(hid_share_daemon PRIVATE hid_share_daemon_lib)
target_compile_definitions(hid_share_daemon PRIVATE
  HID_SHARE_VERSION="${PROJECT_VERSION}")
hid_share_warnings(hid_share_daemon)
if(WIN32)
  # No console window: the daemon runs in the background.
  set_target_properties(hid_share_daemon PROPERTIES WIN32_EXECUTABLE ON)
  if(MSVC)
    target_link_options(hid_share_daemon PRIVATE /ENTRY:mainCRTStartup)
  else()
    target_link_options(hid_share_daemon PRIVATE -mwindows)
  endif()
endif()

add_executable(hid_share hid_share/main.cpp hid_share/calibrate.cpp)
target_link_libraries(hid_share PRIVATE hid_share_control)
target_compile_definitions(hid_share PRIVATE HID_SHARE_VERSION="${PROJECT_VERSION}")
hid_share_warnings(hid_share)

install(TARGETS hid_share_daemon hid_share RUNTIME DESTINATION bin)

if(HID_SHARE_BUILD_GUI)
  add_subdirectory(hid_share_gui)
endif()
