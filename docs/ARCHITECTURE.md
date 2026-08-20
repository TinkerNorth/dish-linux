# Architecture

A map of the codebase aimed at people working in it. It says which primitive to
reach for, where the code goes, and what the layering rules are.

Related documents: [`QML_CONTRACT.md`](QML_CONTRACT.md) is the exposure surface
the Qt Quick UI binds against, [`QML_UI_KIT.md`](QML_UI_KIT.md) is the component
kit and design tokens, [`../src/architecture/README.md`](../src/architecture/README.md)
documents the kernel primitives themselves, and [`PACKAGING.md`](PACKAGING.md)
covers how a build becomes an installed Dish.

Data flows one way. A source of truth owns state, pure reducers and composers
derive from it, the UI binds and renders it, and the UI sends commands back to
the source. The UI never owns domain state, and the core never imports Qt Quick.

## The prime directive: core is independent of the UI

The app is split so that you could delete `src/qml/` and write a different
front-end (a CLI, a tray applet, another toolkit) without touching a file under
`src/core/`, `src/source/`, `src/repository/`, or `src/composer/`. That is the
test for every change.

- **Core layers are Qt-Quick-free and mostly Qt-free.** `src/core/` is fully
  Qt-free: it compiles and unit-tests on the host with no Qt, SDL, or sodium.
  The source, repository, and composer layers may use `QObject`, `QString`, and
  `QSettings` for IO and value types, but they never import a QML type or call
  `tr()` on a user-facing string. Localization happens at the UI edge, so those
  layers carry render keys and reason codes as data rather than pre-localized
  text.
- **The UI is a projection.** `src/qml/` is a thin adapter: it maps
  already-derived state onto bindings and forwards user intents verbatim. It
  holds view state (which page is open, which row is expanded) but never domain
  state.

```
        ┌───────────────────────────────────────────────────────────────┐
        │  qml/       projection + intents, swappable                    │
        │             binds state ▲              │ emits commands        │
        └───────────────────┼──────────────────────┼────────────────────┘
                            │                      ▼
        ┌───────────────────┴──────────────────────────────────────────┐
        │  composer/  Composers (pure derive) · Controllers (effects)   │
        │             · Coordinators (imperative command surface)       │
        └───────────────────┬──────────────────────┬───────────────────┘
              reads Observables                  drives
        ┌───────────────────┴──────────┐ ┌────────┴────────────────────┐
        │  source/  StateSources +      │ │  repository/  durable       │
        │           Gateways (IO edge)  │ │  storage (QSettings)        │
        └───────────────────┬──────────┘ └─────────────────────────────┘
                            │ uses
        ┌───────────────────┴──────────────────────────────────────────┐
        │  core/      PURE: reducers (FSMs + mappers), models,          │
        │             AsyncState, wire/crypto, input math. No Qt.       │
        └───────────────────┬──────────────────────────────────────────┘
                            │ built on
        ┌───────────────────┴──────────────────────────────────────────┐
        │  architecture/  the kernel: Observable · StateSource ·         │
        │                 Composer/Combiner · Controller · Repository    │
        └──────────────────────────────────────────────────────────────┘
```

**Dependency rule:** arrows point up. A lower layer never includes a higher one.
`core/` knows nothing of `source/`; `source/` knows nothing of `composer/`;
nothing below `qml/` knows `qml/` exists. [`AppModel`](../src/AppModel.h) is the
composition root and the only place allowed to wire concrete instances of every
layer together.

## The folder map

