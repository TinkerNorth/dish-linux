# --------------------------------------------------------------------------
# Catch2 v3 (FetchContent fallback so contributors don't need a system pkg)
# --------------------------------------------------------------------------
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    # Catch2's decomposer templates trip -Wnon-virtual-dtor; it's vendored, so mark
    # its headers SYSTEM rather than dropping the warning from our own set.
    foreach(_catch2_target Catch2 Catch2WithMain)
        if(TARGET ${_catch2_target})
            get_target_property(_catch2_includes ${_catch2_target}
                INTERFACE_INCLUDE_DIRECTORIES)
            if(_catch2_includes)
                set_target_properties(${_catch2_target} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_catch2_includes}")
            endif()
        endif()
    endforeach()
endif()

include(Catch)

set(DISH_TEST_SOURCES
    test_atomic_counter.cpp
    test_hex.cpp
    test_endian.cpp
    test_session_crypto.cpp
    test_tofu.cpp
    test_backoff.cpp
    test_close_notify.cpp
    test_reconcile.cpp
    test_latency_window.cpp
    test_models.cpp
    test_beacon_parser.cpp
    test_mdns_discovery.cpp
    test_gamepad_input_processor.cpp
    test_pairing_client_classify.cpp
    test_screen_wake_controller.cpp
    test_freedesktop_screensaver_inhibitor.cpp
    test_satellite_client_rumble.cpp
    test_satellite_client_motion.cpp
    test_satellite_client_touchpad.cpp
    test_satellite_client_lightbar.cpp
    test_satellite_client_send_counter.cpp
    test_wifi_connection_rekey.cpp
    test_wifi_connection_catalog.cpp
    test_output_command_queue.cpp
    test_lightbar_routing.cpp
    test_feature_settings.cpp
    test_sdl_motion_convert.cpp
    test_host_battery.cpp
    test_notification_queue.cpp)

# Qt6::Test gives QSignalSpy + QTest helpers used by test_notification_queue.
find_package(Qt6 6.2 REQUIRED COMPONENTS Test)

add_executable(DishTests ${DISH_TEST_SOURCES})
target_link_libraries(DishTests
    PRIVATE
        dish_core
        Catch2::Catch2WithMain
        Qt6::Test
        dish_warnings)

# Tests don't need -Werror on the third-party header path; keep our own at -Werror.
catch_discover_tests(DishTests)
