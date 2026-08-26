cmake_minimum_required(VERSION 3.21)
project(Dish
    VERSION 0.2.0
    DESCRIPTION "Dish Linux client for the Satellite gamepad-over-LAN protocol"
    # C for the vendored ENet fork (third_party/enet), the transport under the
    # Moonlight-host control stream.
    LANGUAGES C CXX)

# --------------------------------------------------------------------------
# Global build settings
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DISH_BUILD_TESTS    "Build the Dish test suite"            ON)
option(DISH_ENABLE_WARNINGS "Treat compiler warnings as errors"   ON)
option(DISH_ENABLE_LTO     "Enable LTO/IPO for Release builds"    ON)
option(DISH_REQUIRE_TRANSLATIONS
    "Fail configuration when Qt Linguist tools are missing"       OFF)
set(DISH_SANITIZER "" CACHE STRING
    "Sanitizer to build with: address+undefined, thread, or empty")

# udev scans only /etc/udev/rules.d, /run/udev/rules.d and (/usr)/lib/udev/rules.d.
# NOT ${CMAKE_INSTALL_SYSCONFDIR}: the relative form stays "etc" even where
# GNUInstallDirs maps the FULL form to /etc, so a DESTINATION built from it
# lands in /usr/etc/udev/rules.d under the usual -DCMAKE_INSTALL_PREFIX=/usr.
# Not CMAKE_INSTALL_LIBDIR either — Debian multiarch turns that into
# lib/x86_64-linux-gnu, which udev does not scan.
#
# The default has to depend on the prefix, because a RELATIVE path is only
# correct under the packaging prefix. "lib/udev/rules.d" resolves to
# /usr/lib/udev/rules.d under -DCMAKE_INSTALL_PREFIX=/usr, which is what the .deb
# and .rpm build with and what udev scans. Under CMake's DEFAULT prefix it
# resolves to /usr/local/lib/udev/rules.d, which udev does not scan — so a plain
# `sudo cmake --install` silently laid the rule down somewhere inert and every
# USB-direct claim failed PermissionDenied afterwards.
#
# Off the packaging prefix, use the one directory that is correct from any
# prefix: /etc/udev/rules.d, absolute so it is not joined onto the prefix again.
# It is also the path README tells people to install the rule into by hand.
if(CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    set(_dish_udev_rules_default "lib/udev/rules.d")
else()
    set(_dish_udev_rules_default "/etc/udev/rules.d")
endif()
set(DISH_UDEV_RULES_DIR "${_dish_udev_rules_default}" CACHE STRING
    "Directory the hidraw udev rule is installed into; a relative path is joined onto the prefix")
# A Flatpak cannot own a host rules directory and an AppImage installs nothing
# outside itself; both turn this off and degrade to the SDL path.
option(DISH_INSTALL_UDEV_RULES "Install the hidraw udev rule" ON)

if(DISH_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
    if(_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endif()

# Global rather than per-target: a sanitizer that instruments only some
# translation units reports the other half as false positives.
if(DISH_SANITIZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    string(REPLACE "+" "," _dish_san "${DISH_SANITIZER}")
    add_compile_options(-fsanitize=${_dish_san} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${_dish_san})
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
        -Wdouble-promotion -Wformat=2
        # Qt's qC{Debug,Info,Warning,Critical} logging macros are declared
        # as `(category, ...)`. The streaming form `qCInfo(cat) << "msg"`
        # passes zero variadic args, which -Wpedantic flags as
        # gnu-zero-variadic-macro-arguments (an error under clang-tidy's
        # frontend). The streaming form is the standard Qt idiom; suppress.
        -Wno-gnu-zero-variadic-macro-arguments)
    if(DISH_ENABLE_WARNINGS)
        target_compile_options(dish_strict INTERFACE -Werror)
    endif()
endif()

# --------------------------------------------------------------------------
# Hardening. Spelled out rather than inherited from the distro's GCC specs:
# Ubuntu patches PIE, partial RELRO, -fstack-protector-strong and
# _FORTIFY_SOURCE into its defaults, so a runner build looks hardened by
# accident while the same source on Arch or a from-source toolchain ships none
# of it. Full RELRO and a non-executable stack are nobody's default.
# --------------------------------------------------------------------------
add_library(dish_hardened INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dish_hardened INTERFACE
        -fstack-protector-strong
        # _FORTIFY_SOURCE needs an optimising build; at -O0 it is a warning,
        # and a sanitizer run interposes the checked libc calls itself.
        # Undefined before defined: Ubuntu's specs bake in =2, and redefining
        # to a different value is a warning -Werror turns fatal on any
        # toolchain whose built-in has not moved to =3 (jammy's gcc-11).
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<STREQUAL:${DISH_SANITIZER},>>:-U_FORTIFY_SOURCE>
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<STREQUAL:${DISH_SANITIZER},>>:-D_FORTIFY_SOURCE=3>)
    target_link_options(dish_hardened INTERFACE
        -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Network DBus Svg Quick Qml QuickControls2 Concurrent)
qt_standard_project_setup(REQUIRES 6.7)
# Optional: without qttools the .ts → .qm step is skipped and every qsTr()
# falls back to its English source string, which still builds a working app.
find_package(Qt6 6.7 QUIET OPTIONAL_COMPONENTS LinguistTools)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SDL2   REQUIRED IMPORTED_TARGET sdl2)
# OpenSSL's libcrypto backs the Moonlight-host pairing (AES-128, RSA, self-
# signed X.509) and the control-stream AES-GCM, none of which libsodium offers.
# Only libcrypto is used; the TLS client itself is Qt Network's.
find_package(OpenSSL REQUIRED)

# The vendored ENet fork (MIT), the transport Moonlight hosts speak on the
# control stream. See THIRD_PARTY.md.
add_subdirectory(third_party/enet)


# --------------------------------------------------------------------------
# Core library: everything except main.cpp and the QML entry point, so the
# tests can link it.
# --------------------------------------------------------------------------
set(DISH_CORE_SOURCES
    src/architecture/Observable.h
    src/architecture/Combiner.h
    src/architecture/Composer.h
    src/architecture/StateSource.h
    src/architecture/Controller.h
    src/architecture/Repository.h
    src/core/AsyncState.h
    src/core/wire/SessionCrypto.h
    src/core/wire/SessionCrypto.cpp
    # Moonlight (Sunshine/Apollo/Wolf) host protocol: the Qt-free core.
    src/core/moonlight/MoonlightProtocol.h
    src/core/moonlight/MoonlightWire.h
    src/core/moonlight/MoonlightWire.cpp
    src/core/moonlight/MoonlightControlCipher.h
    src/core/moonlight/MoonlightControlCipher.cpp
    src/core/moonlight/MoonlightPairingCrypto.h
    src/core/moonlight/MoonlightPairingCrypto.cpp
    src/core/moonlight/MoonlightPairing.h
    src/core/moonlight/MoonlightPairing.cpp
    src/core/moonlight/MoonlightXml.h
    src/core/moonlight/MoonlightXml.cpp
    src/core/moonlight/MoonlightRtsp.h
    src/core/moonlight/MoonlightRtsp.cpp
    src/core/moonlight/MoonlightSessionMachine.h
    src/core/moonlight/MoonlightButtonMap.h
    src/core/model/Protocol.h
    src/core/model/IdentityKey.h
    src/core/net/Tofu.h
    src/core/net/Tofu.cpp
    src/core/net/IpLiterals.h
    src/core/net/IpLiterals.cpp
    src/core/update/UpdateVersion.h
    src/core/update/UpdateVersion.cpp
    src/core/update/UpdateManifest.h
    src/core/update/UpdateManifest.cpp
    src/core/reducer/RestOutcome.h
    src/core/reducer/Backoff.h
    src/core/reducer/BackgroundMode.h
    src/core/reducer/KeepAwake.h
    src/core/reducer/Reconcile.h
    src/core/reducer/CloseNotify.h
    src/core/reducer/PathChoice.h
    src/core/reducer/DirectClaimFailure.h
    src/core/reducer/UsbPollRate.h
    src/core/reducer/PollRateSampler.h
    src/core/reducer/UsbPathMachine.h
    src/core/reducer/UsbPathMachine.cpp
    src/core/reducer/UsbTwinDedup.h
    src/core/reducer/SatelliteLinkState.h
    src/core/reducer/SleepCycle.h
    src/core/reducer/TrayPresentation.h
    src/core/reducer/ConnectionRows.h
    src/core/reducer/CarriedPads.h
    src/core/reducer/LateSlotConverge.h
    src/core/reducer/ResendPacer.h
    src/core/reducer/BindingPresence.h
    src/core/reducer/PickerVisibility.h
    src/core/reducer/FoundVisibility.h
    src/core/reducer/RumbleRouting.h
    src/core/reducer/BatteryRouting.h
    src/core/reducer/BatteryUi.h
    src/core/reducer/TouchpadRouting.h
    src/core/reducer/TouchpadModeResolve.h
    src/core/reducer/InputRateTracker.h
    src/core/reducer/MotionIndicatorState.h
    src/core/reducer/LatencyWindow.h
    src/core/reducer/CapabilitySolver.h
    src/core/reducer/CapabilitySolver.cpp
    src/core/reducer/ApplyBindingMachine.h
    src/core/reducer/CatalogFeatureGate.h
    src/core/reducer/CatalogPrewarm.h
    src/core/reducer/ConfigBlocker.h
    src/core/reducer/EmulateSeed.h
    src/core/reducer/PairingMachine.h
    src/core/reducer/ReversePairing.h
    src/core/reducer/SatelliteSessionMachine.h
    src/core/reducer/SlotPathFields.h
    src/core/reducer/UpdateMachine.h
    src/core/reducer/UpdateMachine.cpp
    src/core/catalog/BundledCatalog.h
    src/core/catalog/LegacyCatalogTranslator.h
    src/core/input/GamepadButtonLayouts.h
    src/core/input/CaptureMode.h
    src/core/input/Deadzones.h
    src/core/input/PhysicalMotionSource.h
    # The hidraw bus lookup behind the Bluetooth-vs-USB question.
    src/core/input/HidTransport.h
    # Byte layouts mirrored 1:1 from dish-android's usb_parsers.cpp /
    # usb_hid_descriptor.cpp; keep the two in step when either side gains a
    # model.
    src/core/input/UsbReportParsers.h
    src/core/input/UsbHidLayout.h
    src/Util/AtomicCounter.h
    src/Util/Hex.h
    src/Util/Hex.cpp
    src/Util/Endian.h
    src/Util/HostBattery.h
    src/Util/HostBattery.cpp
    src/Util/UnixSignalWatcher.h
    src/Util/UnixSignalWatcher.cpp
    # In dish_core so test_translations exercises the real locale-fallback path
    # rather than a copy of it.
    src/Util/Localization.h
    src/Util/Localization.cpp
    src/Models/Models.h
    src/Models/Models.cpp
    src/Network/SatelliteClient.h
    src/Network/SatelliteClient.cpp
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
    src/source/connection/LANDiscovery.h
    src/source/connection/LANDiscovery.cpp
    src/source/connection/MdnsDiscovery.h
    src/source/connection/MdnsDiscovery.cpp
    src/source/connection/DiscoveryGateway.h
    src/source/connection/DiscoveryGateway.cpp
    # Moonlight-host IO edge: HTTP/RTSP/ENet transports, discovery, pairing,
    # the session coordinator and the subsystem manager.
    src/source/moonlight/MoonlightLog.h
    src/source/moonlight/MoonlightLog.cpp
    src/source/moonlight/MoonlightHttp.h
    src/source/moonlight/MoonlightHttp.cpp
    src/source/moonlight/MoonlightRtspClient.h
    src/source/moonlight/MoonlightRtspClient.cpp
    src/source/moonlight/MoonlightControlStream.h
    src/source/moonlight/MoonlightControlStream.cpp
    src/source/moonlight/MoonlightDiscovery.h
    src/source/moonlight/MoonlightDiscovery.cpp
    src/source/moonlight/MoonlightPairingFlow.h
    src/source/moonlight/MoonlightPairingFlow.cpp
    src/source/moonlight/MoonlightSession.h
    src/source/moonlight/MoonlightSession.cpp
    src/source/moonlight/MoonlightManager.h
    src/source/moonlight/MoonlightManager.cpp
    src/source/http/SatelliteTlsVerifier.h
    src/source/http/SatelliteTlsVerifier.cpp
    # An ETag cache over HTTPClient::getCatalog, not a durable Repository,
    # despite the name.
    src/source/http/SatelliteCatalogRepository.h
    src/source/http/SatelliteCatalogRepository.cpp
    src/source/store/ControllerTypeStore.h
    src/source/store/ControllerTypeStore.cpp
    src/source/store/MotionEnabledStore.h
    src/source/store/MotionEnabledStore.cpp
    src/source/store/SatelliteMotionBackendStatusStore.h
    src/source/store/SatelliteMotionBackendStatusStore.cpp
    src/source/store/UsbPathPreferenceStore.h
    src/source/store/UsbPathPreferenceStore.cpp
    # Keyed under "joystick_remaps", deliberately disjoint from the
    # "usb_path_choices" key above.
    src/source/store/JoystickRemapStore.h
    src/source/store/JoystickRemapStore.cpp
    src/source/store/OnboardingPreferenceStore.h
    src/source/store/OnboardingPreferenceStore.cpp
    src/source/store/ThemePreferenceStore.h
    src/source/store/ThemePreferenceStore.cpp
    src/source/store/CrashReportingStore.h
    src/source/store/CrashReportingStore.cpp
    src/source/store/TouchpadModeStore.h
    src/source/store/UiPreferenceStore.h
    src/source/store/BackgroundPreferenceStore.h
    src/source/store/KeepAwakePreferenceStore.h
    src/source/store/UpdatePreferenceStore.h
    # xpad publishes no hidraw node for Xbox pads, so this path serves
    # DualSense/DS4/Switch Pro/8BitDo and Xbox pads stay on SDL.
    src/source/usb/UsbDeviceGateway.h
    src/source/usb/HidrawGateway.h
    src/source/usb/HidrawGateway.cpp
    src/source/usb/UsbGamepadManager.h
    src/source/usb/UsbGamepadManager.cpp
    src/source/notification/EventChannel.h
    src/source/notification/DishNotifications.h
    src/source/notification/DishNotifications.cpp
    src/source/notification/DesktopNotifier.h
    src/source/notification/DesktopNotifier.cpp
    src/source/inputrate/SlotInputRates.h
    src/source/input/ControllerActivitySource.h
    src/source/inputrate/InputRateStore.h
    src/source/inputrate/InputRateStore.cpp
    src/source/system/BluetoothRadioProbe.h
    src/source/system/BluetoothRadioProbe.cpp
    src/source/system/SecretServiceStore.h
    src/source/system/SleepMonitor.h
    src/source/system/WakeInhibitor.h
    src/source/system/SecretServiceStore.cpp
    src/source/system/SleepMonitor.cpp
    src/source/system/WakeInhibitor.cpp
    src/source/tray/TrayIcon.h
    src/source/tray/SniIcon.h
    src/source/tray/SniIcon.cpp
    src/source/tray/StatusNotifierTrayIcon.h
    src/source/tray/StatusNotifierTrayIcon.cpp
    # The tray's IconPixmap PNGs ride dish_core so DishTests and the exe both
    # carry them; SniIcon.cpp's Q_INIT_RESOURCE keeps the archive member alive.
    resources/tray.qrc
    # Update checking. In dish_core (not the exe) because the tests link this
    # library, not the exe.
    src/update/UpdatePorts.h
    src/update/HttpGateways.h
    src/update/HttpGateways.cpp
    src/update/UpdateChecker.h
    src/update/UpdateChecker.cpp
    src/UI/CrashReport.h
    src/repository/AppSettings.h
    src/repository/SettingsKeys.h
    src/repository/RememberedWifiEq.h
    src/repository/SatellitePinRepository.h
    src/repository/SatellitePinRepository.cpp
    src/repository/SatelliteSharedKeyRepository.h
    src/repository/SatelliteSharedKeyRepository.cpp
    src/repository/RememberedSatelliteRepository.h
    src/repository/RememberedSatelliteRepository.cpp
    src/repository/MoonlightIdentityRepository.h
    src/repository/MoonlightIdentityRepository.cpp
    src/repository/MoonlightHostRepository.h
    src/repository/MoonlightHostRepository.cpp
    src/repository/ConnectionStore.h
    src/UI/CrashReport.cpp
    src/repository/AppSettings.cpp
    src/repository/ConnectionStore.cpp
    src/repository/DeadzoneRepository.h
    src/repository/DeadzoneRepository.cpp
    src/repository/MotionPreferenceRepository.h
    src/repository/MotionPreferenceRepository.cpp
    src/repository/TouchpadModeRepository.h
    src/composer/ConnectionsComposer.h
    src/composer/ConnectionsComposer.cpp
    src/composer/ConnectionCoordinator.h
    src/composer/ConnectionCoordinator.cpp
    src/composer/BackgroundCoordinator.h
    src/composer/BackgroundCoordinator.cpp
    src/composer/SleepCoordinator.h
    src/composer/SleepCoordinator.cpp
    src/composer/CatalogComposer.h
    src/composer/CatalogComposer.cpp
    src/composer/MotionCapabilityComposer.h
    src/composer/MotionCapabilityComposer.cpp
    src/composer/StreamingSlotCount.h
    src/composer/WakeStateComposer.h
    src/composer/WakeStateController.h
    src/composer/WakeStateController.cpp
    src/composer/TrayComposer.h
    src/composer/TrayController.h
    src/composer/TrayController.cpp
    src/composer/ThemeController.h
    src/composer/ThemeController.cpp
    src/composer/CrashReportingBackend.h
    src/composer/CrashReportingBackend.cpp
    src/composer/CrashReportingController.h
    src/composer/CrashReportingController.cpp
    # Theme and FontStacks are in dish_core, not the exe, so ThemeController,
    # ThemeBridge/TokensBridge and the tests can all reach them; the tests
    # cannot link the exe target.
    src/UI/Theme.h
    src/UI/Theme.cpp
    src/UI/FontStacks.h
    src/UI/FontStacks.cpp
    src/UI/SlotLiveStats.h
    src/UI/licenses/LicenseManifest.h
    src/UI/licenses/LicenseManifest.cpp
    src/Input/GamepadInputProcessor.h
    src/Input/GamepadInputProcessor.cpp
    src/Input/SdlMotionConvert.h
    src/Input/SdlMotionConvert.cpp
    src/Input/JoystickMapping.h
    src/Input/JoystickMapping.cpp
    src/Input/OutputCommandQueue.h
    src/Input/SDLGamepadBridge.h
    src/Input/SDLGamepadBridge.cpp
    src/FeatureSettings.h
    src/FeatureSettings.cpp
    src/LightbarRouting.h
    src/AppModel.h
    src/AppModel.cpp
    # QObject/QAbstractListModel only, no Qml/Quick, so the tests link them
    # directly. Registration into QML happens in the exe's QmlEntryPoint.
    src/qml/RenderTokens.h
    src/qml/SlotListModel.h
    src/qml/SlotListModel.cpp
    src/qml/ConnectionListModel.h
    src/qml/ConnectionListModel.cpp
    src/qml/AppViewModel.h
    src/qml/AppViewModel.cpp
    # Split out of AppViewModel so the mapping is testable without AppModel.
    src/qml/AppSettingsMaps.h
    src/qml/AppSettingsMaps.cpp)

add_library(dish_core STATIC ${DISH_CORE_SOURCES})
target_include_directories(dish_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(dish_core PUBLIC DISH_VERSION="${PROJECT_VERSION}")
target_link_libraries(dish_core
    PUBLIC
        Qt6::Core Qt6::Network
        # For the theme palette (QColor + QStyleHints) and the role models.
        Qt6::Gui
        # ScreenSaver inhibit, the BlueZ adapter probe and the appearance portal.
        Qt6::DBus
        # The Moonlight mDNS scan runs off the GUI thread via QtConcurrent::run.
        Qt6::Concurrent
        PkgConfig::SODIUM
        PkgConfig::SDL2
        Threads::Threads
    PRIVATE
        dish_warnings
        dish_strict
        dish_hardened
        # The Moonlight-host pairing/control crypto and the vendored ENet
        # transport. PRIVATE: no dish_core header exposes an OpenSSL or ENet
        # type across the library boundary.
        OpenSSL::Crypto
        dish_enet)

# --------------------------------------------------------------------------
# UI executable
# --------------------------------------------------------------------------
set(DISH_UI_SOURCES
    src/UI/CrashHandler.h
    src/UI/CrashHandler.cpp
    src/UI/common/ExternalLink.h
    src/UI/common/ExternalLink.cpp
    src/main.cpp
    # Brand glyphs, the bundled UI face and the license manifest, via AUTORCC.
    resources/icons.qrc)

qt_add_executable(Dish ${DISH_UI_SOURCES})
set_target_properties(Dish PROPERTIES OUTPUT_NAME dish)
target_link_libraries(Dish PRIVATE dish_core Qt6::Gui Qt6::Svg dish_warnings dish_strict dish_hardened)

# --------------------------------------------------------------------------
# Qt Quick UI: the Dish.Chrome QML module
# --------------------------------------------------------------------------
target_sources(Dish PRIVATE
    src/qml/QmlEntryPoint.h
    src/qml/QmlEntryPoint.cpp
    src/qml/chrome/ThemeBridge.h
    src/qml/chrome/ThemeBridge.cpp
    src/qml/chrome/TokensBridge.h
    src/qml/chrome/TokensBridge.cpp)

# DISH_KIT_QML_FILES: the canonical design-kit file list, kept in its own file
# so the kit reads as a unit rather than as lines lost in this one.
include(src/qml/kit/KitFiles.cmake)

# The module's .qml files live in subdirectories (kit/, pages/, wizard/,
# onboarding/, shared/), and from 6.8 Qt wants a qmldir per extra directory so a
# relative import resolves from the resource system the same way it does from
# disk. NEW generates them; the guard keeps the 6.7 floor buildable, where the
# policy does not exist and the old behaviour is the only behaviour.
# QT_KNOWN_POLICY_*, not CMake's if(POLICY ...): Qt policies live in their own
# namespace, and the CMake form is always false for them.
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# Qt runs qt6_import_qml_plugins at finalization to STATICALLY link the QML
# plugins an import scan turns up. It links a plugin only when that plugin's
# target is a STATIC_LIBRARY, so against a SHARED Qt the scan can never produce
# anything linkable — the modules load from the import path at run time instead.
#
# The scan is not merely useless there, it is noisy: Debian and Ubuntu default
# QT_SKIP_AUTO_QML_PLUGIN_INCLUSION to ON (see Qt6QmlPlugins.cmake, "Distributions
# should probably change this default"), so the Qt6::<plugin> targets are never
# created and every scanned plugin prints "the link target it defines does not
# exist in the current scope" — ~13 warnings on every configure.
#
# Turning that default back OFF is NOT the fix: Ubuntu ships no QmlAssetDownloader
# module, so auto-including every QML plugin config makes Qt6Quick_FOUND false and
# the configure fails outright.
#
# So skip the scan exactly where it cannot do anything: a shared Qt. A static Qt
# still scans and still links, which is the only configuration that needs it.
# QT_QML_MODULE_NO_IMPORT_SCAN is Qt's own early-out, read at the top of
# qt6_import_qml_plugins.
qt_add_qml_module(Dish
    URI Dish.Chrome
    VERSION 1.0
    # So qmllint/qmlcachegen load QtQuick's type database when analysing this
    # module; without it the singletons' QColor properties don't resolve.
    DEPENDENCIES QtQuick
    QML_FILES
        src/qml/Main.qml
        src/qml/AppShell.qml
        src/qml/UpdatePill.qml
        src/qml/UpdatePopover.qml
        ${DISH_KIT_QML_FILES}
        src/qml/shared/BindingDraft.qml
        src/qml/pages/HomePage.qml
        src/qml/pages/ControllersPage.qml
        src/qml/pages/ConnectionsPage.qml
        src/qml/pages/SettingsPage.qml
        # Detail pages pushed via Qt.resolvedUrl, so they must be listed in the
        # module or the resolved URL maps to no registered component.
        src/qml/pages/DeadzoneSettingsPage.qml
        src/qml/pages/DonatePage.qml
        src/qml/pages/LicensesPage.qml
        src/qml/pages/ControlsRemapPage.qml
        src/qml/pages/ConfigureBindingPage.qml
        src/qml/pages/PairingDialog.qml
        src/qml/onboarding/OnboardingFlow.qml
        src/qml/onboarding/WelcomeScreen.qml
        src/qml/onboarding/HelpScreen.qml
        src/qml/wizard/SetupWizardPage.qml
        src/qml/wizard/WizardInputPage.qml
        src/qml/wizard/WizardDestinationPage.qml
        src/qml/wizard/WizardTypePage.qml
        src/qml/wizard/WizardFeelPage.qml
        src/qml/wizard/WizardReviewPage.qml)

# Set AFTER qt_add_qml_module, which writes this property from its own
# NO_IMPORT_SCAN argument and would otherwise clobber the value.
if(QT6_IS_SHARED_LIBS_BUILD)
    set_target_properties(Dish PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN ON)
endif()

# qmltyperegistrar's generated .cpp #includes the bridge headers by BASENAME,
# so their directory has to be on the include path for it to compile.
target_include_directories(Dish PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qml/chrome)

# qmlcachegen compiles every .qml in the module to C++ and adds the result to
# this target, so Qt's generated translation units inherit dish_warnings and
# dish_strict. They do not survive them: `text.length > 0` compiles to a
# qsizetype-to-int assignment that -Wconversion rejects, and generated code
# admits no source fix. Same carve-out dish_strict already makes for Catch2's
# headers — the gate is for the code we write.
get_target_property(_dish_target_sources Dish SOURCES)
set(_dish_qmlcache_sources "")
foreach(_source IN LISTS _dish_target_sources)
    if(_source MATCHES "\\.rcc/qmlcache/.+_qml\\.cpp$")
        list(APPEND _dish_qmlcache_sources "${_source}")
    endif()
endforeach()
if(_dish_qmlcache_sources)
    set_source_files_properties(${_dish_qmlcache_sources} PROPERTIES COMPILE_OPTIONS "-w")
else()
    # Either this Qt runs no cachegen, in which case there is nothing to carve
    # out, or it moved the output and the next -Wconversion failure will name
    # the new path. Say so here rather than fail a configure that may be fine.
    message(STATUS
        "No qmlcachegen output on the Dish target; the generated-code warning "
        "carve-out matched nothing.")
endif()

target_link_libraries(Dish PRIVATE Qt6::Quick Qt6::Qml Qt6::QuickControls2)

# --------------------------------------------------------------------------
# Translations (Qt Linguist)
#
# qt_add_translations compiles each .ts → .qm and bakes the result into the
# executable as a Qt resource under :/i18n/. Util/Localization picks the right
# catalogue at startup from QLocale::uiLanguages(). Gated on
# Qt6LinguistTools_FOUND so a Qt install without qttools still builds — the app
# then runs in the untranslated source strings.
# --------------------------------------------------------------------------
if(Qt6LinguistTools_FOUND)
    qt_add_translations(Dish
        TS_FILES
            translations/dish_en.ts
            translations/dish_bs.ts
            translations/dish_de.ts
            translations/dish_es.ts
            translations/dish_fr.ts
            translations/dish_pt_BR.ts
        # Must stay identical to the lupdate call in
        # scripts/check-translations.sh, or `update_translations` writes
        # <location> tags the staleness gate then fails on.
        LUPDATE_OPTIONS -locations none -no-obsolete
        QM_FILES_OUTPUT_VARIABLE DISH_QM_FILES)
elseif(DISH_REQUIRE_TRANSLATIONS)
    message(FATAL_ERROR
        "Qt6LinguistTools not found and DISH_REQUIRE_TRANSLATIONS is ON.")
else()
    message(STATUS "Qt6LinguistTools not found — building without translation catalogues.")
endif()

# --------------------------------------------------------------------------
# Install
#
# One component, Runtime, so `cmake --install --component Runtime` is exactly
# the payload every packaging path wants and CPack still emits one package.
# --------------------------------------------------------------------------
include(GNUInstallDirs)
# Debian wants the doc directory named after the binary package; GNUInstallDirs
# derives it from PROJECT_NAME, which is capitalised.
set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/dish")

install(TARGETS Dish RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)
# Reverse-DNS names: Wayland matches the window to its launcher by desktop-file
# id, and Flatpak requires the app id for the entry, the icon and the AppStream
# file alike. Every spelling of com.tinkernorth.Dish here is the same string.
install(FILES packaging/dish.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        RENAME com.tinkernorth.Dish.desktop
        COMPONENT Runtime)
# GNOME Software and KDE Discover list an app only if it ships this, and
# Flathub rejects a submission without it.
install(FILES packaging/com.tinkernorth.Dish.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
        COMPONENT Runtime)
install(FILES packaging/dish.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME com.tinkernorth.Dish.svg
        COMPONENT Runtime)
install(FILES packaging/dish.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME com.tinkernorth.Dish.png
        COMPONENT Runtime)

# The rest of the hicolor ladder, rendered from the SVG. Optional because
# rsvg-convert is a packaging-host tool, not a build dependency; without it the
# two icons above still give a working menu entry.
find_program(DISH_RSVG_CONVERT rsvg-convert)
if(DISH_RSVG_CONVERT)
    set(_dish_icons "")
    foreach(_size IN ITEMS 16 24 32 48 64 128 256)
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/icons/${_size}/com.tinkernorth.Dish.png")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}/icons/${_size}"
            COMMAND ${DISH_RSVG_CONVERT} -w ${_size} -h ${_size}
                    -o "${_out}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/dish.svg"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/dish.svg"
            VERBATIM)
        list(APPEND _dish_icons "${_out}")
        install(FILES "${_out}"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
                COMPONENT Runtime)
    endforeach()
    add_custom_target(dish_icons ALL DEPENDS ${_dish_icons})
else()
    message(STATUS "rsvg-convert not found — installing only the scalable and "
                   "512x512 launcher icons.")
endif()

if(DISH_INSTALL_UDEV_RULES)
    install(FILES packaging/udev/70-dish-hidraw.rules
            DESTINATION "${DISH_UDEV_RULES_DIR}"
            COMPONENT Runtime)
    # A rule outside the four directories udev scans is inert, and the failure
    # is silent: every USB-direct claim returns PermissionDenied instead.
    get_filename_component(_dish_udev_abs "${DISH_UDEV_RULES_DIR}"
                           ABSOLUTE BASE_DIR "${CMAKE_INSTALL_PREFIX}")
    if(NOT _dish_udev_abs MATCHES "^(/etc|/run|/usr/lib|/lib)/udev/rules\\.d$")
        message(WARNING
            "DISH_UDEV_RULES_DIR resolves to ${_dish_udev_abs}, which udev does "
            "not scan. USB-direct will fail PermissionDenied. Install the rule "
            "into /etc/udev/rules.d by hand, or set DISH_UDEV_RULES_DIR.")
    endif()
endif()

# LGPL-3.0-or-later needs both texts (the LGPL is written as permissions over
# the GPL), THIRD_PARTY.md is the attribution manifest, and Inter ships under
# the OFL. Debian Policy 12.5 additionally wants `copyright` under this name.
install(FILES LICENSE COPYING.GPL3 THIRD_PARTY.md
              packaging/fonts/Inter-LICENSE.txt packaging/deb/copyright
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        COMPONENT Runtime)

# lintian errors on an uncompressed man page and warns on a missing changelog.
# -n drops the timestamp so two builds of the same source produce the same bytes.
find_program(DISH_GZIP gzip)
if(DISH_GZIP)
    function(dish_install_gzipped src dest_dir dest_name)
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/packaging-gz/${dest_name}")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}/packaging-gz"
            COMMAND ${DISH_GZIP} -9nc "${src}" > "${_out}"
            DEPENDS "${src}"
            VERBATIM)
        string(MAKE_C_IDENTIFIER "dish_gz_${dest_name}" _tgt)
        add_custom_target(${_tgt} ALL DEPENDS "${_out}")
        install(FILES "${_out}" DESTINATION "${dest_dir}" COMPONENT Runtime)
    endfunction()
    dish_install_gzipped("${CMAKE_CURRENT_SOURCE_DIR}/packaging/dish.1"
                         "${CMAKE_INSTALL_MANDIR}/man1" "dish.1.gz")
    # lintian parses /usr/share/doc/<pkg>/changelog.gz as a DEBIAN changelog and
    # emits a syntax error per heading of a Keep-a-Changelog file, so generate
    # the Debian-shaped one and point it at CHANGELOG.md. The date is fixed so
    # two builds of the same source produce the same bytes.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/packaging-src/changelog"
"dish (${PROJECT_VERSION}) unstable; urgency=medium

  * Release ${PROJECT_VERSION}. See
    https://github.com/TinkerNorth/dish-linux/blob/main/CHANGELOG.md

 -- TinkerNorth <security@tinkernorth.com>  Thu, 01 Jan 2026 00:00:00 +0000
")
    dish_install_gzipped("${CMAKE_CURRENT_BINARY_DIR}/packaging-src/changelog"
                         "${CMAKE_INSTALL_DOCDIR}" "changelog.gz")
else()
    install(FILES packaging/dish.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
            COMPONENT Runtime)
endif()

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(DISH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    # test_translations asserts against the COMPILED .qm, so it must build
    # after them or it silently checks whatever an earlier build left on disk.
    # This cannot live in tests/CMakeLists.txt: qt_add_translations creates
    # `release_translations` only when THIS scope is finalized, which is after
    # add_subdirectory(tests) has run. The DEFER is what makes it work.
    if(Qt6LinguistTools_FOUND)
        cmake_language(DEFER CALL add_dependencies DishTests release_translations)
    endif()
endif()

# --------------------------------------------------------------------------
# Packaging (CPack)
#
# Every package is generated from the install() rules above, so it can never
# disagree with `cmake --install` about what ships — which is how the icons,
# the udev rule and the licence texts went missing from the hand-rolled .deb
# this replaces.
#
# Build each package on the distro it targets: dpkg-shlibdeps and rpmbuild's
# soname scanner both read the built binary, so a package built against the
# aqtinstall Qt would demand a Qt no distro ships.
# --------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME    "dish")
set(CPACK_PACKAGE_VENDOR  "TinkerNorth")
set(CPACK_PACKAGE_CONTACT "TinkerNorth <security@tinkernorth.com>")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Wireless gamepad client for the Satellite server")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/TinkerNorth/dish-linux")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "dish-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)
set(CPACK_COMPONENTS_ALL Runtime)
set(CPACK_DEB_COMPONENT_INSTALL OFF)
set(CPACK_RPM_COMPONENT_INSTALL OFF)
set(CPACK_SOURCE_GENERATOR "")

set(CPACK_DEBIAN_PACKAGE_NAME "dish")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
# The EXTENDED description only: CPack uses the summary above as the synopsis
# and indents each line below it.
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"Dish streams a local gamepad to a Satellite server over the LAN on an
encrypted UDP channel: sticks, buttons, motion and touchpad out, rumble and
battery back. It discovers servers over broadcast and mDNS, pairs with
trust-on-first-use key pinning, and can drive up to four pads at once.
.
Supported pads may also be claimed directly over USB for a higher polling
rate; that path needs the hidraw udev rule this package installs.
.
Dish never installs its own updates - it checks, and links out to the release.")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# What shlibdeps cannot see: QML modules and Qt plugins are opened by name at
# run time and appear in no DT_NEEDED, so a package without them installs,
# launches, and dies on `module "QtQuick.Effects" is not installed`. Derive the
# list from `grep -rho '^import [A-Za-z0-9_.]*' src/qml`.
#
# The SVG pair is the easiest of these to miss. The target links Qt6::Svg but
# references no Qt Svg SYMBOL — every brand glyph is decoded by the
# imageformats/libqsvg.so plugin at run time — so the linker drops libQt6Svg
# from DT_NEEDED and shlibdeps never sees it. Debian then splits that plugin
# out of libqt6svg6 into qt6-svg-plugins, so depending on the library alone
# still leaves every glyph as "Unsupported image format".
set(DISH_DEB_RUNTIME_DEPENDS
    "qml6-module-qtqml, qml6-module-qtqml-workerscript, qml6-module-qtquick, \
qml6-module-qtquick-window, qml6-module-qtquick-controls, \
qml6-module-qtquick-templates, qml6-module-qtquick-layouts, \
qml6-module-qtquick-effects, libqt6svg6, qt6-svg-plugins, qt6-qpa-plugins, \
hicolor-icon-theme"
    CACHE STRING "Runtime-loaded Qt pieces dpkg-shlibdeps cannot infer")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${DISH_DEB_RUNTIME_DEPENDS}")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "udev, qt6-wayland")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "bluez, gnome-shell-extension-appindicator")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/triggers")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

