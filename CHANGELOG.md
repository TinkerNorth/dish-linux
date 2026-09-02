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

- **Controller audio, wave 1: wire + capability model** `[wire-coordinated]`
  (satellite's `MSG_MIC_AUDIO`/`MSG_SPEAKER_AUDIO`/`MSG_MIC_LED`; dish-android
  shipped the client reference; dish-windows companion). This lands the
  protocol-2 audio extension's plumbing without yet turning any audio on:
  - the wire: `MSG_MIC_AUDIO` (0x0012) send path, `MSG_SPEAKER_AUDIO` (0x0013)
    and `MSG_MIC_LED` (0x0014) dispatch, the `mic`/`speaker` descriptor caps,
    and the datagram ceilings (1500-byte receive buffer, 1472-byte inner
    payload guard) a full-size audio frame needs;
  - the pure cores: the 2-frame reorder window (`core/audio/AudioJitter.h`,
    the third mirror of satellite's — edit together) and the pinned Opus
    formats (mic mono VOIP 32 kbps DTX, speaker stereo AUDIO 96 kbps, both
    VBR + in-band FEC) behind `core/audio/AudioCodec.h`, libopus-backed in
    `source/audio/OpusAudioCodec.*` (new system dependency: `libopus`, found
    through `pkg-config` like SDL2 and libsodium);
  - the host verdict: `GET /api/server/capabilities` is now probed after every
    session PUT for the `controllerAudio` block (per-backend `audio` fallback),
    so the capability table's mic/speaker host layer reflects what the host
    will actually carry — conservative "no audio" until a probe says yes;
  - the model and UI: Microphone and Controller sound rows in the capability
    matrix and per-binding toggles (mic defaults OFF for privacy, speaker ON),
    persisted like the motion toggle; `wButtons` bit 0x0800 reserved as the
    DualSense mic-mute state.
- **Controller audio, wave 2: the audio itself** `[wire-coordinated]`. A
  Direct-claimed DualSense (or DS4 v2) now carries real audio end to end:
  - pad-to-endpoint routing: the claimed pad's USB product string (iProduct,
    read from sysfs — the same string pipewire and pulse embed in the pad's
    audio device names) is matched against the endpoints SDL enumerates, with
    every ambiguity resolving to "no route" (two pads sharing a name,
    duplicate endpoint names, a name containing two pads' strings) — routes
    re-resolve on claim changes and audio hotplug, and a change re-declares
    the slot's descriptor;
  - the capture engine: the pad's own headset mic, windowed to exact 20 ms
    frames, Opus-encoded and sent as `MSG_MIC_AUDIO`, one seq per window
    including failed encodes. THE PRIVACY INVARIANT: muted, toggled off,
    unrouted, unstreaming or unwelcome at the host means the capture device is
    CLOSED and zero packets leave — never silence in their place;
  - the playout engine: `MSG_SPEAKER_AUDIO` through the reorder window and
    Opus FEC/PLC to the pad's own speaker endpoint, with a two-frame start
    cushion rebuilt as silence after the satellite's suppressed-silence
    stretches;
  - the DualSense mute button: decoded as an edge onto a latch that folds the
    mute STATE into `wButtons` (0x0800) on the read thread, mirrored to the
    app's mute state, stripped from Moonlight's button words; the slot card
    and Configure binding show the local truth with a click-to-toggle control,
    and the pad's mute lamp answers locally at once (a later host `MSG_MIC_LED`
    repaints it — last writer wins on the pad);
  - `MSG_MIC_LED` actuation: the DS5 lamp + mic-amp power-save bit, shadowed in
    the per-claim feedback state so rumble/lightbar/player-LED/trigger writes
    re-assert it instead of stomping it;
  - `SDL_INIT_AUDIO` is owned by the audio gateway, not the gamepad bridge, so
    gamepad re-inits never take a live stream down; the bridge's event loop
    forwards audio hotplug. The Flatpak gains the `pulseaudio` socket (served
    by `pipewire-pulse` on PipeWire desktops), and the AppImage's bundled SDL
    is now built with its audio subsystem on.
  Mute is deliberately session-scoped (not persisted): it clears when the pad
  leaves, the way the hardware's own mute does; the durable off-switch is the
  per-binding Microphone toggle, which still defaults OFF.