| Folder | Role | May depend on | Qt? |
|---|---|---|---|
| `src/architecture/` | The kernel primitives | nothing | **No** |
| `src/core/` | Pure domain: reducers and FSMs (`reducer/`), models (`model/`), the type catalog (`catalog/`), input math (`input/`), wire framing and session crypto (`wire/`), certificate trust-on-first-use pinning and address literals (`net/`), plus `AsyncState` | `architecture/` | **No** |
| `src/source/` | `StateSource`s (owned reactive state) and Gateways (native IO boundary) | `core/`, `repository/` | yes (IO) |
| `src/repository/` | Durable keyed storage over QSettings, dumb and synchronous | `core/` | yes (QSettings) |
| `src/composer/` | Composers (pure derive), Controllers (side effects), Coordinators (command services) | `core/`, `source/`, `repository/` | yes (QObject) |
| `src/qml/` | QML bridge: the `AppViewModel` facade, role models, chrome singletons, and the `.qml` tree | everything | yes (Quick) |
| `src/Models/` | Shared value types (`models::ControllerSlot`, `DiscoveredServer`, notifications) | `core/` | yes (QString) |
| `src/Util/` | Leaf helpers with no domain state: endian, hex, host battery, display-sleep inhibitor, locale install | nothing | mixed |
| `src/UI/` | The design-token palette (`Theme`), the font-family probes (`FontStacks`), crash handling, the `SlotLiveStats` mapper, `common/ExternalLink`, `licenses/LicenseManifest` | `core/` | yes (Gui) |
| `src/Input/` | The SDL bridge, the input processor, joystick mapping, the output command queue | `core/` | yes |
| `src/Network/` | Sockets and the REST control plane: `SatelliteClient`, `ConnectionHub`, `WifiConnectionManager`, `HTTPClient`, `PairingClient` | `core/` | yes |
| `src/update/` | The update checker's IO edge: the manifest gateway (a dedicated QNAM) and `UpdateChecker`. Dish never installs its own update, so there is no download, staging or apply path | `core/`, `source/` | yes |

`src/Input/` and `src/Network/` predate the layer model and keep their
capitalized names. New IO belongs in `src/source/` as a `StateSource` or a
Gateway.

When you add code, the folder is the contract. If it has no Qt and makes a
decision, it belongs in `core/`. If it owns a socket, timer, or cache, it is a
`source/` `StateSource` or a Gateway. If it persists, it is a `repository/`. If
it purely derives one value from others, it is a `composer/` Composer. If it
performs a side effect off a state, it is a `composer/` Controller.

## The kernel primitives

