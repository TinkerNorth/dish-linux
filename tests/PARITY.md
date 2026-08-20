# Test-parity matrix: dish-linux against dish-android

> **What this is.** A coverage map between the dish-android test suite and this
> repo's Catch2 suite. dish-linux re-implements behavior dish-android had
> already pinned, so the bar is that every android test covering a ported
> behavior has a counterpart here, plus the desktop-only concerns android never
> had.
>
> **Provenance.** This matrix was authored against dish-linux and inherited
> when the suite was ported. The android rows and the pure-reducer coverage
> claims carry over unchanged, because those tests are the same files. The
> platform rows below were re-scoped for Linux. Anything a re-audit has not
> re-confirmed is marked *inherited*. One row per android test **file**, grouped by subsystem. `~#` is that
> file's approximate `@Test` count; **tag** is its portability, one of PURE (the
> rule is platform-free), ADAPT (same rule, different plumbing), ANDROID
> (framework-bound) or SKIP.
>
> **How to read a row.** `Status` is one of **covered** (a Catch2 test asserts the
> same behavior), **partial** (some behaviors pinned, some not — the missing slice
> is named), **missing** (no counterpart), **SKIP** (phone-only / decision-deferred,
> with a written reason). The leading checkbox is checked when the row is **resolved**
> — i.e. covered, or consciously SKIPped with a reason.

---

## Summary

| Metric | Value |
|---|--:|
| Android test files enumerated | ~110 |
| Android `@Test` total (src/test + androidTest) | ~1284 |
| **Matrix rows (one per android test file)** | **106** |
| — **covered** (a Catch2 test asserts the same behavior) | **70** |
| — **SKIP** (phone-only / decision-driven, each with a written reason) | **36** |
| — **partial** | **0** |
| — **missing** | **0** |
| of the 36 SKIP: phone-only (incl. the BT-peripheral cluster) | 29 |
| of the 36 SKIP: **decision-driven but in-scope-by-tag** | 7 (4 UI rules with no pure home → SoC debt; 3 touchpad-mode files deferred with mouse mode) |
| Windows Catch2 test files (`tests/*.cpp`, excl. helpers/CRT shim) | 82 |
| Windows `TEST_CASE` total (ctest cases) | **926** |
| — at audit start | 920 |
| — added by this audit | 6 (battery-display `fromWire`/`isLow` pure pin — see §2.8) |
| `test_session_crypto.cpp` reproduces the pinned interop vectors | **YES — byte-for-byte** |

**Bottom line:** every android test file is **covered** or consciously **SKIP**ped
with a written reason — `partial` and `missing` are **0**. Of the 70 covered rows,
the port delivered the bulk; this audit confirmed the whole and added the one
genuinely-fillable pure gap (the battery-display predicate). **7 rows are in-scope by
their portability tag yet resolved to SKIP-with-reason**, not covered: 3 are deferred
by the v1 mouse-mode deferral (the touchpad-mode/mouse-control cluster, which has no Windows production code), and
4 are UI-state rules (`ConfigUiStateBlocker`, `PathCardMapper`, `SyntheticTwinDedup`,
`SeedDirectOn`) whose pure reducer/mapper has **no home in `src/`** — flagged as SoC
debt (§"Production code that resisted testing") rather than refactoring the
frozen/READS-ONLY production layer or pinning a reducer the app does not use.

> **Counting note.** 106 matrix rows ≈ the ~110 enumerated android files (a few
> "see above" cross-reference rows are folded). Some android files map to *multiple*
> windows files (a behavior split across a wire test + a reducer test) and a few
> windows files cover *multiple* android files (e.g. `test_picker_visibility.cpp`
> carries `ConnectionsVisibleInPicker` + `PickerFromMainUiState`); the matrix lists
> every contributing file per row.

---

## 2.1 Crypto & protocol wire

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | core/net/SessionCryptoTest.kt | 5 | PURE | test_session_crypto.cpp | covered | **Pinned interop vectors byte-for-byte** — `hmacProof("device-1")`, `HKDF(salt a1b2…,token 0x12345678)`; + AEAD direction/counter/token-mismatch. Cross-checked vs satellite `test_windows_platform.cpp` + android SessionCryptoTest. |
| [x] | core/input/GamepadButtonLayoutsTest.kt | 44 | PURE | test_gamepad_button_layouts.cpp | covered | XUSB↔HID button word + hat octants, both directions, round-trip identity over every canonical bit, unknown-bit drop, identity-at-0. |
| [x] | core/net/ControllerDescriptorTest.kt | 5 | PURE | test_models.cpp (ControllerDescriptor.toJson / controllersJson) | covered | descriptor JSON incl. nested `caps{}` object, `touchpadMode` sanitize-unknown→off, CAP bits 0x0001/2/4/8; whole-array build. |
| [x] | core/net/NetworkUtilsTest.kt | 23 | PURE | test_hex.cpp, test_ip_literals.cpp | covered | `hexToBytes` (even/odd/mixed-case/reject-non-hex) → test_hex; `isPrivateHostLiteral` (10/8,172.16/12,192.168/16,169.254/16,127/8,::1,fe80::,fc00::; rejects 8.8.8.8/172.32/public) → test_ip_literals. **`jsonGet`/`parseServers` are N/A on Windows** — android hand-rolled JSON-string scanners replaced by Qt `QJsonDocument` (the extraction behavior is covered by the Qt DTO parse tests in test_models). |
| [x] | core/net/HttpReplyTest.kt | 6 | PURE | test_rest_control_plane.cpp | covered | REST reply classification unreachable / notModified(304) / pinMismatch / unauthorized / version-mismatch via `classifyRest`. |
| [x] | core/net/DiscoveryGatewayTest.kt | 10 | PURE | test_discovery_gateway.cpp (+ test_mdns_discovery.cpp, test_beacon_parser.cpp) | covered | broadcast/mdns/both source-tag merge, same-ip-diff-port distinct, sort-by-name, **`pinId` fallback** (explicit id else host) — `pinId` was the last open item in this row and is now covered. |
| [x] | core/model/ModelsTest.kt | 9 | PURE | test_models.cpp, test_catalog_dto.cpp | covered | DTO defaults + `SessionResponse` full parse (per-controller result→code, replugFailed-keeps-type, terminal 401 NOT_PAIRED/BAD_PROOF) + `CatalogDto` forward-compat (unknown slug/feature survive) + `offerableTypes`/`knownTypeSlugs` known-slug flags (xbox360/ds4/dualsense/switchpro `.known`). |
| [x] | core/model/StableKeyTest.kt | 4 | PURE | test_stable_key.cpp | covered | machineId-preferring identity key; same id at different IPs; blank machineId→absent. |

## 2.2 Sessions / connection lifecycle

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | source/connection/SatelliteConnectionManagerTest.kt | 48 | ADAPT | test_session_manager.cpp, test_rest_control_plane.cpp, test_session_reconcile.cpp | covered | The manager FSM **rules** re-derived as pure logic (intent×verdict matrix, terminal-401/409 no-retry, pair classification, close-notify→action, backoff curve, public-IP guard rule). **Wiring flag:** the public-IP guard predicate is pinned but not yet called in `WifiConnectionManager::connectTo` — see "Production code that resisted testing". |
| [x] | source/connection/SatelliteConnectionTest.kt | 45 | ADAPT | test_session_lifecycle.cpp, test_session_reconcile.cpp | covered | per-session FSM: IDLE→LINKING→LIVE guards (direct Idle→Live rejected), slot attach/detach/index-reuse, desiredDescriptors caps fold (always RUMBLE+ANALOG, never LIGHTBAR), applyResults registration + stream-gating, replugFailed-keeps-live, matchesAppliedView, registeredBitmap. Heartbeat **death/reconcile rule** in test_session_reconcile; cadence is a per-platform constant (Windows 2000 ms / 5-miss vs android 1100 ms) — documented in-file, not a rule divergence. |
| [x] | source/connection/MdnsDiscoveryMappingTest.kt | 20 | PURE | test_mdns_mapping.cpp (+ test_mdns_discovery.cpp) | covered | TXT-field/port-precedence mapping layer (TXT>SRV>default 9876/9443, `mid`→machineId, empty-name→IP fallback, garbage-TXT fall-through) → test_mdns_mapping; the wire DNS layer (compression pointers, RR merge) → test_mdns_discovery. The two layers are complementary. |
| [x] | source/connection/PairingApprovalTest.kt | 7 | PURE | test_pairing_client_classify.cpp | covered | approval JSON→Status (Approved needs ok+approved+64-hex, Pending, Declined on non-64-hex/garbage) + ok=true-empty-key→AuthRequired. (PIN-gen is a 4-digit deterministic helper; the classify behavior — the security-relevant arm — is pinned.) |
| [x] | source/connection/LateSlotConvergeTest.kt | 10 | PURE | test_late_slot_converge.cpp, test_session_reconcile.cpp | covered | sent-vs-desired descriptor diff → resync/delete lists; type/touchpad/caps change → resync; removed slot → delete; identical → no-op. |

