# Changelog

All notable changes to the Dish Linux client are documented in this file. The
format is loosely based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The version number has one source, `project(Dish VERSION ...)` in
[`CMakeLists.txt`](CMakeLists.txt), which becomes the `DISH_VERSION` compile
definition the in-app About surface and the update check read.

Cross-repo coordination: changes to the wire protocol or the pairing flow that
need matching updates in `satellite`, `dish-android`, `dish-windows` or
`dish-mac` are marked `[wire-coordinated]`. Releases tagged in lockstep across
the repos share a version number.

---

## [Unreleased]

### Added

- The unidirectional-dataflow architecture the sibling clients use: the
  header-only kernel in `src/architecture/` (`Observable`, `StateSource`,
  `Composer`, `Controller`, `Repository`), pure reducers and mappers in
  `src/core/`, `src/source/` state sources and IO gateways, `src/repository/`
  durable storage, and `src/composer/` derivations and effect controllers. The
  layering, the primitives and the rules for choosing between them are in
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
  [`src/architecture/README.md`](src/architecture/README.md).
- A Qt Quick UI replacing the Qt Widgets one: a collapsible navigation rail
  over a `StackView`, in-scene dialogs and a single toast host, a guided
  five-page setup wizard, first-run onboarding, per-device control remapping,
  deadzone editing, a capability table, and a licenses screen. The component
  kit and its review-blocking rules are in
  [`docs/QML_UI_KIT.md`](docs/QML_UI_KIT.md); the C++/QML boundary is in
  [`docs/QML_CONTRACT.md`](docs/QML_CONTRACT.md).
- Light and dark palettes that follow the desktop through the XDG appearance
  portal, with a System/Light/Dark preference. Sixteen semantic tokens per
  palette plus derived washes, all documented in [`DESIGN.md`](DESIGN.md) and
  gated by WCAG contrast tests over the real palette values.
- USB-direct input over `hidraw` for DualSense, DualShock 4, Switch Pro,
  8BitDo and Steam Controller class pads, driven by the same pure
  `UsbPathMachine` FSM the other clients use. Unlike the Windows raw-HID path,
  generic pads decode through the canonical report-descriptor parser, because
  `HIDIOCGRDESC` hands back the real descriptor.
- A udev rule ([`packaging/udev/70-dish-hidraw.rules`](packaging/udev/70-dish-hidraw.rules))
  granting the logged-in seat access to the supported models' hidraw nodes.
  Without it a claim fails with a distinct permission error and the pad stays
  on the SDL path.
- Bluetooth adapter presence and power detection over sysfs and BlueZ, so the
  setup wizard can tell "no adapter" from "adapter switched off".
- Reduced-motion support, read from the XDG settings portal and then
  `kdeglobals`, defaulting to motion allowed when neither answers.
- An update check that surfaces a newer release and links to it. See below.
- A crash handler that writes a backtrace to `$XDG_STATE_HOME/dish/crash.log`
  and re-raises on the default disposition, so core-dump collectors still work.
- A sixth translation catalogue (English), needed because a `%n` message
  carries one source string but needs one plural form per category.
- `scripts/qml-lint-literals.sh` and `scripts/check-translations.sh`, both
  gated in CI, plus `qmllint` over every tracked QML file.
- [`docs/PACKAGING.md`](docs/PACKAGING.md), [`PRIVACY.md`](PRIVACY.md),
  [`THIRD_PARTY.md`](THIRD_PARTY.md), [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md)
  and this file.
- Native installers for every mainstream Linux desktop, all generated from the
  same `install()` rules through CPack so a package cannot disagree with
  `cmake --install` about what ships: `.deb` (Debian 13+), `.rpm` (Fedora, RHEL,
  openSUSE), a self-contained AppImage
  ([`scripts/build-appimage.sh`](scripts/build-appimage.sh)), a Flatpak manifest
  on the KDE runtime, and a `.tar.gz`. The `.deb` and `.rpm` release jobs
  install their own package and launch it before uploading.
- AppStream metadata
  ([`packaging/com.tinkernorth.Dish.metainfo.xml`](packaging/com.tinkernorth.Dish.metainfo.xml)),
  a man page, a Debian `copyright` file and the licence texts, all installed.
  GNOME Software and KDE Discover list nothing without the first, and LGPL-3.0
  §4(b) plus OFL-1.1 §2 make the last mandatory for redistribution.
- `latest.json`, emitted by the release workflow and published under the fixed
  name the in-app update check fetches. It was never produced, so every check
  404'd; the job now also refuses to publish when the git tag and
  `project(Dish VERSION ...)` disagree.
- A `version-consistency` workflow gating the CMake version against the
  AppStream `<release>`, the CHANGELOG heading, the update policy floor and the
  five documents that mirror the Qt floor.
- CI: a clang leg beside gcc, ASan/UBSan and TSan legs, the Release
  configuration actually running the suite, a coverage report, a packaging job
  that lintians and launches the built `.deb`, and ccache plus a cached Qt
  install to pay for them.
- Hardening flags — `-fstack-protector-strong`, `_FORTIFY_SOURCE=3`, full RELRO,
  a non-executable stack and PIE — spelled out rather than inherited from a
  distro's patched GCC defaults, and asserted on the release binary in CI.

### Changed

