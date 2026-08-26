# FetchContent fallback so contributors don't need a system Catch2 package.
# The tag is pinned: an unpinned FetchContent makes the test suite's result
# depend on whatever upstream tagged today.
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
    # Owns the one QCoreApplication for the session; see the file header for
    # why a function-local static is not equivalent.
    main.cpp
    test_kernel.cpp
    test_async_state.cpp
    test_session_crypto.cpp
    test_moonlight_wire.cpp
    test_moonlight_control_cipher.cpp
    test_moonlight_crypto.cpp
    test_moonlight_pairing.cpp
    test_moonlight_xml.cpp
    test_moonlight_rtsp.cpp
    test_moonlight_rtsp_framing.cpp
    test_moonlight_tls_config.cpp
    test_moonlight_session_machine.cpp
    test_moonlight_button_map.cpp
    test_moonlight_host_repository.cpp
    test_atomic_counter.cpp
    test_hex.cpp
    test_endian.cpp
    test_models.cpp
    test_beacon_parser.cpp
    test_mdns_discovery.cpp
    test_stable_key.cpp
    test_tofu.cpp
    test_ip_literals.cpp
    test_mdns_mapping.cpp
    test_discovery_gateway.cpp
    test_satellite_tls_verifier.cpp
    test_pin_repository.cpp
    test_shared_key_repository.cpp
    test_remembered_satellite_repository.cpp
    test_connection_store_identity.cpp
    test_remembered_reconnect.cpp
    test_gamepad_input_processor.cpp
    test_sdl_motion_convert.cpp
    test_joystick_mapping.cpp
    test_joystick_remap_assign.cpp
    test_capture_mode.cpp
    test_joystick_remap_repository.cpp
    test_gamepad_button_layouts.cpp
    test_deadzones.cpp
    test_deadzone_repository.cpp
    test_motion_preference_repository.cpp
    test_motion_enabled_store.cpp
    test_motion_backend_status_store.cpp
    test_motion_capability_composer.cpp
    test_physical_motion_source.cpp
    test_pairing_client_classify.cpp
    test_rest_control_plane.cpp
    test_reverse_pairing.cpp
    test_battery_ui.cpp
    test_config_blocker.cpp
    test_touchpad_mode_resolve.cpp
    test_touchpad_mode_repository.cpp
    test_touchpad_mode_store.cpp
    test_pairing_machine.cpp
    test_satellite_session_machine.cpp
    test_udp_opcodes.cpp
    test_session_reconcile.cpp
    test_satellite_link_state.cpp
    test_connection_rows.cpp
    test_carried_pads.cpp
    test_resend_pacer.cpp
    test_late_slot_converge.cpp
    test_binding_presence.cpp
    test_pad_identity_seed.cpp
    test_connections_composer.cpp
    test_connection_coordinator.cpp
    test_found_visibility.cpp
    test_session_lifecycle.cpp
    test_session_manager.cpp
    test_catalog_repository.cpp
    test_catalog_dto.cpp
    test_controller_type_store.cpp
    test_picker_visibility.cpp
    test_screen_wake_controller.cpp
    test_wake_state_composer.cpp
    test_wake_state_controller.cpp
    # Background running, the tray gateway and the suspend/resume edge.
    test_background_mode.cpp
    test_keep_awake.cpp
    test_keep_awake_preference_store.cpp
    test_controller_activity_source.cpp
    test_background_preference_store.cpp
    test_background_coordinator.cpp
    test_tray_presentation.cpp
    test_tray_composer.cpp
    test_tray_controller.cpp
    test_tray_icon_fallback.cpp
    test_sleep_cycle.cpp
    test_sleep_coordinator.cpp
    test_dish_notifications.cpp
    test_input_rate_tracker.cpp
    test_input_rate_store.cpp
    test_latency_window.cpp
    test_slot_live_stats.cpp
    test_motion_indicator_state.cpp
    test_freedesktop_wake_inhibitor.cpp
    test_satellite_client_rumble.cpp
    test_satellite_client_motion.cpp
    test_satellite_client_touchpad.cpp
    test_satellite_client_session.cpp
    test_satellite_client_lightbar.cpp
    test_satellite_client_send_counter.cpp
    test_wifi_connection_rekey.cpp
    test_output_command_queue.cpp
    test_lightbar_routing.cpp
    test_feature_settings.cpp
    test_host_battery.cpp
    test_rumble_routing.cpp
    test_rumble_helpers.cpp
    test_battery_routing.cpp
    test_battery_validator.cpp
    test_physical_battery_mapping.cpp
    test_touchpad_routing.cpp
    test_usb_path_machine.cpp
    test_usb_path_machine_edge_cases.cpp
    test_usb_poll_rate.cpp
    test_poll_rate_sampler.cpp
    test_path_choice.cpp
    test_transport_failure.cpp
    test_app_settings_migration.cpp
    test_secret_service_store.cpp
    test_crash_report.cpp
    test_usb_path_resolution.cpp
    test_usb_path_preference_store.cpp
    test_usb_gamepad_manager.cpp
    test_usb_report_parsers.cpp
    test_usb_hid_layout.cpp
    test_usb_twin_dedup.cpp
    test_hid_transport.cpp
    test_catalog_prewarm.cpp
    test_onboarding_store.cpp
    test_license_manifest.cpp
    test_theme_store.cpp
    test_crash_reporting.cpp
    test_main_ui_state.cpp
    test_slot_list_model.cpp
    test_slot_path_fields.cpp
    test_connection_list_model.cpp
    test_app_view_model_maps.cpp
    test_app_view_model_connections.cpp
    test_app_view_model_bind_availability.cpp
    test_capability_solver.cpp
    test_apply_binding_machine.cpp
    test_font_stacks.cpp
    test_theme_contrast.cpp
    # Auto-updater.
    test_update_version.cpp
    test_update_manifest.cpp
    test_update_machine.cpp
    test_update_checker.cpp
    test_update_preference_store.cpp
    test_translations.cpp
    test_udev_rules_coverage.cpp
    test_hidraw_admission.cpp
    test_http_gateways.cpp
    test_ui_preference_store.cpp
    test_catalog_feature_gate.cpp
    test_crash_log_path.cpp
    test_unix_signal_watcher.cpp
    test_reduced_motion.cpp)