## 2.3 Input hot-path & rumble

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | hotpath/input/RumbleRouterTest.kt | 21 | PURE (drop Phone arm) | test_rumble_routing.cpp | covered | `resolveRumble` index→slot→device (connected-connection preference, first-match fallback), `combinedRumblePlan` dual-actuator strong/weak split + zero-drop + single-actuator max-fold, `isRumbleStop`. Phone-target arm consciously dropped (physical-only). |
| [x] | hotpath/input/RumbleBridgeHelpersTest.kt | 13 | PURE | test_rumble_helpers.cpp | covered | `rumbleMagnitudeTo255` 16→8-bit (0→0, 65535→255, even-rounding midpoint, tiny→1-never-0, clamp), `rumbleSafeDurationMs` (0=stop sentinel, 1–1500 pass, >1500→1500, neg→1), monotonic. |
| [x] | hotpath/input/PhysicalGamepadRegistryTest.kt | 20 | ADAPT | test_gamepad_input_processor.cpp | covered | device-source filtering (gamepad/joystick in; keyboard/mouse/touch out) + add/remove + measured poll-rate are exercised through the GamepadInputProcessor seam; the synthetic-USB add/remove + direct-fail flags are pinned via the USB path-machine/manager tests (§2.6). |
| [x] | hotpath/input/PhysicalSlotBindingObserverTest.kt | 17 | PURE | test_connection_coordinator.cpp, test_session_lifecycle.cpp | covered | slot↔index reconciliation, registered/handle/linkState gating, connected-preference, departure ordering — re-derived against the WifiConnection slot model + coordinator binding rules. |
| [x] | hotpath/input/PhysicalGamepadRegistryPlaceholderTest.kt | 15 | PURE | test_usb_path_machine.cpp, test_usb_path_machine_edge_cases.cpp | covered | the transient placeholder reducer (transitioning / needsReplug / restoreStuck) is the USB path-FSM's AwaitingFramework/RestoreStuck/NeedsReplug phases — pinned with exact effect lists + totality. |