set(CPACK_RPM_PACKAGE_NAME "dish")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_LICENSE "LGPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_VENDOR "TinkerNorth")
set(CPACK_RPM_PACKAGE_DESCRIPTION
"Dish streams a local gamepad to a Satellite server over the LAN on an
encrypted UDP channel: sticks, buttons, motion and touchpad out, rumble and
battery back. It discovers servers over broadcast and mDNS, pairs with
trust-on-first-use key pinning, and can drive up to four pads at once.

Supported pads may also be claimed directly over USB for a higher polling
rate; that path needs the hidraw udev rule this package installs.

Dish never installs its own updates - it checks, and links out to the release.")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_AUTOPROV ON)
set(CPACK_RPM_PACKAGE_REQUIRES
    "qt6-qtdeclarative, qt6-qtsvg, hicolor-icon-theme, systemd-udev")
set(CPACK_RPM_PACKAGE_SUGGESTS "qt6-qtwayland, bluez, gnome-shell-extension-appindicator")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/post.sh")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postun.sh")
# rpm treats a duplicate directory claim as a conflict, and these are owned by
# filesystem, hicolor-icon-theme and systemd.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/bin
    /usr/lib
    /usr/lib/udev
    /usr/lib/udev/rules.d
    /usr/share
    /usr/share/applications
    /usr/share/doc
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/16x16 /usr/share/icons/hicolor/16x16/apps
    /usr/share/icons/hicolor/24x24 /usr/share/icons/hicolor/24x24/apps
    /usr/share/icons/hicolor/32x32 /usr/share/icons/hicolor/32x32/apps
    /usr/share/icons/hicolor/48x48 /usr/share/icons/hicolor/48x48/apps
    /usr/share/icons/hicolor/64x64 /usr/share/icons/hicolor/64x64/apps
    /usr/share/icons/hicolor/128x128 /usr/share/icons/hicolor/128x128/apps
    /usr/share/icons/hicolor/256x256 /usr/share/icons/hicolor/256x256/apps
    /usr/share/icons/hicolor/512x512 /usr/share/icons/hicolor/512x512/apps
    /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps
    /usr/share/man
    /usr/share/man/man1
    /usr/share/metainfo)

set(CPACK_GENERATOR "TGZ")
find_program(DISH_DPKG dpkg)
find_program(DISH_RPMBUILD rpmbuild)
if(DISH_DPKG)
    list(APPEND CPACK_GENERATOR "DEB")
endif()
if(DISH_RPMBUILD)
    list(APPEND CPACK_GENERATOR "RPM")
endif()

include(CPack)