- The Qt floor moved from 6.2 to 6.7, which the Qt Quick UI requires. Ubuntu
  24.04 LTS ships 6.4 and now needs a backport or a Flatpak build; see
  [`docs/PACKAGING.md`](docs/PACKAGING.md).
- The update flow is check-and-notify only. A distro package or Flatpak owns
  the binary, so there is no download, no staging directory and no boot-time
  apply — the reducer has six reachable phases where dish-windows has nine, and
  the *Download updates automatically* preference is gone because there is
  nothing for it to control.
- `Util/ScreenWakeController` was replaced by `composer/WakeStateComposer` plus
  `composer/WakeStateController`, splitting the derivation from the effect.
- The `.deb` is generated by CPack from the install rules rather than assembled
  by hand in the release workflow. The hand-rolled one shipped the binary and a
  misnamed desktop entry: no icon, no udev rule, no licence text, and a
  `Depends:` list naming packages that do not exist on the distro it was built
  on and omitting every QML module, which the app loads by name at run time.

### Fixed

- The installed `.deb` renders its brand glyphs. Every SVG failed to decode
  because Debian splits `imageformats/libqsvg.so` out of `libqt6svg6` into
  `qt6-svg-plugins`, which nothing depended on; the plugin is opened by name at
  run time, so no linkage revealed it. The packaging gates now fail on a decode
  error rather than warning about one.
- Logout and shutdown no longer discard the session's settings. SIGTERM's
  default disposition killed the process where it stood, so `~AppModel` never
  ran: the SDL input thread was not stopped and `QSettings` never wrote what
  the session changed. The signal is now delivered through a self-pipe and
  quits the event loop, so `main` unwinds the way it does on a normal exit; a
  second signal still kills, so a shutdown that wedges is not unkillable.
- The beacon parser's `service` check is a structured JSON field read rather
  than a substring probe of the raw body, so a crafted beacon cannot spoof the
  service name through an unrelated string field. `[wire-coordinated]` — the
  same fix is worth carrying to `dish-windows`.
- USB-direct is no longer invisible without the udev rule. `HidrawGateway` now
  enumerates from the world-readable sysfs attributes instead of opening
  `/dev/hidraw*` read-only, so a pad the rule does not cover still appears and
  the claim reports `PermissionDenied` — the outcome the UI, the gateway header
  and `docs/PACKAGING.md` all already described. Previously the device never
  enumerated at all and the user saw nothing.
- The five PDP wired Switch pads decode in the right button order over
  USB-direct. `claim()` never set `switchOrderButtons`, so physical A landed on
  X, ZL lost its trigger, and R3/Home/Capture vanished.
- A changed satellite certificate is a terminal, named failure instead of
  "Server unreachable" retried on the backoff curve forever. The TOFU verifier's
  mismatch hook had no caller; `RestVerdict`/`PairVerdict::IdentityChanged` now
  carry it to a no-retry arm with copy that names the remedy.
- The update check reacts to connectivity again. `ReachabilityChanged` had no
  producer, so the offline gate never fired: a captive portal's HTML splash
  surfaced as "the release information didn't parse", and a laptop rejoining
  Wi-Fi waited out the full backoff ladder instead of rechecking in 30 s.
- Skipping an update no longer persists when the release is required, which
  could mute the unsupported-build warning permanently.
- The clock-skew escape in the update schedule is reachable. An `elapsed >= 0`
  term made it dead code, so a stored timestamp one millisecond in the future
  took the same path as one a century out.
- A laptop whose only battery has an unreadable capacity reports an unknown
  level rather than "desktop, fully charged", and a pack that reports neither
  charging nor discharging below the full threshold now sends `Discharging`
  like the other clients instead of `Unknown`.
- Keep-awake holds a logind idle inhibit as well as the screensaver one, so a
  desktop whose suspend timer is independent of screen blanking can no longer
  suspend mid-stream.
- `sendto` on the input thread is `MSG_DONTWAIT` with a soft drop on a full
  buffer; a blocking send was measured stalling 1.5 s across a Wi-Fi power-save
  transition.
- One unparsable datagram on the beacon port no longer suppresses that address
  for the rest of the discovery window.
- `cmake --install` puts the udev rule where udev reads it. The relative
  `CMAKE_INSTALL_SYSCONFDIR` resolved against the prefix, so a `/usr` install
  landed it in `/usr/etc/udev/rules.d`.
- The rail-collapse toggle draws its glyph at the kit size instead of filling
  the button, and it and the update pill carry the mandated focus ring.
- The test suite builds the translation catalogues before asserting against
  them; the dependency the tests documented was never actually wired.
- The action-pin linter's regex no longer aborts under the `mawk` that is
  `/usr/bin/awk` on a stock Ubuntu 24.04, and it now covers
  `.github/actions/**` as well as `.github/workflows/**`.

### Removed

- The Qt Widgets UI (`MainWindow`, `ConnectionsPage`, `PairingPage`,
  `SettingsView`, `SlotCard`, `ErrorBanner`, `NotificationToastStack`,
  `DishLoaders`, `BrandIcon`) and the `Qt6::Widgets` dependency. No `QWidget`
  is constructed anywhere.
- `AppModel`'s god-object role: state now lives in sources, composers and
  repositories, and `AppModel` is the composition root plus the hot-path seam.