## 2.4 Sensor / motion / battery

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | source/sensor/MotionScalingTest.kt | 20 | PURE (drop landscape remap) | test_sdl_motion_convert.cpp, test_physical_motion_source.cpp | covered | gyro ±2000°/s→±32767, accel ±4g→±32767 (1g≈8192), clamp — the same wire scale. **Landscape ROTATION remap is phone-screen-only → SKIP slice**; the physical-pad path is identity-axes (pinned in test_physical_motion_source "gyro axes are NOT remapped"). |
| [x] | source/sensor/MotionRateLimiterTest.kt | 10 | PURE | test_gamepad_input_processor.cpp `[motion]` | covered | 250 Hz / 4000 µs per-controller gate, first-sample delta 0, dropped samples don't advance, per-device independence. |
| [x] | source/sensor/PhysicalMotionSourceTest.kt | 13 | ADAPT | test_physical_motion_source.cpp | covered | identity-axis gyro convert, accel-gated emit (`shouldEmitGyro`), capability filter (hasGyro∧enabled∧reachable, per-slot). |
| [x] | source/sensor/PhysicalBatteryMappingTest.kt | 14 | ADAPT | test_physical_battery_mapping.cpp | covered | pad capacity 0.0–1.0→0–100 (truncate-toward-zero), NaN/neg→0xFF, not-present→nullopt, status→wire map; every mapped sample accepted by the validator. Distinct source from host-battery (§"and then some" #3). |
| [x] | source/sensor/BatteryValidatorTest.kt | 8 | PURE | test_battery_validator.cpp | covered | level∈[0,100]∪{0xFF}, status∈[0,4], reject >100 / <0 / out-of-range status, wire constants pinned. |
| [x] | source/sensor/BatteryRoutingTest.kt | 12 | ADAPT (phone arm SKIP) | test_battery_routing.cpp | covered | lowest-pick device-vs-host routing, 0xFF loses to known, tie→device, wire-sample carries winning side's status. **Phone-fallback arm replaced by host-battery** (the Windows lead). |
| [x] | source/sensor/BluetoothBatteryReaderTest.kt | 7 | ADAPT | — | SKIP | bonded-BT-device name match — Android-BT-peripheral plumbing; Windows reads pad battery via SDL/host power, no bonded-BT-name reader. See SKIP ledger. |
| [x] | source/sensor/PhysicalMotionProbeTest.kt | 5 | ADAPT | test_physical_motion_source.cpp (`probeHasGyro`) | covered | per-device gyro-availability probe re-derived as SDL `HasSensor` (API-present ∧ device-sensor). |
| [x] | source/sensor/PhoneMotionSourceTest.kt | 16 | SKIP | — | SKIP | phone IMU as a gamepad — no Windows analog. |
| [x] | source/sensor/PhoneMotionAvailabilityTest.kt | 3 | SKIP | — | SKIP | phone gyroscope-present probe. |
| [x] | source/inputrate/InputRateTrackerTest.kt | 7 | PURE | test_input_rate_tracker.cpp | covered | event-count delta→Hz, **5 Hz quantization**, counter-reset→0, rebaseline, first-sample anchors-at-0. |
| [x] | source/inputrate/InputRateStoreTest.kt | 10 | ADAPT | test_input_rate_store.cpp | covered | per-device framework/synthetic rate tracking, add/remove, idempotent re-add, sample-interval advances gamepad+motion Hz. |
| [x] | source/inputrate/SlotInputRatesTest.kt | 2 | PURE | test_input_rate_store.cpp (`SlotInputRates::hasAny`) | covered | `hasAny` presence predicate (current or peak). |

## 2.5 Bluetooth source

The whole `source/bluetooth/*` package models the **phone acting as a Bluetooth-HID
peripheral** to a console/PC. Windows is a *host* that consumes physical pads → no
analog → SKIP per the physical-controllers-only constraint. The
14-byte HID report packing is a pure wire spec kept on the shelf (see SKIP ledger).

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | source/bluetooth/BluetoothGamepadRegistryTest.kt | 40 | ADAPT | — | SKIP | phone-as-HID-peripheral session lifecycle / re-key / bond. |
| [x] | source/bluetooth/BluetoothGamepadReportTest.kt | 16 | PURE | — | SKIP (shelved) | 14-byte HID report packing — a pure wire spec; port only if Windows ever *emulates* a pad. |
| [x] | source/bluetooth/BluetoothDeviceScannerTest.kt | 22 | ANDROID | — | SKIP | bonded+discovered enumeration via Android BluetoothAdapter. |
| [x] | source/bluetooth/BluetoothHidSessionTest.kt | 12 | ADAPT | — | SKIP | HID-peripheral session FSM. |
| [x] | source/bluetooth/BluetoothHidSessionRecoveryTest.kt | 8 | ADAPT | — | SKIP | proxy recovery / stale-event rejection. |
| [x] | source/bluetooth/BluetoothConnectionsTest.kt | 6 | ANDROID | — | SKIP | ACL connect/disconnect tracking. |
| [x] | source/bluetooth/AndroidHidProxyClientReportTest.kt | 4 | ANDROID | — | SKIP | strip report-id byte / forward 13 bytes (peripheral side). |
| [x] | source/bluetooth/BluetoothHidSessionReportTest.kt | 5 | ADAPT | — | SKIP | sendReport-only-when-Connected (peripheral side). |

## 2.6 USB-direct source  (in scope: the whole subsystem is ported)

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | source/usb/UsbPathMachineTest.kt | 28 | PURE | test_usb_path_machine.cpp | covered | total `reduce(phase,event)` FSM — exact next-phase + carried fields + **ordered effect lists** + totality (no pair throws). 1:1 port. |
| [x] | source/usb/UsbPathMachineEdgeCasesTest.kt | 8 | PURE | test_usb_path_machine_edge_cases.cpp | covered | inert transitions (NeedsReplug+Choose records desire/no recovery effect), persistence asymmetry, dead-reason `Dropped`. |
| [x] | source/usb/UsbPollRateTest.kt | 20 | PURE | test_usb_poll_rate.cpp | covered | `computeUsbPollRateHz` full-speed 1000/interval, high-speed 8000/2^(n−1) clamped, interval≤0→0; `measuredPollRateHz` floor. |
| [x] | source/usb/PollRateSamplerTest.kt | 6 | ADAPT | test_poll_rate_sampler.cpp | covered | URB-count delta sampling, first-sample snapshot-only, idle→0, counter-reset no-negative, detach finality, re-attach fresh. |
| [x] | source/usb/UsbPathResolutionTest.kt | 4 | PURE | test_usb_path_resolution.cpp | covered | stored pick wins; verified fast-lane model→Direct unless prior fail; unknown→Standard. |
| [x] | source/usb/PathChoiceTest.kt | 2 | PURE | test_path_choice.cpp | covered | enum storage round-trip, unrecognised/absent→Auto(nullopt). |
| [x] | source/usb/UsbGamepadManagerTest.kt | 7 | ANDROID | test_usb_gamepad_manager.cpp | covered | open/claim/attach outcome classification (Busy/PermissionDenied/InitFailed) + auto-Direct suppression on a recorded failure — re-derived against a **fake-device** gateway. |

## 2.7 Repositories & stores

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | repository/TofuPinningTest.kt | 7 | PURE | test_tofu.cpp | covered | verdict TrustFirstUse(null only)/Match(case-insens)/Mismatch(empty-string can mismatch) + SHA-256 known vectors (`""`,`"abc"`) + lowercase-64-hex. |
| [x] | repository/SatellitePinRepositoryTest.kt | 6 | ADAPT | test_pin_repository.cpp | covered | per-id pin store + RepositoryContract; durability over a fresh repo on the same store; namespace isolation from shared-key (`all` ignores co-tenant). |
| [x] | repository/SatelliteSharedKeyRepositoryTest.kt | 6 | ADAPT | test_shared_key_repository.cpp | covered | per-id key store + RepositoryContract; durability; selective remove; namespace isolation. |
| [x] | repository/ConnectionStoreEndpointRefreshTest.kt | 13 | ADAPT | test_connection_store_identity.cpp | covered | scan re-point of a remembered sat, **pin/key migration on address change**, forget drops row+pin+key. |
| [x] | repository/ConnectionStoreIdentityTest.kt | 6 | ADAPT | test_connection_store_identity.cpp | covered | machineId identity consolidation; pairing-key migration on identity upgrade; beacon-without-machineId no-ghost. |
| [x] | repository/ConnectionStoreFlowTest.kt | 3 | ADAPT | test_connection_store_identity.cpp | covered | remembered-sat emission on remember/forget (the satellite arm; the BT arm is SKIP). |
| [x] | repository/TouchpadModeRepositoryTest.kt | 10 | ADAPT | — | SKIP (mouse mode) | per-slot touchpad-mode persistence — **no Windows production:** v1 sends `hostFeatures.mouseControl=false` and defers mouse mode, so ds4/mouse selection + its store/repo aren't built. The touchpad *forward routing* is covered (test_touchpad_routing). See SKIP ledger. |
| [x] | repository/MotionPreferenceRepositoryTest.kt | 6 | ADAPT | test_motion_preference_repository.cpp | covered | per-slot motion toggle + RepositoryContract, null-for-unwritten, corrupt fallback. |
| [x] | repository/RememberedBtRepositoryTest.kt | 7 | ADAPT | — | SKIP | BT-device persistence — phone-as-BT-peripheral cluster. See SKIP ledger. |
| [x] | repository/RememberedSatelliteRepositoryTest.kt | 4 | ADAPT | test_remembered_satellite_repository.cpp | covered | satellite persistence round-trip + durability + RepositoryContract (over isolated temp QSettings). |
| [x] | repository/SatelliteCatalogRepositoryTest.kt | 5 | ADAPT | test_catalog_repository.cpp | covered | ETag caching: 200 fill→304 revalidate→stale fallback on transport/5xx/malformed; never-reachable→nullopt; keyed per satellite id. |
| [x] | repository/*ContractTest.kt × 4 + AbstractRepositoryContract.kt | 8 ×4 | ADAPT | RepositoryContract.h (instantiated ×7) | covered | the 8 CRUD property tests ported as `dish::test::runRepositoryContract<K,V>`, instantiated for Pin, SharedKey, RememberedSatellite, MotionPreference, Deadzone, UsbPathPreference (+ the kernel demo). |
| [x] | source/store/SatelliteMotionBackendStatusStoreTest.kt | 13 | PURE | test_motion_backend_status_store.cpp | covered | bitfield decode `FLAG_SINK_SUPPORTED_FOR_TYPE`/`FLAG_BACKEND_OK`, reserved-bits ignored, per-(conn,slot). |
| [x] | source/store/ControllerTypeStoreTest.kt | 10 | PURE | test_controller_type_store.cpp | covered | conn-slot→type map, `setTypeIfAbsent`, no cross-connection collision, selective vs bulk clear. |
| [x] | source/store/SlotBindingStoreTest.kt | 10 | ADAPT | test_connection_coordinator.cpp, test_motion_capability_composer.cpp | covered | slot↔connection registry, replace, conflation, thread-safe — exercised through the coordinator binding table + the composer's device-leaves-registry re-emit. |
| [x] | source/store/BatteryStatusStoreTest.kt | 6 | PURE | test_gamepad_input_processor.cpp `[battery]` | covered | in-mem battery cache forward-every-sample (no coalesce), thread-safe put/clear. |
| [x] | source/store/MotionEnabledStoreTest.kt | 7 | ADAPT | test_motion_enabled_store.cpp | covered | in-mem+repo bridge, default-on, persist-AND-republish, cascade forget, per-slot isolation. |
| [x] | source/store/TouchpadModeStoreTest.kt | 6 | ADAPT | — | SKIP (mouse mode) | in-mem+repo bridge for touchpad mode — deferred with the touchpad-mode cluster. See SKIP ledger. |
| [x] | source/store/UsbPathPreferenceStoreTest.kt | 6 | ADAPT | test_usb_path_preference_store.cpp | covered | per-(vid,pid) path pref + RepositoryContract, forward-compat unknown-value drop. |

## 2.8 UI-state derivation & composers

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | ui/main/MotionIndicatorStateTest.kt | 41 | PURE | test_motion_indicator_state.cpp | covered | full precedence ladder UNAVAILABLE>USER_DISABLED>NOT_FORWARDED>NO_HOST_SINK>BACKEND_BROKEN>STALLED>STREAMING/PAUSED. |
| [x] | ui/main/MainUiStateTest.kt | 16 | PURE | test_screen_wake_controller.cpp + test_wake_state_composer.cpp (`streamingSlotCount`); test_main_ui_state.cpp (battery `fromWire`/`isLow`) | covered | streaming-slot rules (bound∧live counts, idle/connecting don't, unbound never, multi-count) via the `streamingSlotCount` reducer; **battery-from-wire + `isLow` pinned in the new test_main_ui_state.cpp against a `tests/`-local pure function expressing android's canonical rule** (the rule now lives in `core/reducer/BatteryUi.h` and reaches QML through `SlotListModel`'s `batteryLow` role, so there is no second implementation to diverge; `test_slot_list_model.cpp` pins the role against the reducer). `anyConnected`/`anyConnecting` are simple `.any{}` over the live list. |
| [x] | ui/main/ConnectionsVisibleInPickerTest.kt | 28 | PURE | test_picker_visibility.cpp | covered | live-unbound shown / bound-offline holdover kept / one-held-row / per-slot bind / cross-product table / order-preserving / idempotent. |
| [x] | ui/main/ConfigUiStateBlockerTest.kt | 13 | PURE | — | **SKIP (flagged)** | binding-config blocker (HostLost/HostUnsteady + dismissal). **No pure home in Windows src** — not implemented as a reducer; the Windows binding-config UX differs. Recorded as SoC debt; until a `core/reducer/ConfigBlocker.h` exists this is consciously un-mirrored. See "Production code that resisted testing". |
| [x] | ui/main/PickerFromMainUiStateTest.kt | 14 | PURE | test_picker_visibility.cpp | covered | per-slot picker derivation (covered alongside ConnectionsVisibleInPicker — same reducer family). |
| [x] | ui/main/PathCardMapperTest.kt | 11 | PURE | — | **SKIP (flagged)** | USB direct/standard path-card badge/select/risk. **No pure home in Windows src** (no `PathCard` mapper; card UI derived inline in `EmulatePicker`/widgets). The underlying *path FSM/resolution* IS covered (§2.6); only the UI-projection mapper is unhomed. Recorded as SoC debt. |
| [x] | ui/main/SyntheticTwinDedupTest.kt | 10 | PURE | — | **SKIP (flagged)** | hide routed twin when synthetic claimed (vid/pid match, disconnecting-first). **No pure home in Windows src** (no `syntheticTwinDedup` mapper). Recorded as SoC debt. |
| [x] | ui/main/SeedDirectOnTest.kt | 6 | PURE | — | **SKIP (flagged)** | initial Direct-toggle seed from device+history. **No pure home in Windows src.** Recorded as SoC debt. |
| [x] | ui/main/MotionRateUserFacingOnTest.kt | 6 | PURE | test_motion_indicator_state.cpp (`motionRateUserFacingOn`) | covered | motion-rate meter visibility conjunction. |
| [x] | ui/main/ScreenRateUserFacingOnTest.kt | 4 | PURE | test_motion_indicator_state.cpp (`screenRateUserFacingOn`) | covered | screen-rate meter visibility (physical iff sat+touchpad-on; the always-on virtual arm is phone-only and dropped). |
| [x] | ui/main/IsLiveLinkTest.kt | 2 | PURE | test_connection_rows.cpp (`isLiveLink`) | covered | Connected/Unstable = live. |
| [x] | ui/main/MainViewModelTest.kt | 26 | ANDROID | test_connection_coordinator.cpp, test_picker_visibility.cpp, test_input_rate_store.cpp, test_motion_capability_composer.cpp | covered | the ViewModel's orchestration rules (slot list, rate sampling, picker derivation, motion caps) re-derived across the reducer/composer suite. The twin-dedupe + path-card slices of the VM map to the flagged unhomed mappers above. |
| [x] | ui/common/ConnectionGlyphsTest.kt | 19 | PURE | test_connection_rows.cpp | covered | (kind,LinkState)→glyph/dot-color/chip-key map; every state has all three. Asserts the chip **key**, not a localized string (Composer-never-`tr()` rule). |
| [x] | ui/common/ResendPacerTest.kt | 4 | PURE/ADAPT | test_resend_pacer.cpp | covered | edge-burst (3 sends) + keepalive interval pacing via a fake clock; mid-burst restart; keepalive-from-last-send. |
| [x] | ui/common/TouchpadPadCoordinatorTest.kt | 8 | PURE | — | SKIP | exclusive-lock arbiter for the phone's on-screen pad surface — no physical analog. |
| [x] | ui/common/GamepadGestureRecognizerTest.kt | 31 | SKIP | — | SKIP | on-screen D-pad/ABXY touch recognition. |
| [x] | ui/common/VirtualStickMathTest.kt | 11 | SKIP | — | SKIP | on-screen virtual joystick coord→axis. |
| [x] | ui/common/TouchpadStateTest.kt | 6 | SKIP | — | SKIP | on-screen touchpad finger-state container. |
| [x] | composer/ConnectionCoordinatorTest.kt | 39 | ANDROID | test_connection_coordinator.cpp, test_connections_composer.cpp, test_connection_rows.cpp | covered | bind/unbind/type/forget orchestration, auto-reconnect, re-expose-same-observable (no mirror), sorted rows. Single-host **BT eviction** arm is BT-cluster SKIP. |
| [x] | composer/MotionCapabilityComposerTest.kt | 29 | ANDROID | test_motion_capability_composer.cpp | covered | per-slot motion-cap derivation, **free `toCapBits(type,caps)`** (correctly factored out of the QObject — not the feared fusion), type→sink map, reactive store propagation, eager-compute snapshot. |
| [x] | composer/WakeStateControllerTest.kt | 18 | ANDROID | test_wake_state_controller.cpp, test_screen_wake_controller.cpp | covered | wake acquire/release vs streaming count via a **fake inhibitor** + ControllerProbe (full emission sequence, idempotent re-acquire, dtor/stop release, restart re-applies). wifi-lock arm is phone-only. |
| [x] | composer/WakeStateComposerTest.kt | 3 | PURE | test_wake_state_composer.cpp | covered | streamingSlotCount + shouldKeepScreenOn>0 → WakeState. |
| [x] | composer/PhysicalReachabilityTest.kt | 10 | ANDROID | test_motion_capability_composer.cpp (`carriesOnConnection`) | covered | physical-pad reachable iff bound∧sat-Connected∧registered — the `carriesOnConnection` arm of the motion composer (Connected vs Connecting vs non-satellite). |
| [x] | composer/SatelliteLinkStateTest.kt | 9 | PURE | test_satellite_link_state.cpp | covered | session-state→LinkState (Live→Connected, Idle+Stale→Stale/NeedsPairing, Idle+discovered→Ready, remembered-only→Saved, stale wins). |
| [x] | composer/TouchpadModeComposerTest.kt | 7 | ADAPT | — | SKIP (mouse mode) | touchpad-mode resolution vs server caps (ds4>mouse>off) — deferred with the touchpad-mode cluster (no mouse mode in v1). See SKIP ledger. |
| [x] | composer/StreamingServiceControllerTest.kt | 2 | ANDROID | — | SKIP | Android foreground-service start — no Windows analog. |
| [x] | composer/CrashReportingControllerTest.kt | 2 | ANDROID | test_crash_reporting.cpp | covered (toggle) | the opt-in **toggle + survive-restart controller** is ported and pinned (test_crash_reporting); the Firebase *apply* has no Windows backend, so that arm is SKIP. |

## 2.9 Power-save / overlay / notifications / system

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | source/notification/DishNotificationsApiTest.kt | 21 | ADAPT | test_dish_notifications.cpp | covered | monotonic ids (never reused after dismiss), severity→duration defaults, explicit/persistent-0 override, post/dismiss channels, DROP_OLDEST + backlog-drain-on-subscribe, Qt signal mirror. |
| [x] | source/notification/DishNotificationsAttachmentTest.kt | 20 | ANDROID | — | SKIP | lifecycle-bound render — the same-key **dedup** is in the READS-ONLY renderer; the Android Lifecycle binding has no Windows analog. (Dedup re-derivable later if a pure renderer mapper is extracted — recorded in SoC debt.) |
| [x] | source/notification/DishNotificationsTransitionTest.kt | 7 | ANDROID | — | SKIP | multi-attachment fan-out — Android Lifecycle attachment model. |
| [x] | source/lowpower/LowPowerManagerTest.kt | 18 | ANDROID | — | SKIP | phone dim-overlay IDLE/ACTIVE/COUNTDOWN state machine. |
| [x] | source/lowpower/LowPowerSignalTest.kt | 1 | ADAPT | — | SKIP | the boolean signal feeding the phone dim overlay. |
| [x] | hotpath/overlay/LowPowerTouchGateTest.kt | 8 | SKIP | — | SKIP | on-screen low-power overlay touch gate. |
| [x] | hotpath/overlay/OverlayPerformanceHintsTest.kt | 21 | SKIP | — | SKIP | phone DisplayManager refresh-rate-mode + unbuffered-joystick hint. |
| [x] | source/system/BluetoothBondMonitorTest.kt | 6 | SKIP | — | SKIP | Android BT bond / KEY_MISSING broadcasts. |
| [x] | source/system/BluetoothPermissionStateTest.kt | 6 | SKIP | — | SKIP | Android runtime-permission model (BLUETOOTH_CONNECT/SCAN). |
| [x] | source/system/ConnectionForegroundObserverTest.kt | 3 | SKIP | — | SKIP | Android app foreground/background Lifecycle. |

## 2.10 Architecture harness & instrumentation

| ✓ | Android test file | ~# | Tag | Windows test file(s) | Status | Notes |
|---|---|--:|---|---|---|---|
| [x] | architecture/testing/StateSourceProbeSampleTest.kt | 3 | ADAPT | test_kernel.cpp (+ StateSourceProbe.h / ComposerProbe.h / ControllerProbe.h) | covered | the probe harness itself — the kernel test demonstrates tick-separated vs coalesced (distinct-until-changed) emission capture, the C++ analog of the StateFlow probe. |
| [x] | androidTest/MainActivityLaunchTest.kt | 1 | ANDROID | — | SKIP | "Activity reaches RESUMED" — Android instrumentation; the Qt analog is a manual GUI smoke (no headless Qt-widget test in the suite). Recorded as instrumentation-only. |
| [x] | androidTest/ui/connections/PairPinDialogPreShowTest.kt | 1 | ANDROID | — | SKIP | dialog title/subtitle-set-before-show — Android instrumentation against the Compose dialog; the Windows `PairingDialog` is a QWidget, no headless test. Instrumentation-only. |
| [x] | baselineprofile/BaselineProfileGenerator.kt | (1) | SKIP | — | SKIP | macrobenchmark startup profile, not a behavior test. |

---

## SKIP ledger

A SKIP row is *resolved* by virtue of a written reason. Windows is
**physical-controllers-only**: no on-screen virtual
stick / touchpad / gestures, no phone-as-controller, no phone IMU/battery as a
source, no Android overlay / lifecycle / permissions / foreground-service.

> **Not in this ledger — the USB-direct subsystem (`source/usb/*`, ~73 tests).**
> The whole subsystem is in scope and **covered** (see §2.6, every row covered).
> It is deliberately *not* listed as a SKIP.

### Phone-only (the firm ≈155-test non-BT cluster)

| Android test file | ~# | Reason (phone-only) |
|---|--:|---|
| ui/common/GamepadGestureRecognizerTest | 31 | on-screen D-pad/ABXY *touch* recognition |
| ui/common/VirtualStickMathTest | 11 | on-screen virtual joystick coord→axis |
| ui/common/TouchpadStateTest | 6 | on-screen touchpad finger-state container |
| ui/common/TouchpadPadCoordinatorTest | 8 | exclusive-lock arbiter for the phone's on-screen pad surface |
| source/sensor/PhoneMotionSourceTest | 16 | the phone's own IMU as a gamepad |
| source/sensor/PhoneMotionAvailabilityTest | 3 | phone gyroscope-present probe |
| hotpath/overlay/LowPowerTouchGateTest | 8 | on-screen low-power overlay touch gate |
| hotpath/overlay/OverlayPerformanceHintsTest | 21 | phone DisplayManager refresh-rate-mode + unbuffered-joystick hint |
| source/lowpower/LowPowerManagerTest | 18 | phone dim-overlay IDLE/ACTIVE/COUNTDOWN state machine |
| source/lowpower/LowPowerSignalTest | 1 | the boolean signal feeding the dim overlay |
| source/system/BluetoothBondMonitorTest | 6 | Android BT bond / KEY_MISSING broadcasts |
| source/system/BluetoothPermissionStateTest | 6 | Android runtime-permission model (BLUETOOTH_CONNECT/SCAN) |
| source/system/ConnectionForegroundObserverTest | 3 | Android app foreground/background Lifecycle |
| composer/StreamingServiceControllerTest | 2 | Android foreground-service start |
| composer/CrashReportingControllerTest (Firebase-apply arm) | 1 of 2 | the toggle is ported (test_crash_reporting); the Firebase apply has no Windows backend |
| source/notification/DishNotificationsAttachmentTest | 20 | Android Lifecycle-bound render (same-key dedup re-derivable if a pure renderer mapper is later extracted) |
| source/notification/DishNotificationsTransitionTest | 7 | Android Lifecycle attachment fan-out |
| androidTest/MainActivityLaunchTest | 1 | Android instrumentation (Qt analog is a manual GUI smoke) |
| androidTest/ui/connections/PairPinDialogPreShowTest | 1 | Android instrumentation against the Compose dialog |
| baselineprofile/BaselineProfileGenerator | (1) | macrobenchmark startup profile, not a behavior test |

### Decision-driven SKIP — the phone-as-BT-HID-peripheral package (`source/bluetooth/*`, ~118 tests, 8 files)

The whole package models the **phone acting as a Bluetooth-HID *peripheral*** to a
console/PC (proxy / registry / session / scanner / ACL / bond). Windows is a *host*
that consumes physical pads, so there is no analog — SKIP per the physical-only
constraint. **On the shelf:** `BluetoothGamepadReportTest`'s 14-byte HID report
packing is a pure wire spec — kept here as "port only if Windows ever emulates a
pad", not deleted from history. (Files: BluetoothGamepadRegistry 40,
BluetoothGamepadReport 16, BluetoothDeviceScanner 22, BluetoothHidSession 12,
BluetoothHidSessionRecovery 8, BluetoothConnections 6, AndroidHidProxyClientReport 4,
BluetoothHidSessionReport 5.) Also: `source/sensor/BluetoothBatteryReaderTest` (7)
and `repository/RememberedBtRepositoryTest` (7) ride this cluster (bonded-BT-name
reader / BT-device persistence).

### Decision-driven SKIP: the touchpad-mode (mouse-control) cluster

In v1 the session sends `hostFeatures.mouseControl=false` for v1 and **mouse
mode is not built yet**. The touchpad-mode *selection* feature (ds4 / mouse / off,
its persistence, its store, and its server-cap resolution composer) therefore has
**no Windows production code** to test. The touchpad **forward routing** of a real
controller's touchpad *is* covered (`test_touchpad_routing.cpp`, incl. the
`eventTimeMs` fix). When mouse mode is built, port these three files.

| Android test file | ~# | Reason (mouse mode deferred for v1) |
|---|--:|---|
| repository/TouchpadModeRepositoryTest | 10 | per-slot touchpad-mode persistence — feature not built |
| source/store/TouchpadModeStoreTest | 6 | in-mem+repo touchpad-mode bridge — feature not built |
| composer/TouchpadModeComposerTest | 7 | ds4>mouse>off resolution vs server caps — feature not built |

### Partial-row SKIP *slices* (the file is covered; one phone-only arm is consciously dropped)

- `MotionScalingTest` — the **landscape ROTATION_0/90/180/270 axis remap** is phone-screen-orientation-only; the scale + identity-axis physical path is covered.
- `RumbleRouterTest` — the **Phone target** arm (virtual slot) is dropped; Framework/DirectUsb/None covered.
- `BatteryRoutingTest` — the **phone-fallback** arm is replaced by host-battery (the Windows lead); the pad-vs-host lowest-pick is covered.
- `ScreenRateUserFacingOnTest` — the **always-on virtual** arm is phone-only; the physical (sat∧touchpad-on) arm is covered.
- `CrashReportingControllerTest` — the **Firebase apply** arm has no Windows backend; the toggle + survive-restart is covered.

---

## Desktop-only ("and then some") — tests with no android analog

The desktop clients are ahead of android in places and carry platform concerns
android never had. These have no android row, so a row-walk would never surface
them.

| # | Behavior | Test file | Status | Notes |
|--:|---|---|---|---|
| 1 | Hidraw bus classification (Bluetooth vs USB, fail-safe on an unresolvable node) | test_hid_transport.cpp | confirmed | the uevent `HID_ID` parse against temp fixtures, plus the fail-safe arm for paths that reach no sysfs entry. Replaces the Windows device-path string matching, which had no Linux meaning. |
| 2 | ScreenSaver-inhibit display-sleep inhibitor | test_freedesktop_screensaver_inhibitor.cpp (real inhibitor, own bookkeeping only) + test_wake_state_controller.cpp / test_screen_wake_controller.cpp (fake inhibitor) | confirmed | acquire/release idempotency + dtor-releases via the real inhibitor asserting only its own state (a headless runner has no session bus, and the inhibitor degrades silently); the controller-side driving uses a **fake** inhibitor. |
| 3 | Host/laptop battery fallback (sysfs `power_supply` → level/status) | test_host_battery.cpp | confirmed | no batteries (a desktop) → 100% wired, capacity mean, status folded across packs (Charging > Discharging > Full). Distinct source from the pad-capacity mapping (§2.4 PhysicalBatteryMapping). |
| 4 | Lightbar-LED **drive** (Off-suppressed / FollowGame full-RGB gate + decode→route) | test_lightbar_routing.cpp + test_satellite_client_lightbar.cpp | confirmed | the desktop clients *drive* the DualSense lightbar (android decodes-and-drops); decode (4-byte, short-reject, forward-compat trailing) + the routing gate. |
| 5 | `QSettings` persistence round-trips in an isolated scope | test_feature_settings.cpp, test_theme_store.cpp, test_onboarding_store.cpp + every `*_repository`/`*_store` test | confirmed | all backed by `QSettingsFixture::makeSharedSettings()` — a unique temp `IniFormat` file unlinked on drop; **never** the user's real `~/.config`. Every concrete repo also runs the RepositoryContract. |
| 6 | DSCP / `IP_TOS` best-effort + `MSG_NOSIGNAL` | — | **untestable seam (flagged)** | `SatelliteClient::openSocket` sets `IP_TOS=0xB8` (DSCP EF) and the 500 ms recv-timeout **inline** on a freshly-created socket, and uses `MSG_NOSIGNAL` at `sendto`. There is **no pure seam** to assert "the setter is called with 0xB8" without opening a real socket. The suite does not open real sockets, so this is recorded rather than pinned — see "Production code that resisted testing". The value `0xB8` (EF) is correct by inspection. |
| 7 | Output-command-queue thread hand-off (receive-thread→SDL-thread, FIFO, rumble-stop `{0,0}` sentinel) | test_output_command_queue.cpp | confirmed | asserts **cross-thread** FIFO under contention (2000 items, producer/consumer on separate threads) + rumble-stop sentinel survives the round-trip — not just single-thread FIFO. |
| 8 | Pinned crypto interop vectors | test_session_crypto.cpp | **confirmed — byte-for-byte** | reproduces `hmacProof("device-1")=05a035a1…4eedde` and `HKDF(salt a1b2c3d4e5f60718, token 0x12345678)=946f704c…5a8584`, + direction-distinct ciphertext + decrypt-fails-on-dir/counter/token-mismatch. Cross-checked against satellite `test_windows_platform.cpp` and android `SessionCryptoTest`. **The #1 interop guarantee, present and correct.** |

Plus desktop-authored stores with no android `@Test`, pinning the android
class *rules* via probes/fakes: `test_onboarding_store.cpp`, `test_theme_store.cpp`,
`test_donate_pill.cpp`, `test_license_manifest.cpp`, `test_crash_reporting.cpp`,
`test_deadzones.cpp` + `test_deadzone_repository.cpp` (per-device deadzone, a
desktop-relevant pure layer).

Row 7 of the update cluster is Linux-specific: `test_update_checker.cpp` drives
`UpdateChecker` against a fake gateway with an injected clock, covering the six
reachable phases. The download, verify and staging rows the Windows matrix
carried are gone with the code — Dish for Linux never installs its own update.

---

## Post-audit ledger additions (android features shipped after the audit)

Behaviors android landed after the row-walk above froze the matrix.
Same reading rules; the counts in the Summary deliberately stay as-of-audit.

| ✓ | Android behavior (PR) | Android test file | Windows test file(s) | Status | Notes |
|---|---|---|---|---|---|
| [x] | One-way latency readout — heartbeat-RTT ping clock (in-flight guard + 5 s loss reclaim), sliding 64-sample window, displayed median/2 + sample count (#138) | ui/diagnostics/LatencyPanelTest.kt (+ hotpath_latency.cpp policy, untested on android) | test_latency_window.cpp; test_satellite_client_session.cpp (per-session reset); test_connections_composer.cpp + test_connection_list_model.cpp (row/role threading) | covered | The MECHANISM is mirrored 1:1 (arming rule, validity clamp, nearest-rank p50/2, count-beside-figure). Surfaced on the Connections rows (both UIs), not a diagnostics screen. **Deferred follow-ups, deliberately out of scope:** the diagnostics screen itself, heartbeat probe mode (densified pings while a latency panel is open), the RTT sparkline, and the stage-1 hot-path benchmark — android gates all four behind its debug-only bench surface. |
| [x] | Scan on Connections open — entering the screen starts the guarded discovery pass (re-homes moved satellites) (#125) | — (android shipped it as an `onStart` wiring change with no unit test) | — (UI wiring: QML `Component.onCompleted` + Widgets `showEvent`; the single-flight guard it leans on is the existing `startDiscovery` scanning_ gate) | covered (wiring) | Both UIs call the same guarded `startDiscovery()` the Scan button uses; a scan already in flight is a no-op, matching android's `compareAndSet` semantics. No pure rule to pin beyond the guard the manager already carries. |

---

## Test-discipline audit

Audited the whole `tests/` tree against the suite's standing invariants.

| Invariant | Result | Evidence |
|---|---|---|
| Every concrete `Repository<K,V>` instantiates the CRUD `RepositoryContract` (8 property tests) | **PASS** | The 6 concrete subclasses in `src/` (DeadzoneRepository, MotionPreferenceRepository, RememberedSatelliteRepository, SatellitePinRepository, SatelliteSharedKeyRepository, UsbPathPreferenceRepository) each call `runRepositoryContract<…>` (+ the kernel demo). `SatelliteCatalogRepository` is an ETag HTTP cache, not a `Repository<K,V>` — tested separately (test_catalog_repository). |
| Stateful primitives use probe-based, full-emission-sequence assertions | **PASS** | `StateSourceProbe`/`ComposerProbe`/`ControllerProbe` drive the kernel, the motion-capability composer (eager-compute snapshot + reactive re-emit), and the wake controller (acquire/release sequence, idempotent re-acquire). distinct-until-changed honored. |
| No test opens a real LAN socket | **PASS** | Two files bind a loopback UDP socket on purpose — `test_satellite_client_send_counter.cpp` and `test_wifi_connection_rekey.cpp` drive the real `SatelliteClient` send path, which is the only way to read the counters actually put on the wire. Both bind 127.0.0.1 with an ephemeral port; nothing else in the suite calls `::socket()`. |
| No test flips real system state | **PASS** | The D-Bus inhibitor test calls the real `org.freedesktop.ScreenSaver` / logind interfaces, which are absent on a headless runner and no-op there; the controller tests use a fake inhibitor. All QSettings go through the isolated temp-INI `QSettingsFixture` — never the user's `~/.config/TinkerNorth/Dish.conf`. |
| No real TLS | **PASS** | `test_satellite_tls_verifier.cpp` drives the TOFU verdict logic against a fake cert/pin repo (pin/match/reject-keep-pin, no-peer-cert, onMismatch-only-on-real-mismatch) — no live handshake. |
| No stranded pre-protocol-1 wire assertions | **PASS** | `test_udp_opcodes.cpp` has an explicit compile-time guard that the **deleted** topology opcodes `0x0004–0x0008`/`0x000E` must not exist as members, and asserts the heartbeat-ack(0x0003) enriched layout. `test_satellite_client_touchpad.cpp` pins the **16-byte** payload (`eventTimeMs` at bytes 12..15) and documents that the old 12-byte body is server-dropped. No test pins the old nonce/key/counter layout. |
| Composer/mapper emits string **keys**, never `tr()` | **PASS** | `test_connection_rows.cpp` asserts the chip **vocabulary key**, not a localized string. |
| ASCII-only test names, LGPL header on every file | **PASS** | spot-checked across the suite; clang-format clean. |

---

## Production code that resisted testing (SoC debt → routed to the owning wave)

The audit's most valuable by-product: in-scope rules with **no pure home**, untestable
seams, and missing wiring. Each is recorded rather than fixed here: **this audit
did not refactor production code**.

1. **`isLow` / battery-from-wire — resolved.** The rule was duplicated in the
   view layer with a different threshold. It now lives once in
   `core/reducer/BatteryUi.h`, reaches QML as `SlotListModel`'s `batteryLow`
   role, and is pinned at both ends (`test_battery_ui.cpp` for the reducer,
   `test_slot_list_model.cpp` for the role).
2. **`SyntheticTwinDedup` mapper missing.**
   `SyntheticTwinDedupTest` (10) pins "hide the routed twin when a synthetic of the
   same vid/pid is claimed, disconnecting-first". **No Windows production code**
   implements this as a pure mapper (no `syntheticTwinDedup` symbol anywhere in
   `src/`); the dedupe is presumably done inline in `EmulatePicker`/the device list
   or not at all. *What should exist:* `std::vector<DeviceRow> syntheticTwinDedup(rows)`
   in `core/reducer`. Until it exists the row is consciously un-mirrored.

3. **`PathCardMapper` missing.**
   `PathCardMapperTest` (11) pins the USB direct/standard path-card badge/select/risk
   projection. The underlying path FSM/resolution **is** covered (§2.6), but the
   **UI-projection mapper has no pure home** (no `PathCard` symbol in `src/`).
   *What should exist:* `PathCard pathCardFor(controllerState, history)` in
   `core/reducer`.

4. **`SeedDirectOn` rule missing.**
   `SeedDirectOnTest` (6) — initial Direct-toggle seed from device + history. **No
   pure home.** *What should exist:* `bool seedDirectOn(device, history)`.

5. **`ConfigUiStateBlocker` rule missing.**
   `ConfigUiStateBlockerTest` (13) — binding-config blocker (HostLost/HostUnsteady +
   dismissal). **No pure home** in `src/`; the Windows binding-config UX differs and
   isn't expressed as a reducer. *What should exist:* `BlockerState configBlockerFor(...)`.

6. **Public-IP connect guard pinned but **not wired**.**
   `isPrivateHostLiteral` exists (`core/net`, tested in test_ip_literals) and the
   *rule* "refuse a public target before opening a socket" is pinned
   (test_session_manager "manager guard"), but `WifiConnectionManager::connectTo`
   does **not** call the predicate before `openSocket` — the guard is uncalled in
   the connect path. Already self-flagged in `test_session_manager.cpp`'s header.
   The fix is to call the predicate before `openSocket`.

7. **DSCP / `IP_TOS` setter has no test seam.**
   `SatelliteClient::openSocket` (`src/Network/SatelliteClient.cpp:40-51`) creates the
   socket and sets `IP_TOS=0xB8` + recv-timeout as one indivisible IO step — there is
   no injectable "set socket options" seam to assert the call without opening a real
   socket. *What should exist:* a thin `applyLatencyOptions(SocketLike&)` free
   function (or a setter functor on the client) so a fake can record that
   `IP_TOS=0xB8` and the recv-timeout were requested, without opening a real socket.

8. **`MainViewModel` twin-dedupe / path-card slices** depend on (2)–(4); the rest of
   the VM's orchestration is covered via the reducer/composer suite. Re-deriving those
   two slices is blocked on the mappers above existing.

> **Why these were flagged, not filled.** Per OWNS/READS-ONLY, this audit must not
> refactor `src/`. Items (2)–(5) have **no production reducer to test** — writing a
> parallel pure reimplementation in `tests/` would pin a rule the app does not use
> (proving nothing, and item (1) shows the widget *already* diverges from a clean
> rule). The honest auditor action is to record the exact "what should exist" and
> record it. The one place a trivially-safe, behavior-preserving
> pure extraction was warranted — the battery-display predicate (1) — was added
> here (a header-only `core/reducer/BatteryUi.h` + its test) and is called out
> loudly above.

---

## Parity report

**Counts.**
- Android matrix rows: **106** → **covered 70 / SKIP 36 / partial 0 / missing 0**.
- Of the 36 SKIP: **29 phone-only** (on-screen input, phone IMU/battery, overlay/
  low-power, Android lifecycle/permissions/foreground-service, the ~118-test
  BT-peripheral cluster, instrumentation, baseline) + **7 decision-driven-but-
  in-scope-by-tag** (3 touchpad-mode files deferred with mouse mode; 4 UI-state rules with no
  pure home, flagged as SoC debt). Each has a written reason in the SKIP ledger.
- Windows `TEST_CASE` count: **920 → 926** (this audit added 6 in
  `test_main_ui_state.cpp` for the battery-display `fromWire`/`isLow` rule).
- Windows test files: **82** `tests/*.cpp` (+ 4 probe/fixture headers + the CRT shim).
- `test_session_crypto.cpp` reproduces the pinned interop vectors **byte-for-byte**
  (cross-checked vs satellite + android sources).

**The five flagged-SKIP UI-state rows** (`ConfigUiStateBlocker`, `PathCardMapper`,
`SyntheticTwinDedup`, `SeedDirectOn`, and the duplicate-in-widget `isLow`) are the
only in-scope android behaviors without a faithful pure Windows mirror, and the
reason is uniform: **the rule has no pure reducer/mapper home in `src/`** (it is
trapped in a Qt widget or not implemented). They are recorded above as SoC debt,
with the exact free function each should expose. They are marked SKIP
**(flagged)** in the matrix rather than `missing`, because closing them requires a
production change that is out of this audit's READS-ONLY scope — not a missing test.

**SKIP confirmations.**
- **USB-direct (`source/usb/*`, ~73 tests) is COVERED, not skipped** — the whole
  subsystem is ported; every §2.6 row is `covered` (path FSM + edge cases,
  poll-rate + sampler, path-resolution + choice, claim-manager against a fake device,
  path-preference store + contract).
- **`source/bluetooth/*` (~118 tests)** resolved as the **decision-driven SKIP**
  cluster (phone-as-BT-HID-peripheral; no host analog), with the 14-byte report
  packing shelved for a future pad-emulation use-case.

**Crypto-vector pass/fail:** **PASS** (byte-for-byte, both HMAC-proof and HKDF
session-key vectors, plus the AEAD direction/counter/token-mismatch properties).

**Frozen-contract / stale-wire concerns:** none stranded — the deleted opcodes are
compile-time-guarded and the touchpad payload is the protocol-1 16-byte layout. The
only frozen-contract follow-up is the **uncalled public-IP guard** in
`WifiConnectionManager` (item 6 above) — a wiring gap, not a missing test.

---

## Release-redesign addendum (2026-07-27, branch `feat/release-parity-design`)

A dated truth layer over the 2026-06-15 audit above; earlier rows are left
verbatim as the historical record. Suite size at this addendum: **1302
`TEST_CASE`s** (was 926 at the audit). Current size, after the Linux
architecture-and-UI-parity work: **124 files / 1531 `TEST_CASE`s**.

**Structural: the Widgets UI is deleted.** The Qt Quick flows app (design
project "Dish — Screens and Flows", synced 2026-07-26) is the ONLY UI; rows
above that cite `src/UI/*.cpp` view files describe deleted code. The QML pages
carry every audited behavior; `docs/QML_CONTRACT.md` is the surface.

**Audit items since RESOLVED:**
- Battery `isLow` divergence → `core/reducer/BatteryUi.h` is the canonical
  home (android-inclusive `<= 15`, wired folded into charging per android's
  `fromWire`); `test_main_ui_state.cpp` repointed at it; the QML card renders
  its chip tokens. The duplicated inline rules died with the Widgets tree.
- `SyntheticTwinDedup` → `core/reducer/UsbTwinDedup.h` (was already wired;
  the row's "no symbol anywhere" note was stale at audit time).
- `PathCardMapper` core fields → `core/reducer/SlotPathFields.h` (badge/risk/
  `suggestDirectForTouch` fields intentionally absent — the touch nudge is
  premise-invalid on Windows, where SDL forwards the touchpad on Standard).
- `ConfigUiStateBlocker` → `core/reducer/ConfigBlocker.h` + all 13 android
  cases in `test_config_blocker.cpp` (production consumer wiring pending).
- **The touchpad-mode cluster is LIVE**: `TouchpadModeRepository` /
  `TouchpadModeStore` / `TouchpadModeResolve` (ds4 > mouse > off ladder with
  the catalog ds4-mode gate) exist with contract + probe tests, and the
  descriptor path DECLARES the resolved mode (SDL touch-source detection →
  hub resolver → `attachSlot` → PUT; reconcile compares the applied mode).
  `mouseControl` remains false in v1 by decision — no UI sets a "mouse" pick.
- `isPrivateHostLiteral` guard → wired in `WifiConnectionManager::connectTo`.
- `LinkState::Unstable` → entered at 2 consecutive missed acks; recovers on
  the next ack. The "NOT YET ENTERED" notes above are historical.
- Reverse pairing: a Path-B `none` AFTER `pending` is now a terminal decline
  (satellite #68 removed the wire `denied`); early `none` tolerates the
  POST→first-poll race.
- Pairing TLS: `PairingClient` is TOFU-pinned via the shared pin store (the
  "later wave" note above landed).
- Catalog: legacy/absent-version bodies normalize via
  `core/catalog/LegacyCatalogTranslator.h` at the repository fill boundary;
  `catalogVersion`/`emulates`/per-feature `modes` parse; the emulate seed
  honors `emulates` (`core/reducer/EmulateSeed.h`, delegated from
  `seedControllerType`).
- The dead satellite download URL → `dish.tinkernorth.com/downloads/satellite`.
- SDL button labels → pinned positional (`SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=0`)
  so the SDL and USB-direct paths agree.

**REMOVED features (deliberate, with the Widgets tree):** the dismissible
DonatePill (+ its pure logic + `test_donate_pill.cpp`) — the flows design's
Settings carries a "Support Dish" row instead; the informational
SetupWizard/OnboardingNavBar screens — superseded by the live 3-step
`SetupGuideDialog`.

**Ports landed but NOT yet wired (headers + partial tests, integration
pending):** `core/input/UsbHidLayout.h` (android HidLayout/decodeFromLayout —
the HidP caps builder + gateway use are open), `core/reducer/CatalogFeatureGate.h`
(descriptor caps ∩ catalog type features — referenced only from the
`AppModel::resolveTouchpadMode` comment, no call site),
`core/catalog/BundledCatalog.h` (offline capability sets — reached only from
`LegacyCatalogTranslator.h` / `CatalogFeatureGate.h`, themselves unwired).

`composer/TouchpadModeComposer.h` was listed here in error: no such header was
ever written. The store-fold is unbuilt, consistent with the mouse-mode SKIP rows for
`TouchpadModeComposerTest` (§2.8 and the mouse-mode SKIP ledger); the hub resolver
computes the ladder directly. What DID land is the trio
`core/reducer/TouchpadModeResolve.h` + `repository/TouchpadModeRepository.h` +
`source/store/TouchpadModeStore.h`.

**Still-open android deltas (tracked for post-release).** Re-verified against
the tree at `3b8c822` (symbol-presence + call-site audit); the list below is
that audit, not the original 3ea439a snapshot.

- **Per-path rumble capability + the USB-direct OUTPUT write path** (rumble /
  lightbar encoders + gateway write). Today rumble is advertised on Direct
  with no actuator: `HidrawGateway` only *detects* the endpoint (from the
  interface's `bmAttributes` in sysfs; the quoted Windows
  `caps.OutputReportByteLength` is the dish-windows spelling) and nothing ever
  writes an output report. `hasOutEndpoint` has no reader.
- **`claim()` HID-collection re-check + ranking + the kKnown model-table
  port.** No `kKnown` table and no ranking/re-check symbols in the tree.
- **The six-layer capability fold (`Capability.kt`) + `RumbleEnabledStore` +
  host feature/runtime stores.** No `RumbleEnabledStore` under
  `src/source/store/`; rumble delivery at `AppModel`'s rumble handler is
  ungated, unlike the adjacent lightbar path (which honours
  `FeatureSettings::lightbarFollowGame`).
- **`GET /api/server/capabilities` consumption + the motion sink/backend feed
  (R10 wiring).** The client half EXISTS — `HTTPClient::getCapabilities` —
  but has zero callers in `src/` or `tests/`. The gap is consumption only.
- **Guided-setup live-state depth (`SetupUsb` recovery flows).** The wizard's
  Connect step went live in `ee8cb9c`; the USB recovery depth did not. No
  `SetupUsb` anything in the tree.
- **`SlotTopologyComposer` / `SlotTopologyController`.** Absent. NB the
  departed-device binding sweep this item used to be paired with LANDED in
  `ee8cb9c` + `e110a23` (see the post-addendum note below) — the two were
  split because only this half is still open.
- **Catalog prewarm-on-Live.** No `prewarm` anywhere in `src/`.
- **Stored-type clamp-to-catalog — the CLAMP ITSELF, not just its call
  site.** Previously worded as a missing call site, which understated it:
  `src/core/catalog/` holds only `BundledCatalog.h` +
  `LegacyCatalogTranslator.h`, and every `clamp` in the tree belongs to
  Backoff / BatteryRouting / LatencyWindow / Reconcile. Nothing to call yet.
- **QML translation fill.** Exactly 312 `type="unfinished"` entries in each of
  the five catalogs (`bs`, `de`, `es`, `fr`, `pt_BR`). English fallback ships.

**Deliberate exclusions reaffirmed (not regressions):** the Diagnostics
screen/inspector/probe/bench (the design's 16 frames exclude it; the latency
MECHANISM ships on connection rows), catalog images, the X25519 pairing
extension (contract-optional), the FakeSatellite integration layer (the
no-real-sockets invariant stands), the touch-capable "Needs Direct" nudge
(premise-invalid on Windows).

### Post-addendum fixes (2026-07-27, later the same day)

- **USB-direct unplug (stale controller card)**: nothing ever emitted
  `event::UsbUnplugged` — `onUsbGone` had zero callers, so a `Direct` pad
  survived its own unplug forever (the reducer's erase at `applyEvent` was
  unreachable; SDL-path pads were unaffected). `UsbGamepadManager::reconcile()`
  now runs a departed-device presence sweep (the polled Windows analog of
  android's `ACTION_USB_DEVICE_DETACHED`): tracked keys absent from
  `enumerate()` route through `onUsbGone` → erase + `releaseClaim` +
  `syntheticRemoved`. Three new manager-level tests in
  `test_usb_gamepad_manager.cpp` (Direct erase+release, Routed forget,
  replug re-evaluates auto-Direct); the §2.6 "no unplug scenario" gap is
  closed at the manager layer. Hardened same day: the sweep debounces to 2
  consecutive missed scans (one flaky enumeration pass must read as a blip,
  never a teardown of a live claim — same shape as the missed-ack Unstable
  rule), with a blip test. Found while chasing a "Claiming controller…"
  flap: `AppModel::pollUsbDirect()` compared `find()`/`end()` iterators from
  two separate BY-VALUE `controllers()` temporaries — UB that only executes
  once a Direct pad exists (debug CRT: "map/set iterators incompatible"
  assert ~2 s after a fast-lane pad claims; release: freed-heap reads, the
  0xc0000005 ntdll crash). Fixed with a single snapshot per pass.
- **Pairing sheet — both paths at once (android `PairPinDialog` parity)**: the
  forward/reverse split dialog (a tap-gated `reverseMode` flip; the reverse
  POST fired only on the "Show a PIN on this PC instead…" link) is replaced by
  the shared `src/qml/pages/PairingDialog.qml`: the 6-digit satellite-PIN
  field stays typeable throughout, and the 4-digit `clientPin` is POSTed
  automatically on open (android's "sent immediately so no extra tap"
  rule). New `App.pairingServerId` lets the parked-target auto-open drive
  both paths (it previously passed an empty id, killing Path B). Contract:
  `docs/QML_CONTRACT.md`, "Discovery, connect, pair".
- **Guided connect wizard (android `ui/setup` step-3 port)**: the wizard flow
  android replaced its onboarding with (#129) never existed on Windows in
  functional form — `SetupGuideDialog` was informational copy. Its Connect
  step is now the LIVE flow: scan-on-open (#125), Rescan, discovered-host
  rows opening the shared pairing sheet, the get-Satellite empty state, and
  auto-advance gated on the wizard's own pending host (the
  `SetupConnectionViewModel` "never advance on a background reconnect" rule,
  ported). Step 2 lists `App.slotModel` live. The USB/BT input-picker steps
  stay collapsed by design (pads auto-appear on Windows); `SetupUsb`-style
  recovery depth remains in the still-open list above.
- **FOUND/REMEMBERED one-spot rule**: a remembered satellite that was also in
  the current scan rendered TWICE on the Connections page — a FOUND row (raw
  `discoveredServers`) and its REMEMBERED row (the composer already marks it
  discovered via the `Ready` chip). The contract always said FOUND
  is the *not-yet-remembered* rest; the filter just never existed. Now
  `reducer::serversVisibleInFound` (`core/reducer/FoundVisibility.h`) drops
  any discovered id that already has a connections row (remembered ∪ live —
  the row-id universe, so a mid-pair live session collapses too, matching
  android where the found list is keyed off the same summaries map).
  `AppViewModel::discoveredServers()/foundCount` read through it, and
  `discoveredChanged` also re-fires when the connection-row id SET moves
  (pair lands / forget drops a row) — keyed on the id set so the 1 Hz latency
  ticks never churn the FOUND repeater. Pinned in
  `test_found_visibility.cpp` (7 cases: one-spot, scan order, DHCP-move fold
  on `mid:`, legacy `wifi:ip:port` fold, mid-pair suppression,
  forget-reappearance, input purity).

### The Home flow (design sync 2026-07-27 — frames 17/18 + the revised rail)

The design project grew a "06 · HOME" section (Home — signal path; Action
card — states) and revised the rail (Home on top, a pane "+ Add", Support
Dish as a pulse-pink heart destination above a gear-glyphed Settings).
Implemented end-to-end, screenshot-verified against the frames by driving the
built app:

- **Home destination** (`pages/HomePage.qml`, the new default): one wiring row
  per pad — pad card → wire (measured rate `·` one-way latency over the dish
  glyph; solid accent live, dashed outline dead/"idle") → satellite card (the
  the connection-row vocabulary) or the dashed "Bind…" ghost; the "+ Add a controller"
  invitation row; the keep-awake floating pill ("Streaming — do not close").
- **Data**: the slot model gained the bound-satellite JOIN roles (`satIp`,
  `satLinkState`, `satChip`, `satDotColor`, `satGlyph`, `satLatencyText`,
  `satLatencySamples`) — joined in C++ by `boundConnectionId` against the
  coordinator's derived rows, same render-token vocabulary + latency
  formatter/gate as `ConnectionListModel` (shared `qml/RenderTokens.h`, so the
  three surfaces cannot drift). Pinned in `test_slot_list_model.cpp` (6 new
  cases: join, unbound/vanished-row empties, samples gate, roles-scoped
  `dataChanged`, no-slot no-op). `App.streamingSlotCount` re-projects the
  EXISTING `composer::streamingSlotCount` (the wake rule) for the header.
- **Action-card vocabulary** (`kit/ActionCard.qml`): rest `primaryFill`,
  hover 18 %, pressed 24 %, keyboard focus solid-border + ring, disabled 0.4 —
  the frame-18 ladder; pane density = the rail's "+ Add" (opens the setup
  guide's Controller step via the new `openSetupGuideAt`; the wizard gained
  `initialStep`).
- **Bind chooser extracted** to the shared `pages/BindChooserDialog.qml`
  (ControllersPage + HomePage; behavior unchanged, ControllersPage's inline
  copy deleted). Hard-coded destination index updated (Connections → 2).
- **Pulse tokens**: `ThemePalette.pulse` (android `colorPulse` #FF6FB5; light
  AA-darkens to #C2417F per the palette's documented on-light shift) →
  `Theme.pulse`/`pulseFill`/`pulseEdge` in the QML bridge; the palette
  completeness test now covers it. DonatePage restyled to frame f-e3 (pulse
  hero + CTA + rails + condensed pays-for card, 330px/flex two-column grid);
  the rail heart is a ♥ text glyph in `Theme.pulse`. `gear.svg` added to the
  brand set (qrc) for Settings.
- **Fixes surfaced while driving the app**: fractional `font.pixelSize`
  literals fail qmlcachegen-compiled component creation (int-typed) — the
  restyled DonatePage silently refused to load until 10.5/11.5/12.5 became
  tokens; `BrandGlyph` now maps an empty glyph to an empty source (was
  warning `qrc:/brand/.svg` on every hidden-glyph create).
- Contract: `docs/QML_CONTRACT.md`, "The bound-satellite join".

---

## Android 2026-07/08 wave (#145–#159) — reviewed 2026-08-17

One row per substantive android PR merged 2026-07-15..2026-08-17, disposition
against this tree. Dependency bumps skipped. Same reading rules as the
post-audit ledger.

| ✓ | Android PR | Disposition | Notes |
|---|---|---|---|
| [x] | #145 send-counter nonce-reuse guard | already ported | Landed here as #23 (64-bit counter goes silent past 2^32−1, clamped view, proactive re-key at 0xF0000000). The warning/CI-hygiene layer is android-build-specific. |
| [x] | #146 gate rumble/motion on the active path | **ported (this wave)** | The rule ("a capability shows only where it fires"), re-derived for Windows' inverted actuator reality: SDL (Standard) drives rumble/lightbar and forwards motion/touch, the raw-HID claim (Direct) decodes motion/touch but drives nothing back. `CAP_RUMBLE` folds per-slot (SDL probe on Standard, never on Direct); the bind seams read `slotHardware` so a Direct claim advertises the motion/touchpad it decodes (they used to scan only the SDL list — CAP_MOTION and the touchpad render mode were silently dropped for every synthetic slot); `CapabilitySolver::linkCarries` tells the same per-path truth; the UI rumble chips read the probe. android's `modelFrameworkRumbleUnreliable` (Switch Pro phantom framework vibrator) has no Windows analog: SDL's HIDAPI switch driver actually drives the Pro's rumble. Pinned in test_capability_solver (per-path matrix), test_session_lifecycle (per-slot fold), test_satellite_client_motion (bit 0x0002). |
| [x] | #147 Android 17 Local Network Protections | SKIP (android-OS) | A runtime-permission model this platform does not have; Windows' firewall consent is OS-owned. |
| [x] | #149 USB setup recovery (SetupUsbViewModel) | SKIP (architecture) | The dead-ended modal "How should Dish read it?" screen does not exist here: pads auto-appear, the path pick lives on the card, and the FSM's NeedsReplug/RestoreStuck phases + notices already surface retry/replug inline (§2.6). No dead end to fix. |
| [x] | #150 classify USB-direct by interface descriptor | **HID arm ported (this wave)** | The XInput/GIP arms are unreachable: XUSB hides those pads from raw HID, so no VID:PID allowlist ever gated them here. The HID arm's Windows analog is decode-by-descriptor: GENERIC-HID claims now build a per-device field map from HidP preparsed caps (real usages, logical ranges, button indices) instead of the fixed-offset guess; `UsbHidLayout.h`'s pure parser/decoder is the vector-pinned reference (test_usb_hid_layout, android's fixtures byte-for-byte). The ranked-interface-selection arm maps to per-collection admission (`collectionMatchesParser`): Windows splits interfaces into separate HID collections, so ranking reduces to admitting only the collection the model's parser decodes. |
| [x] | #151 touch-capable "Needs Direct" nudge | SKIP (reaffirmed) | Premise-invalid on Windows: SDL forwards the DS4/DualSense touchpad on Standard, so there is nothing to nudge toward. |
| [x] | #152 DualSense + Switch Pro + thin-catalog | already ported + **prewarm closed (this wave)** | Types/thin-catalog landed as #29 + the release addendum. The one open tail — `CatalogPrewarmer` — is now `core/reducer/CatalogPrewarm.h` + `AppModel::prewarmCatalogs()`: warm once per Live stretch, re-armed by a drop, silent (never drives `catalogState_`). Pinned in test_catalog_prewarm. |
| [x] | #153 Amazon Luna Controller fast lane | SKIP (platform) | The Luna's wired mode is a byte-exact XInput device; on Windows the XUSB driver claims it and SDL serves it as an Xbox 360 pad already. Raw HID never sees it, so there is no table to add it to. |
| [x] | #154 Steam Controller over USB Direct | **ported (this wave)** | `HidParser::SteamController` (state decode incl. shared stick/pad axes + IMU self-gate), quiet/restore config sequences over HidD feature reports with the capped transient-failure retry, restore on every release path, dongle wireless connect (re-init) / disconnect (neutral publish) events, vendor-collection admission by model, `frameworkExpected=false` FSM arms (release settles to Routed instead of a false RestoreStuck — android's exact branches), and a claimable card for frameworkless models (no SDL twin exists to host the path control; same slot id as the future synthetic so the binding survives the claim). Never fast-laned. Decode/config vectors pinned 1:1 from android's hardware-verified suite (test_usb_report_parsers `[steam]`). android's foreground-service and boot-interface-rank arms are android-plumbing. NOT hardware-verified on Windows — see the PR body's Unverified section. |
| [x] | #157 fastlane metadata path / bs listing | SKIP (Play CI) | Google Play release plumbing; this repo ships via GitHub Releases + the installer. In-app Bosnian is unaffected on both platforms. |
| [x] | #158 claim a silent verified pad | SKIP (by construction) | There is no attach probe on Windows: claims open the collection and start the read loop, interrupt reads stay pending until data arrives, and the departed-device sweep is presence-based. An idle event-driven pad claims and holds today, which is the behavior #158 built toward. |
| [x] | #159 Switch-layout remap for PDP pads | **Direct half ported (this wave)** | `switchOrderButtonBit` + the ZL/ZR trigger fold in the shared layout decoder, `ButtonOrder::Switch` on the five PDP rows in `kKnownModels` (0186 excluded, same reason), wired through the HidP button-index mapping. The Standard half (keycode remap) is SDL's jurisdiction on Windows — `SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=0` is already pinned so the two paths agree positionally. Truth tables + the Faceoff descriptor fixture + the western-scramble pin ported byte-for-byte (test_usb_hid_layout `[switch-order]`). |

**Still-open android deltas closed by this wave:** catalog prewarm-on-Live;
`UsbHidLayout.h` wiring (the HidP caps builder + gateway use now exist as
`HidrawGateway`'s descriptor parse); the per-path rumble capability HALF of the
"per-path rumble + USB-direct OUTPUT write path" item (capability truth is
done; the output write path itself — rumble/lightbar encoders + gateway
writes — remains open, and is why Direct still advertises no rumble).

---

## Android 2026-08-15..18 tail (post-#159) — reviewed 2026-08-18

Disposition of everything android merged after the #145–#159 wave above froze
(its review date, 2026-08-17). Same reading rules. Release chores (#160 Play
10100 notes + cosign 2.6.5, #161 Play 10101 notes) are SKIP (Play CI; no cosign
exists anywhere in this repo's workflows), as are the #155/#156 dependabot
bumps + their pin-map comment sync (android-repo dependencies; this repo's
action pins are its own).

| ✓ | Android change | Disposition | Notes |
|---|---|---|---|
| [x] | #162 decode the Guide button bit in decodeXInput360 | SKIP (platform) + audited | No XInput-360 raw decoder exists here: XUSB hides that family from raw HID and SDL delivers Guide for it. Audited every decoder this repo DOES carry for the same dead-bit class — DS4 `buf[7]&0x01`, DualSense `buf[10]&0x01`, Switch Pro Home `0x10`, Steam `kSteamGuide`, HidP layout button 11 — all decode Guide (test_usb_report_parsers / test_usb_hid_layout pin each). No gap. |
| [x] | wired path on a Bluetooth card when the cable is present (9c26557, + the 4267500 dual-presence ghost characterization tests) | **re-derived (this wave)** | The android gap is permission-shaped: without a USB grant the attach-time auto pick parks a Routed ghost silently, the BT card hides the whole path section, and nothing represents the cable — hence the pill + "Use wired" action. Windows has no permission broker: a plugged twin of a fast-lane model claims Direct immediately (`hasPermission` is always true; pinned by "a verified model with no recorded failure auto-claims Direct"), and a non-fast-lane wired twin gets its own SDL card carrying the path control — the wired path is taken or visibly offered without new UI, so the pill/action does not port. What the scenario surfaced HERE instead, both landed this wave: **(a)** count-based twin-dedup could hide a same-model **Bluetooth** pad while the claimed pad's USB SDL twin kept streaming (two pads, the BT one attached first) — `RoutedDevice` now carries `bluetooth` and the hide priority is USB-before-BT, transport outranking `disconnecting` (a BT instance is never the claimed device; the gateway refuses BT paths), with a lone BT twin still suppressible for the single-pad dual-presence case (SDL's serial dedup can keep the BT-flagged instance). Pinned in test_usb_twin_dedup. **(b)** the "a BT slot never wears its USB twin's path control" rule lived as an `if` at the rebuild call site — moved into `slotPathFields` (the mapper layer android pinned its new BT cases on) and pinned in test_slot_path_fields, including the claimed-twin arm. |
| [x] | MainViewModelTest UsbGamepadManager.controllers stub | SKIP (android test plumbing) | Scaffolding for their ANDROID-tagged VM row after the fix above; the Windows analogs are the reducer pins in (a)/(b). |

**Unverified on hardware (recorded, same status as the #154 note):** whether a
given dual-mode pad streams input on its USB link while a BT session is live
varies by model (the Luna reportedly stays BT-silent-cable; Sony pads stream on
the plugged link). The reducer rules above are correct for both worlds: a
streaming USB twin is suppressed, a silent one costs nothing, and the BT
instance survives whenever it is not the model's last twin.
