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
# Dependencies
# --------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Network DBus Svg Quick Qml QuickControls2)
qt_standard_project_setup(REQUIRES 6.7)
# Optional: without qttools the .ts → .qm step is skipped and every qsTr()
# falls back to its English source string, which still builds a working app.
find_package(Qt6 6.7 QUIET OPTIONAL_COMPONENTS LinguistTools)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SDL2   REQUIRED IMPORTED_TARGET sdl2)


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
    src/core/wire/SessionCrypto.h
    src/core/wire/SessionCrypto.cpp
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
    src/core/reducer/ConnectionRows.h
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
    src/Util/DisplaySleepInhibitor.h
    src/Util/DisplaySleepInhibitor.cpp
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
    src/source/inputrate/SlotInputRates.h
    src/source/inputrate/InputRateStore.h
    src/source/inputrate/InputRateStore.cpp
    src/source/system/BluetoothRadioProbe.h
    src/source/system/BluetoothRadioProbe.cpp
    # Update checking. In dish_core (not the exe) because the tests link this
    # library, not the exe.
    src/update/UpdatePorts.h
    src/update/HttpGateways.h
    src/update/HttpGateways.cpp
    src/update/UpdateChecker.h
    src/update/UpdateChecker.cpp
    src/repository/SettingsKeys.h
    src/repository/RememberedWifiEq.h
    src/repository/SatellitePinRepository.h
    src/repository/SatellitePinRepository.cpp
    src/repository/SatelliteSharedKeyRepository.h
    src/repository/SatelliteSharedKeyRepository.cpp
    src/repository/RememberedSatelliteRepository.h
    src/repository/RememberedSatelliteRepository.cpp
    src/repository/ConnectionStore.h
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
    src/composer/CatalogComposer.h
    src/composer/CatalogComposer.cpp
    src/composer/MotionCapabilityComposer.h
    src/composer/MotionCapabilityComposer.cpp
    src/composer/StreamingSlotCount.h
    src/composer/WakeStateComposer.h
    src/composer/WakeStateController.h
    src/composer/WakeStateController.cpp
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
    src/UI/CrashHandler.h
    src/UI/CrashHandler.cpp
    src/UI/common/ExternalLink.h
    src/UI/common/ExternalLink.cpp
    src/main.cpp
    # Brand glyphs, the bundled UI face and the license manifest, via AUTORCC.
    resources/icons.qrc)

qt_add_executable(Dish ${DISH_UI_SOURCES})
set_target_properties(Dish PROPERTIES OUTPUT_NAME dish)
target_link_libraries(Dish PRIVATE dish_core Qt6::Gui Qt6::Svg dish_warnings dish_strict)

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

# qmltyperegistrar's generated .cpp #includes the bridge headers by BASENAME,
# so their directory has to be on the include path for it to compile.
target_include_directories(Dish PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qml/chrome)

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
        QM_FILES_OUTPUT_VARIABLE DISH_QM_FILES)
else()
    message(STATUS "Qt6LinguistTools not found — building without translation catalogues.")
endif()

# --------------------------------------------------------------------------
# Install
# --------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS Dish RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Reverse-DNS names: Wayland matches the window to its launcher by desktop-file
# id, and Flatpak requires the app id for both the entry and the icon.
install(FILES packaging/dish.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        RENAME com.tinkernorth.Dish.desktop)
install(FILES packaging/dish.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME com.tinkernorth.Dish.png)
install(FILES packaging/dish.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME com.tinkernorth.Dish.svg)
# Without this rule every USB-direct claim fails PermissionDenied and Dish
# keeps the pad on the SDL path.
install(FILES packaging/udev/70-dish-hidraw.rules
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/udev/rules.d)

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(DISH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
