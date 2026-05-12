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
endif()

include(Catch)

set(DISH_TEST_SOURCES
    test_atomic_counter.cpp
    test_hex.cpp
    test_endian.cpp
    test_models.cpp
    test_beacon_parser.cpp
    test_gamepad_input_processor.cpp
    test_pairing_client_classify.cpp
    test_screen_wake_controller.cpp)

add_executable(DishTests ${DISH_TEST_SOURCES})
target_link_libraries(DishTests
    PRIVATE
        dish_core
        Catch2::Catch2WithMain
        dish_warnings)

# Tests don't need -Werror on the third-party header path; keep our own at -Werror.
catch_discover_tests(DishTests)