Five header-only C++17 primitives in [`src/architecture/`](../src/architecture/README.md),
ported from the `StateFlow` / `AbstractStateSource` / `AbstractComposer` /
`AbstractController` / `Repository` set in
[dish-android](https://github.com/TinkerNorth/dish-android).

| You have… | Use |
|---|---|
| a value that changes over time, observed by others | `Observable<S>` |
| state owned from a socket, timer, cache, or setting | `StateSource<S>` |
| one value purely derived from other Observables | `Composer<Out, Ins...>` / `Combiner` |
| a side effect driven by a state | `Controller<S>` |
| durable keyed storage | `Repository<K,V>` |

The kernel README covers their semantics (distinct-until-changed, subscription
lifetime, teardown policy, the repository property tests). Three more
conventions are classes you write per feature rather than base classes:

- **Reducer / Mapper.** A pure free function in `core/` for a
  `(state, event) -> result` decision or a domain-to-UI shape. This is the unit
  of testability.
- **Coordinator.** A plain `QObject` service for imperative cross-cutting
  commands over several sources. It re-exposes a child's `Observable` by
  reference and never mirrors it, because a mirror is a second writer. See
  [`ConnectionCoordinator`](../src/composer/ConnectionCoordinator.h).
- **Gateway.** An IO or native boundary with no domain state, for example
  [`HidrawGateway`](../src/source/usb/HidrawGateway.h) or
  [`MdnsDiscovery`](../src/source/connection/MdnsDiscovery.h).

## <a name="async-state"></a>Capturing async state: `AsyncState<T>`

Every asynchronous operation has four states, and the UI must be able to bind
all four. A `bool busy`, a `QSet<id> inFlight`, or an `std::optional<T>` that
silently drops failures cannot tell *empty* from *still loading*, and cannot say
why something failed.

[`core/AsyncState.h`](../src/core/AsyncState.h) is the container:

```cpp
template <class T, class E = std::string>
struct AsyncState {
    AsyncPhase phase;            // Idle | Loading | Success | Error
    std::optional<T> data;       // last good value, RETAINED across refresh/error
    std::optional<E> error;      // populated iff Error: a typed reason, not a string
    bool stale;                  // data carried from a prior success
};
```

Transitions are pure: `asyncIdle()`, `toLoading(prev)`, `toLoadingFresh()`,
`toSuccess(prev, value)`, `toRevalidated(prev)` (HTTP 304), and
`toError(prev, reason)`. It is `==`-comparable, so it drops straight into an
`Observable<AsyncState<T>>` and gets distinct-until-changed for free.

Two properties make it the right tool:

1. **Stale-while-revalidate is built in.** `toLoading` and `toError` keep the
   prior `data` and flag it `stale`. A refresh shows a spinner over the
   last-known content; a failed refresh shows that content with an error chip,
   never a blank screen.
2. **Failure carries a typed reason.** `E` defaults to `std::string`, but prefer
   an enum (`CatalogError`, `PairFailure`) so the UI localizes a code, matching
   the render-keys-not-strings rule.

The worked example is the catalog fetch.
[`SatelliteCatalogRepository`](../src/source/http/SatelliteCatalogRepository.h)
delivers `AsyncState<CatalogDto, CatalogError>`: a fresh 200 becomes `Success`,
a 304 becomes `Success(stale)`, and an unreachable, 5xx, or malformed reply
becomes `Error(reason)` still carrying the cached catalog.
[`AppModel::refreshCatalogForSlot`](../src/AppModel.cpp) flips it to `Loading`
before the GET and stores the terminal state, so the Emulate picker binds a
spinner, a typed error, and an empty-versus-content distinction. `AppViewModel`
projects that as `emulateLoading` / `emulateError` / `emulateStale`.

**Use `AsyncState<T>`** for a request and response with a clear in-flight to
terminal shape: a catalog fetch, a discovery scan, a one-shot connect attempt, a
forward-pairing submit. **Do not use it** for a multi-state domain lifecycle
with more than four states or domain-specific terminals (a live session that
goes Linking, Live, Faltering, Stale, Reconnecting). That is a reducer FSM.

## <a name="reducer-fsm"></a>Capturing lifecycles: the reducer FSM

For a lifecycle richer than Idle/Loading/Success/Error, model it as a pure,
total reducer FSM. The reference implementation is
[`core/reducer/UsbPathMachine.h`](../src/core/reducer/UsbPathMachine.h), which
drives USB input-path switching.

The shape:

```cpp
// state + event  ->  next state + effects (as DATA, not performed here)
struct Reduction { std::optional<UsbController> next; std::vector<UsbEffect> effects; };
Reduction reduce(const UsbController& c, const UsbEvent& event);
```

The rules that make it robust:

1. **Pure and total.** `reduce` performs no IO and is defined for every
   (phase × event) pair, so a weird or failed transition can never silently drop
   state the way scattered imperative branches can. Events go in as a
   `std::variant`; effects come out as one.
2. **Effects are returned as data.** `reduce` decides what should happen
   (`Claim`, `Release`, `Notify{reason}`, `MarkRestoreStuck`). The coordinator,
   [`UsbGamepadManager`](../src/source/usb/UsbGamepadManager.h), turns world
   signals into events, runs `reduce`, and executes the effects against real
   subsystems. Decisions are testable without hardware, and IO lives in one
   place.
3. **One unidirectional notify.** After every state-changing event the
   coordinator fires a single "state moved" signal and the UI rebuilds from the
   fresh snapshot. Granular per-effect callbacks are the alternative, and they
   miss transitions that produce no effect (a held-synthetic phase change), so
   the list never rebuilds and a toggle appears stuck.
4. **Loading and error are derived state, not events.** "Switching" is a pure
   predicate over the phase (`slotPathSwitching`, surfaced as the
   `claimInProgress` role). Failure is a phase (`RestoreStuck`, `NeedsReplug`)
   plus a reason (`DirectClaimFailure`) carried on the controller and stamped
   onto the slot. Only the transient "what just happened" banner (`UsbNotice`)
   is routed to a toast.
5. **Exhaustively unit-tested.** One assertion per (phase × event); see
   `tests/test_usb_path_machine.cpp` and `test_usb_path_machine_edge_cases.cpp`.
   Because `reduce` is pure, the whole decision space is pinned with no mocks.

Machines that follow this pattern and are wired into the running app:

| Machine | Coordinator |
|---|---|
| [`UsbPathMachine`](../src/core/reducer/UsbPathMachine.h) | `UsbGamepadManager` |
| [`ApplyBindingMachine`](../src/core/reducer/ApplyBindingMachine.h) | `AppViewModel::applyBinding` |
| [`BindingPresence`](../src/core/reducer/BindingPresence.h) | `AppModel::rebuild` |
| [`CapabilitySolver`](../src/core/reducer/CapabilitySolver.h) | `AppViewModel::capabilityForCandidate` |
| [`UpdateMachine`](../src/core/reducer/UpdateMachine.h) | `UpdateCoordinator` |

Three more exist as pure, tested specifications with no live coordinator yet;
see [Not yet implemented](#not-yet-implemented).

`UpdateMachine` is the newest and the most conventional application of the
pattern: phases Disabled through Ready and Failed, events for every arrival
from the network, the preferences and the connectivity probe, and effects
(`FetchManifest`, `StartDownload`, `VerifyAndPromote`, `DiscardStaged`,
`SweepStaging`, `ScheduleNextCheck`, `Notify`) that `UpdateCoordinator`
executes against a dedicated `QNetworkAccessManager` on the main thread and a
worker thread that owns a second one for the payload. Scheduling constants live
on the machine, not in the coordinator, so the backoff ladder and the yank rule
are pinned by unit tests rather than observed by waiting.

### Choosing between them

| | `AsyncState<T>` | Reducer FSM |
|---|---|---|
| Shape | request, in-flight, one terminal | many domain states and transitions |
| States | exactly Idle/Loading/Success/Error | bespoke (Routed/Claiming/Direct/AwaitingFramework/…) |
| Effects | the caller performs them inline | returned as data, executed by a coordinator |
| Examples | catalog fetch, discovery scan, connect attempt | USB path switch, binding apply, binding presence |

## Unidirectional flow: the rules

1. **One owner per piece of state.** It lives in exactly one `StateSource` or
   FSM and is exposed read-only. Nobody else stores a copy. A Coordinator
   re-exposes `composer.state()` by reference; it does not mirror it, because
   mirroring is a two-writer race.
2. **Derive, do not mutate.** Downstream values are produced by a pure
   `Composer` from upstream Observables and recomputed on change. Do not
   hand-patch a derived field in place from an event handler.
3. **Effects as data, executed at the edge.** A reducer returns what to do; a
   coordinator or Controller does it.
4. **One notify, then rebuild.** State change, single signal, UI rebuilds from
   the snapshot. No granular per-field back-channels that can miss a transition.
5. **Distinct-until-changed everywhere.** Every state value is `==`-comparable
   so a no-op emit is suppressed. This is why `AsyncState`, every composer row,
   and every FSM state implement `operator==`.
6. **Commands flow the other way, explicitly.** The UI calls an intent
   (`bindSlot`, `pairByServerId`, `setSlotPath`); that becomes an event or
   effect; the resulting state flows back out. The UI never writes state
   directly.

## <a name="ui-contract"></a>The UI binding contract: bind all the states

A screen that can load, fail, or be empty must render all four. The kit
components in [`src/qml/kit/`](../src/qml/kit/) exist so every page does this
the same way.

| State | Kit component | Source |
|---|---|---|
| Loading / in-flight | `LoadingSpinner`, `DishProgressBar` | `AsyncState::isLoading()` or a `*InProgress` role |
| Error / failure | `ErrorBanner` (message, detail, optional Retry) | `AsyncState::isError()` plus a typed reason localized in QML |
| Empty | `EmptyState` (title, body, optional action) | `Success && data.empty()` |
| Success / content | the real content | `AsyncState::data` |
| Transient event | `NotificationToastHost`, one host at the shell | `App.errorMessage` and other one-shots |

Doctrine for QML pages:

- **Never represent a result as the absence of content.** "No rows" must
  distinguish loading (spinner) from empty (`EmptyState`) from failed
  (`ErrorBanner`). Bind the phase, not just the list length.
- **One toast host, at the shell.** `NotificationToastHost` is dropped once in
  `AppShell.qml` and listens to `App.errorMessage`. Pages raise a transient
  notice through `shellApi.toast(message, severity)` rather than dropping a
  second host into their own tree.
- **Bind reactive properties, not polling getters.** Expose a `Q_PROPERTY` with
  a `NOTIFY` folded off the owning Observable or signal. Do not make QML call a
  point-in-time `isFooInFlight()` and hope a broad signal re-evaluates it.
- **Worked example:** [`ControllersPage.qml`](../src/qml/pages/ControllersPage.qml)
  with [`SlotListModel`](../src/qml/SlotListModel.h). The `claimInProgress` role
  drives the spinner and the disabled toggle; the phase drives the error note.

[`AppViewModel`](../src/qml/AppViewModel.h) is the seam: it maps C++ state onto
`Q_PROPERTY` and role models and forwards intents verbatim. It stays thin, with
no derivation, no edge detection, and no domain logic. Derivation that creeps in
here belongs in a `core/` mapper or a `composer/` Composer, where it is
unit-testable without the Quick stack.

## Threading and the hot path

The kernel notifies on the thread that called `set`. When binding from a
background thread, marshal to the GUI loop with `Qt::QueuedConnection` or
`QMetaObject::invokeMethod`.

The threads in the app:

| Thread | Owner | What runs on it |
|---|---|---|
| GUI | `QGuiApplication` | Everything in `qml/`, `composer/`, the stores, `AppModel::rebuild` |
| SDL input | [`SDLGamepadBridge`](../src/Input/SDLGamepadBridge.h) | The SDL event pump, `GamepadInputProcessor::publish`, and the `sendto` that follows it |
| USB direct | `UsbGamepadManager` per claimed device | Raw-HID URB reads, feeding the same publish path |
| Heartbeat | [`SatelliteClient`](../src/Network/SatelliteClient.h) | Per-session keepalive sends and ping arming |
| Receive | `SatelliteClient` | Ack decode, RTT sampling, rumble and lightbar callbacks, close-notify |

**The input hot path is intentionally not routed through the kernel.** The
input, decode, encode, `sendto` path is plain C++ with no queue, no Qt event
hop and no cross-thread signal. It is not allocation-free: `sendEncrypted`
still heap-allocates its inner frame per report
([`SatelliteClient.cpp`](../src/Network/SatelliteClient.cpp)), which is worth
removing since the payload never exceeds 17 bytes. Its
cross-thread seams are explicit and narrow:

- `AppModel`'s `routing_` and `motionRouting_` tables (per-slot senders), guarded
  by `routingMtx_` ([`AppModel.h`](../src/AppModel.h)).
- The bridge's `suppressedMtx_` (which device ids USB-direct has taken over) and
  `remapMtx_` (the per-`(vid,pid)` joystick remap), each on its own mutex so a
  GUI-thread write never contends with the per-report read.
- [`OutputCommandQueue`](../src/Input/OutputCommandQueue.h). Rumble and lightbar
  arrive on the receive thread but every `SDL_GameController*` is resolved and
  used only on the SDL thread, so the request is queued and drained by
  `runLoop()`.

Do not add Observables, `tr()`, or QML to that path. Everything around it
(device hotplug, capability, capture mode, path switching) is modelled state and
goes through the kernel.

## One front-end

The Qt Quick app is the app: `src/qml/`, entry point `runQmlApp`, hosted by
`QGuiApplication`. There is no build option to select a front-end and no widget
tree anywhere in the binary.

`src/UI/` keeps its name but holds only shared non-view infrastructure: the
`Theme` palettes (plain `QColor` data plus the OS appearance reader),
`FontStacks`, `CrashHandler`, the `SlotLiveStats` mapper,
`common/ExternalLink`, and `licenses/LicenseManifest`. None of it draws.

The view model and both role models live in the `dish_core` library rather than
in the Quick target, which is what keeps them unit-testable without standing up
a QML engine.

## How to add a feature

1. **Name the state.** What changes over time? Is it a request (use
   `AsyncState<T>`) or a lifecycle (use a reducer FSM in `core/reducer/`)?
   Define the value type with `operator==`, and reasons and render keys as data.
2. **Write the pure core first.** The reducer or mapper plus its exhaustive
   test. No Qt. It must be green before any wiring exists.
3. **Own it in a `source/` `StateSource`**, or a Coordinator for cross-cutting
   commands. Feed world signals in; expose `state()` out.
4. **Derive UI shape in a `composer/` Composer** if more than one upstream
   contributes. Side effects become a `Controller`.
5. **Project it in `AppViewModel`** as a reactive `Q_PROPERTY` with a `NOTIFY`,
   plus the intent invokables. Keep it thin, and record it in
   [`QML_CONTRACT.md`](QML_CONTRACT.md).
6. **Bind all states in QML** using the kit. No result-as-absence.
7. **Wire it once in `AppModel`**, the only composition root.

## <a name="not-yet-implemented"></a>Known limitations and not yet implemented

Each entry names the decided shape, so a future change lands on it rather than
re-litigating it.

**Pure specifications with no live coordinator.** These are exhaustively unit
tested and describe the target behaviour, but nothing feeds them events yet.
Wiring them rewrites the live UDP session loop and the SDL input threading,
which the unit suite cannot exercise (no socket, no satellite, no controller in
CI), so each needs a device-in-the-loop test pass.

- [`SatelliteSessionMachine`](../src/core/reducer/SatelliteSessionMachine.h):
  Discovered, Pairing, Linking, Live, Faltering, Reconnecting, Stale,
  Failed(reason), modelled on `UsbPathMachine`, with `retryAttempt` and
  `nextRetryAtMs` on the state so the UI can show "Reconnecting (n)". It exists
  because `WifiConnectionManager` currently has a `Faltering` state nothing
  reaches, invisible reconnect backoff, and no captured failure reason.
  `WifiConnectionManager` is meant to become its coordinator.
- [`PairingMachine`](../src/core/reducer/PairingMachine.h): Idle, Submitting,
  Succeeded, Failed(reason), consuming the existing pure `classifyPair` verdict.
  `Succeeded` is reached only on a real `SessionConfirmedLive` event, replacing
  the "infer success from a rising online count" heuristic.
- [`CaptureMode`](../src/core/input/CaptureMode.h): Idle and
  Capturing(slot, target), a single owner for the press-to-assign lifecycle,
  reusing the bridge's capture-threshold predicates. Today the state is split
  between an atomic flag on `SDLGamepadBridge` and `AppViewModel`'s
  `capturingSlotId_`.

**Composition and ownership.**

- `MainUiState` is hand-mutated on `AppModel` and patched in a second place by
  `onInputRatesChanged`, which is a two-writer arrangement. It should be a
  top-level Composer over the slot, connection, pairing, and USB Observables,
  carrying a per-flow `AsyncState` rather than one `busy` bool.
- [`MotionCapabilityComposer`](../src/composer/MotionCapabilityComposer.h)
  exists and is tested but nothing consumes it. It needs the device-list
  Observable fed in.
- The SDL `devices()` getter plus its content-free `devicesChanged()` signal
  should be a `StateSource<vector<DeviceSnapshot>>`. Distinct-until-changed
  would end the battery-poll rebuild storm and close the cross-thread
  time-of-check/time-of-use gap on the device list.
- `AppModel` still performs work that belongs in dedicated coordinators: USB
  framework-presence driving, the catalog and Emulate flow, and the remap,
  deadzone, and rumble pushes. Extracting them would leave it a pure
  composition root plus the hot-path seam. Keep the `routingMtx_` tables intact
  when doing so.

**Product gaps.**

- **High contrast.** Needs a third palette plus a bridge onto the desktop's
  high-contrast setting, an `Appearance::HighContrast` arm, and a pass over
  every place the kit derives a wash from the accent, since high contrast
  forbids alpha washes. A partial pass is worse than none, because a
  high-contrast user cannot tell which surfaces to trust.
- **Toast actions.** `NotificationToastHost` is a queue with no per-toast action
  channel, so there is no Undo. Adding one needs an action slot on
  `models::DishNotification` and the queue, plus a callback lifetime story for a
  toast that outlives the page that raised it.
- **Per-binding rumble.** No rumble store exists; rumble rides the descriptor
  caps. `App.rumbleEnabledFor` returns `true` and `App.setRumbleEnabled` is a
  no-op. The Feel row still renders, because the capability verdict for it is
  real and hiding the row would hide true information. A fix mirrors
  `MotionEnabledStore`, keyed per binding, read by the descriptor assembly in
  `ConnectionHub::bind`.
- **No positive bind-accepted edge.** `ConnectionHub::bind` applies the binding
  locally and the satellite answers asynchronously; only the failure is typed
  (`slotRegistrationFailed`). `ApplyBindingMachine` therefore reads success as
  "the binding held and the session is live" on a 250 ms tick inside an 8 s
  budget. A slow satellite can still reject after the overlay closes, which the
  rollback toast covers. The fix is a per-controller accepted signal out of
  `WifiConnectionManager`'s controller PUT, fed in as `BindAccepted`.
- **SDL type slugs are absent from the emulation-type ladder.**
  `reducer::seedControllerType` matches a catalog `emulates` hint on either the
  pad's USB `vid:pid` or an SDL type slug (`ps4`, `ps5`, `switchpro`,
  `xbox360`). `SDLGamepadBridge::Device` carries no type slug, so only the
  `vid:pid` half ever answers. A pad absent from the catalog's hint list
  degrades to the catalog's first offered type, which is correct by fallback
  rather than by identity. The fix is `SDL_GameControllerGetType` to a slug on
  the bridge's `Device`, threaded into `currentTypeForConnection` as the second
  key; the reducer already takes both.

## Testing

- **Pure core is the contract.** Every reducer, mapper, and `AsyncState`
  transition has a Catch2 test that pins the full decision space with no mocks
  (`test_async_state`, `test_usb_path_machine`, `test_pairing_machine`,
  `test_catalog_repository`, `test_capability_solver`,
  `test_apply_binding_machine`, `test_binding_presence`, `test_capture_mode`,
  `test_satellite_session_machine`). If logic is not testable without standing
  up a live `QObject`, socket, or SDL, it is in the wrong layer: move the
  decision into `core/`.
- **The design system is tested.** `test_theme_store` pins palette completeness
  (every dark role has a light value, and they differ). `test_theme_contrast`
  computes WCAG 2.1 ratios over the real palette values in both palettes, so a
  token that reads on dark and vanishes on light fails the build rather than a
  screenshot. `test_font_stacks` pins the family probe that keeps mono off
  Courier New. `scripts/qml-lint-literals.sh` keeps raw pixel values inside
  `src/qml/kit/`.
- **Translations are tested.** `test_translations` pins locale fallback, the
  per-language `numerusform` order, and placeholder integrity across every
  catalogue. `scripts/check-translations.sh` re-runs `lupdate` in CI and fails
  on any diff, so a new string cannot land without its catalogue entry.
- **The update checker is tested the same way.** Test names are prefixed so
  `ctest -R update` selects them: the exhaustive reducer table for
  `UpdateMachine`, the manifest grammar, the preference schema, and
  `UpdateChecker` driven against a fake gateway with an injected clock.
- **Probes for the kernel.** `StateSourceProbe`, `ComposerProbe`, and
  `ControllerProbe` capture emission sequences rather than just final values.
  `RepositoryContract` runs the property tests every repository must pass.
- One `DishTests` executable links `dish_core`. `scripts/build.sh test` builds
  and runs it. Keep it green at every step.
</content>
</invoke>