add_executable(DishTests ${DISH_TEST_SOURCES})
target_link_libraries(DishTests
    PRIVATE
        dish_core
        Catch2::Catch2
        dish_warnings)

# The manifest is a resource of the Dish exe, not of dish_core, which is what
# DishTests links — so the test reads the source file by absolute path instead.
target_compile_definitions(DishTests PRIVATE
    DISH_LICENSES_JSON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../assets/licenses/licenses.json")

# Same reason for the .qm files, which land beside the top-level binary dir.
target_compile_definitions(DishTests PRIVATE
    DISH_QM_DIR="${CMAKE_BINARY_DIR}"
    DISH_TS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../translations")

# Same reason again for the udev rule: it is packaging data, not a resource, and
# the test that cross-checks it against the claimable-model table reads the file.
target_compile_definitions(DishTests PRIVATE
    DISH_UDEV_RULES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../packaging/udev/70-dish-hidraw.rules")
# The `release_translations` dependency is wired from the TOP-LEVEL CMakeLists:
# that target does not exist until this directory scope is finalized.

# SKIP_RETURN_CODE 4 is Catch2's contract: a test case that calls SKIP() exits 4,
# and without telling CTest what that means, every skip is reported as a failure.
# The keyring round-trip is the case that needs it — it exercises a real
# org.freedesktop.secrets and skips where none answers, which is every CI runner
# and every headless box, so treating its skip as a failure made the whole suite
# red anywhere without a desktop keyring.
catch_discover_tests(DishTests PROPERTIES SKIP_RETURN_CODE 4)