- **Protocol 2** `[wire-coordinated]` (satellite #86, #87; dish-android #174,
  #175; dish-windows companion). The version is now negotiated rather than
  assumed: the client offers 2, the satellite settles the session on that
  offer and echoes it back, and the
  *settled* version keys the wire frames. A satellite that predates versioning
  echoes 1 and keeps working on the v1 frames. A 409 whose range still overlaps
  ours is re-offered at the satellite's ceiling instead of dead-ending, and one
  that does not names which end has to update.
- **The POINTER frame** (0x000C, v2, 19 bytes). The touchpad click moved out of
  the finger flags into a buttons byte and a signed vertical wheel was appended.
  A physical pad's touch surface reports one click and no wheel, so the right,
  middle and wheel fields ride as zero rather than being synthesised from
  gestures the user never made.
- **Feedback to a Direct-claimed pad.** The USB-direct claim path gained an OUT
  report path, so a raw-HID claim now drives the pad as well as reading it:
  - rumble and the RGB lightbar, which previously fired only on the SDL path;
  - `MSG_TRIGGER_EFFECTS` (0x0010) replays the game's own DualSense
    adaptive-trigger blocks verbatim;
  - `MSG_PLAYER_LEDS` (0x0011) drives the DualSense indicator bar and the Switch
    Pro's player lights.
  The descriptor advertises the actuator, not the hardware: a capability is
  claimed only where a report would actually land, so the satellite never sends
  into a path that would drop it.
- **Moonlight touch.** A bound pad's touchpad now reaches the host as
  `CONTROLLER_TOUCH` events: the full-state frame is diffed into per-pointer
  DOWN / MOVE / UP, with a tracking-id change closing the old contact before
  opening the new one.
- **Moonlight motion is subscription-gated.** Samples go out only after the host
  asks with a `MOTION_EVENT`, per (pad, motion type) and at the requested rate.
- **Moonlight trigger rumble** is folded onto the pad's body motors and
  advertised, so a game whose only haptics are trigger effects is no longer
  silent. No pad this client can claim has impulse-trigger motors: xpad binds an
  Xbox pad as evdev-only and publishes no hidraw node for it, and every family
  that does enumerate has two body motors and nothing in the triggers. The fold
  is the honest maximum rather than a shortcut. The two host rumble streams mix
  per motor by maximum, so neither can cancel the other.

### Changed

- **Build system: local builds and CI run the same rails.** `CMakePresets.json`
  (new) carries the `debug`, `release` and `package` configure lines;
  `linux-ci.yml`, `codeql.yml` and `release.yml` call the presets, the shared
  `scripts/check-format.sh` gate, and the new `scripts/build-deb.sh` /
  `scripts/build-rpm.sh` packaging scripts instead of inline copies (matrix
  compilers stay env-injected CC/CXX and the ccache launcher stays
  workflow-side, so presets never pin what the matrix varies). Locally:
  `scripts/install-deps.sh` (the README apt list plus CI's pinned
  clang-format; `--ci-qt` installs the exact Qt 6.9.3 CI builds against),
  `scripts/build.sh` rewritten onto the presets (the debug tree is now
  `build/`, CI's name, instead of `build-debug/`; the pre-commit hook's
  `clang-tidy -p` target follows), and `scripts/ci_local.sh` renamed to
  `scripts/ci-local.sh` (a forwarder keeps the old name) with its known gaps
  closed: the Debug and Release configures now carry
  `DISH_REQUIRE_TRANSLATIONS=ON` like CI, qmllint gains CI's
  `-I "$QT_ROOT_DIR/qml"` include, and a `--compiler gcc|clang` flag
  reproduces either side of CI's compiler matrix.

### Fixed

- Moonlight motion samples were forwarded as the satellite's raw fixed-point
  int16 cast to a float, where the wire wants deg/s and metres per second
  squared. A pad at rest read correctly and a moving one read as spinning at
  tens of thousands of degrees per second.
- A Moonlight-bound pad never declared `CAP_BATTERY` and never forwarded its
  charge level, though the pad was already publishing one.
- Moonlight motion was gated on a single session-wide flag, so one game
  opening one sensor started the stream for every pad on that host, at
  whatever rate the hardware polled rather than the rate the host asked for.
- An absent `protocolVersion` in a satellite response was read as this build's
  version rather than as 1, which would have made a pre-versioning satellite
  look like it had agreed to frames it cannot decode.

## [0.2.0] - 2026-08-24

### Added

- Every release now also uploads its packages under version-less stable
  names: `dish-amd64.deb`, `dish-x86_64.rpm`, `Dish-x86_64.AppImage` and
  `Dish-x86_64.flatpak`. `releases/latest/download/<name>` is therefore a
  permanent link to the newest build, alongside `latest.json` which already
  worked that way. The stable names are covered by SHA256SUMS and the cosign
  signatures, and they are a public API: download pages link them, so they
  must not be renamed or dropped.

## [0.1.2] - 2026-08-24

### Fixed

- The tray item no longer advertises the `com.tinkernorth.Dish` icon name when
  the icon theme cannot resolve it. StatusNotifier hosts prefer `IconName` over
  `IconPixmap`, so a run without the hicolor icons installed (a source build,
  the AppImage) rendered GNOME's "..." missing-icon placeholder in the top bar
  even though pixmaps were shipped. The name is now advertised only when the
  theme lookup succeeds, `IconPixmap` carries the brand mark pre-rasterised at
  22 and 48 px and compiled into the client (replacing the hand-drawn
  stand-in glyph), and the tooltip carries the same icon fields.

## [0.1.1] - 2026-08-23

### Fixed

- The AppImage bundles Qt's Wayland client buffer integrations
  (`wayland-graphics-integration-client`). linuxdeploy's qt plugin deploys the
  shell-integration and decoration plugins but not this directory, and a
  Wayland session with zero buffer integrations aborts on the first expose;
  0.1.0's AppImage only started under `QT_QPA_PLATFORM=xcb`. The build now
  fails if any Wayland or xcb platform piece is missing from the bundle.

## [0.1.0] - 2026-08-23

### Added

- Configurable keep-awake, under *Power* in Settings. **Never**, **While
  playing** (the default: hold only while a bound controller has actually been
  actuated inside a 1–180 minute idle window, 5 by default) or **While
  connected** (hold for as long as a slot streams, however long the pad sits
  still). The hold now covers the **machine** by default and the display only on
  request, because forwarding a pad needs the computer, not the panel. Activity
  is measured post-deadzone against a reference that only advances when it
  moves, so a drifting stick cannot pin the machine awake and a slow deliberate
  push still registers. The streaming pill names the reach it actually holds and
  carries a **Configure** button through to the setting.
- Running in the background. Closing the window leaves Dish streaming behind a
  StatusNotifierItem tray icon whose menu is the way back and the way out, and a
  one-time desktop notification says so the first time it happens. The hide is
  gated on a StatusNotifier host actually owning the watcher name, so a desktop
  without one keeps quitting on close rather than stranding a running process
  with no window and no menu; the item re-registers when the panel restarts.
  *Keep running in the background* in Settings turns it off.
- Suspend and resume handling. A logind `sleep`/`delay` lock buys time on
  `PrepareForSleep` to close the satellite sessions before the machine goes
  down, and a resume rescans and re-opens them instead of waiting out the ~10 s
  heartbeat death. Tearing down first is load-bearing: a session the machine
  slept through comes back `Faltering`, which slips past both reconnect guards,
  so a bare reconnect would open a second socket beside the frozen one. A closed
  lid still suspends — `LidSwitchIgnoreInhibited` defaults to `yes`, so that is
  a `logind.conf` setting rather than something an application can hold.
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

- Settings live in one XDG-shaped file, `$XDG_CONFIG_HOME/com.tinkernorth.Dish/dish.conf`,
  keyed by the same reverse-DNS app id as the desktop entry, the AppStream
  metainfo and the Flatpak. Every store used to default-construct
  `QSettings("Dish","Dish")` — a Windows registry path carried over from the
  dish-windows port — so the app wrote both `~/.config/Dish/Dish.conf` and
  `~/.config/TinkerNorth/Dish.conf`, and the documented one held a single UI flag
  while the other held the device id, the remembered satellites and the pairing
  keys. Both are folded into the new file on first launch, key by key and never
  over one already present, then renamed `.migrated`.
- Crash reporting is described as what it is. Collection has always been local
  and nothing has ever been transmitted, so the Settings toggle no longer says
  reports are shared, and a crash now surfaces a report the user reads and sends
  themselves. The text has the home directory, IPv4 literals, `.local` hostnames
  and any hex run of 32 characters or more removed before it is shown, so a
  pairing key on the stack cannot be pasted into a public issue.
- A satellite that cannot be reached says why. Refused, timed out and a TLS
  failure were all one sentence about checking the power; a refused connection
  now points at the satellite not running instead. The cause is diagnostic only
  — the retry verdict is unchanged for every one of them.
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

- Pairing keys are stored in the desktop keyring over `org.freedesktop.secrets`
  rather than as plaintext hex in a world-readable config file. gnome-keyring
  and KWallet both implement it and QtDBus was already linked, so this adds no
  dependency; where no Secret Service answers the config file is still used, now
  0600. Two paths that would have left a plaintext copy behind are closed: the
  legacy `wifi_shared_key/` entries are removed once migrated, and a key still in
  the file while the keyring already holds one is swept rather than skipped.
- A remembered satellite that is switched off no longer floods the log. The REST
  gateway read replies that were never opened, printing
  `QIODevice::read (QNetworkReplyHttpImpl): device not open` on every backoff
  tick; it now writes one line per cause change, with per-tick detail behind
  `QT_LOGGING_RULES="dish.net.debug=true"`.
- `sudo cmake --install` puts the hidraw rule somewhere udev reads.
  `DISH_UDEV_RULES_DIR` was relative, so off the packaging prefix it resolved to
  `/usr/local/lib/udev/rules.d`, which udev does not scan — the rule installed
  silently and every USB-direct claim failed `PermissionDenied` afterwards. The
  default now depends on the prefix; the packaged path is unchanged.
- Error messages in `WifiConnectionManager` and `PairingClient` can be
  translated. `QCoreApplication::translate` was called with a variable context,
  which lupdate cannot resolve, so none of those strings were ever extracted and
  all of them shipped in English regardless of locale. Twelve strings are
  recovered, nine of which predate this change.
- The `⋯` overflow menus open. A Menu takes its width from its background, and
  both the Connections host menu and the Home row menu restyled that background
  without restating a width, so the menu opened at zero width — it took focus
  and drew nothing, which is indistinguishable from a dead button. Both are now
  sized to their widest item over a shared `Tokens.menuMinWidth` floor, the same
  way `ComboButton` already did it.
- A skipped test no longer reads as a failed one. Catch2 exits 4 from `SKIP()`
  and CTest was never told what that means, so the keyring round-trip — which
  skips wherever no Secret Service answers, including every CI runner — turned
  the whole suite red on any machine without a desktop keyring.
- Configuring is warning-free. Qt's QML import scanner links plugins only for a
  static Qt, and Debian and Ubuntu ship the plugin targets undefined, so a shared
  build printed thirteen "link target does not exist" warnings that no
  configuration could satisfy; the scan is now skipped where it cannot act.
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
