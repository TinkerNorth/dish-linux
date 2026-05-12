cmake_minimum_required(VERSION 3.21)
project(Dish
    VERSION 0.1.0
    DESCRIPTION "Dish Linux client for the Satellite gamepad-over-LAN protocol"
    LANGUAGES CXX)

# --------------------------------------------------------------------------
# Global build settings
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DISH_BUILD_TESTS    "Build the Dish test suite"            ON)
option(DISH_ENABLE_WARNINGS "Treat compiler warnings as errors"   ON)
option(DISH_ENABLE_LTO     "Enable LTO/IPO for Release builds"    ON)

if(DISH_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
    if(_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endif()

# --------------------------------------------------------------------------
# Common warning flags. Two targets:
#   * dish_warnings  — the lint set, applied everywhere (including tests).
#   * dish_strict    — adds -Werror, applied only to first-party code so
#                      Catch2's headers don't fail the build under -Werror.
# --------------------------------------------------------------------------
add_library(dish_warnings INTERFACE)
add_library(dish_strict   INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dish_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion -Wnull-dereference
        -Wdouble-promotion -Wformat=2)
    if(DISH_ENABLE_WARNINGS)
        target_compile_options(dish_strict INTERFACE -Werror)
    endif()
endif()

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network DBus)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SDL2   REQUIRED IMPORTED_TARGET sdl2)

# --------------------------------------------------------------------------
# Core library: everything except main.cpp & UI is reusable for tests
# --------------------------------------------------------------------------
set(DISH_CORE_SOURCES
    src/Util/AtomicCounter.h
    src/Util/Hex.h
    src/Util/Hex.cpp
    src/Util/Endian.h
    src/Util/DisplaySleepInhibitor.h
    src/Util/DisplaySleepInhibitor.cpp
    src/Util/ScreenWakeController.h
    src/Util/ScreenWakeController.cpp
    src/Models/Models.h
    src/Models/Models.cpp
    src/Network/SatelliteClient.h
    src/Network/SatelliteClient.cpp
    src/Network/LANDiscovery.h
    src/Network/LANDiscovery.cpp
    src/Network/PairingClient.h
    src/Network/PairingClient.cpp
    src/Network/HTTPClient.h
    src/Network/HTTPClient.cpp
    src/Network/WifiConnection.h
    src/Network/WifiConnection.cpp
    src/Network/WifiConnectionManager.h
    src/Network/WifiConnectionManager.cpp
    src/Network/ConnectionHub.h
    src/Network/ConnectionHub.cpp
    src/Network/ConnectionStore.h
    src/Network/ConnectionStore.cpp
    src/Input/GamepadInputProcessor.h
    src/Input/GamepadInputProcessor.cpp
    src/Input/SDLGamepadBridge.h
    src/Input/SDLGamepadBridge.cpp
    src/AppModel.h
    src/AppModel.cpp)

add_library(dish_core STATIC ${DISH_CORE_SOURCES})
target_include_directories(dish_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dish_core
    PUBLIC
        Qt6::Core Qt6::Network Qt6::DBus
        PkgConfig::SODIUM
        PkgConfig::SDL2
        Threads::Threads
    PRIVATE
        dish_warnings
        dish_strict)

# --------------------------------------------------------------------------
# UI executable
# --------------------------------------------------------------------------
set(DISH_UI_SOURCES
    src/UI/Theme.h
    src/UI/Theme.cpp
    src/UI/SlotCard.h
    src/UI/SlotCard.cpp
    src/UI/PairingDialog.h
    src/UI/PairingDialog.cpp
    src/UI/ConnectionsDialog.h
    src/UI/ConnectionsDialog.cpp
    src/UI/MainWindow.h
    src/UI/MainWindow.cpp
    src/main.cpp)

add_executable(Dish ${DISH_UI_SOURCES})
set_target_properties(Dish PROPERTIES OUTPUT_NAME dish)
target_link_libraries(Dish PRIVATE dish_core Qt6::Widgets Qt6::Gui dish_warnings dish_strict)

install(TARGETS Dish RUNTIME DESTINATION bin)

# Desktop entry + icon — so the app shows up in app menus with the correct
# launcher icon. Icon=dish in dish.desktop resolves to dish.png in pixmaps.
include(GNUInstallDirs)
install(FILES packaging/dish.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/dish.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps)
install(FILES packaging/dish.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(DISH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
