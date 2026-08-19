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

### Fixed

- The beacon parser's `service` check is a structured JSON field read rather
  than a substring probe of the raw body, so a crafted beacon cannot spoof the
  service name through an unrelated string field. `[wire-coordinated]` — the
  same fix is worth carrying to `dish-windows`.

### Removed

- The Qt Widgets UI (`MainWindow`, `ConnectionsPage`, `PairingPage`,
  `SettingsView`, `SlotCard`, `ErrorBanner`, `NotificationToastStack`,
  `DishLoaders`, `BrandIcon`) and the `Qt6::Widgets` dependency. No `QWidget`
  is constructed anywhere.
- `AppModel`'s god-object role: state now lives in sources, composers and
  repositories, and `AppModel` is the composition root plus the hot-path seam.
