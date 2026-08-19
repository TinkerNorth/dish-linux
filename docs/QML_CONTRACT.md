# QML contract

The exposure surface between the C++ core and the Qt Quick UI. Everything the
QML tree may read or call is listed here; anything not listed is private to
C++ and the UI must not reach for it.

The companion documents are [`QML_UI_KIT.md`](QML_UI_KIT.md) for the component
kit and the design tokens, and [`ARCHITECTURE.md`](ARCHITECTURE.md) for the
layering the surface sits on top of.

## How the surface is reached

Three names are in scope for QML:

| Name | What it is | Import |
|---|---|---|
| `App` | [`AppViewModel`](../src/qml/AppViewModel.h), the one model surface | none, it is a context property |
| `Theme` | the colour palette | `import Dish.Chrome` |
| `Tokens` | the non-colour design tokens | `import Dish.Chrome` |

`App` is registered on the root context by
[`QmlEntryPoint::runQmlApp`](../src/qml/QmlEntryPoint.cpp), so it is visible in
every QML scope with no import. `Theme` and `Tokens` are registered by instance
into the `Dish.Chrome` module. There is no window-chrome bridge: the window
manager draws the decorations, and the shell's own header is the only chrome
Dish paints.

`SlotListModel` and `ConnectionListModel` are also registered uncreatable under
`Dish.Chrome`, so a delegate can name the type. You rarely need to: bind
`App.slotModel` or `App.connectionModel` straight into a `ListView.model`.

### The one accepted lint gap

`App` is a runtime context property. Static analysis cannot see it, so every
`App.foo` reference is an unqualified lookup as far as `qmllint` is concerned.
CI therefore runs `qmllint` with `--unqualified info` and gates every other
category at error, including `missing-property`, `unused-imports` and
`unresolved-type`. This is the only downgrade, and this document is the
compensating control: a reference to `App` is checked against the table below
rather than by the linter.

The gap closes when `AppViewModel` becomes a compiled QML singleton instead of
a context property. Until then, do not add a second context property; anything
new goes on `App`.

## `App` properties

`AppViewModel` is a thin adapter. Every property is a re-projection of state
already derived in a C++ store or composer, and every method forwards verbatim.
It adds no behaviour of its own.

### Header and counts

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `statusText` | `string` | `stateChanged` | Dashboard headline: "No connections yet", "\<n\> remembered", the single live server label, or "\<n\> online". |
| `summaryText` | `string` | `stateChanged` | Header sub-line: "Tap Manage to add one", "\<n\> remembered", "\<n\> of \<m\> online". |
| `onlineCount` | `int` | `stateChanged` | Connections whose link is `Connected`. |
| `connectionCount` | `int` | `stateChanged` | Total remembered plus live connections. |
| `slotCount` | `int` | `stateChanged` | Rows in `slotModel`, mirrored so a binding never pokes the model for a count. |
| `boundSlotCount` | `int` | `stateChanged` | Slots currently bound to a connection. |
| `streamingSlotCount` | `int` | `stateChanged` | Slots bound **and** whose link is `Connected`. The same pure `composer::streamingSlotCount` rule the wake controller inhibits the display on, so the header and the keep-awake pill cannot disagree. |
| `firstOnlineName` | `string` | `stateChanged` | Label of the first `Connected` connection; empty when none. |
| `foundCount` | `int` | `discoveredChanged` | Size of the FOUND list, after the one-spot exclusion below. |
| `keepAwakeActive` | `bool` | `stateChanged` | The display-sleep inhibitor is held. Drives the shell's streaming pill and the quit confirm. |
| `busy` | `bool` | `stateChanged` | A controller is registering. |

Header sub-lines are assembled in QML from these primitives, so the wording
stays in the `qsTr` catalogues. Pages own their own copy; see the per-page
`headerSub` bindings.

### Telemetry

Sampled about once a second off the input processor.

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `eventsPerSec` | `int` | `telemetryChanged` | Input events per second. |
| `sendsPerSec` | `int` | `telemetryChanged` | Wire sends per second. |
| `totalSent` | `qulonglong` | `telemetryChanged` | Cumulative reports sent since launch. |

### Collections

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `slotModel` | `SlotListModel*` | CONSTANT | One row per controller slot. See below. |
| `connectionModel` | `ConnectionListModel*` | CONSTANT | One row per derived connection. See below. |

The property is `slotModel`, not `slots`: `slots` is the `Q_SLOTS` keyword and
moc strips it.

### Discovery and pairing

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `discoveredServers` | `list` | `discoveredChanged` | The FOUND list, as JS objects `{ name, ip, udpPort, pairPort, httpPort, machineId, source, id }`. `source` is the discovery-source label ("UDP broadcast", "mDNS", "mDNS + broadcast"). |
| `scanning` | `bool` | `scanningChanged` | A discovery scan is in flight. Gate the Scan button on it. |
| `pairingActive` | `bool` | `stateChanged` | The model parked a pairing target. Open the pairing sheet on the rising edge, and call `clearPairingTarget()` before showing it. |
| `pairingServerName` | `string` | `stateChanged` | Display name of the parked target; empty when `!pairingActive`. |
| `pairingServerId` | `string` | `stateChanged` | Stable id of the parked target. Capture it, with the name, **before** `clearPairingTarget()`; the sheet needs it for both pairing paths. |
| `reversePairingPhase` | `string` | `reversePairingChanged` | `"idle"` / `"awaiting"` / `"approved"` / `"declined"` / `"timedout"`. |
| `reversePairingPin` | `string` | `reversePairingChanged` | The 4-digit PIN to show while `awaiting`. Stays set on the terminal phases so the sheet can keep showing it; cleared on the next `requestReversePairing` or `cancelReversePairing`. |
| `reversePairingServerName` | `string` | `reversePairingChanged` | Name, or ip when unnamed, of the server being reverse-paired. Empty when `idle`. |

**The one-spot rule.** `discoveredServers` excludes any server whose stable `id`
already has a `connectionModel` row. A box gets exactly one row in the UI: once
it is remembered, its reachability shows as that row's chip, not as a duplicate
FOUND entry. The two lists are disjoint by construction.

### Emulate catalog lifecycle

The catalog fetch is an `AsyncState<CatalogDto, CatalogError>`. These three
project it so the type picker can tell loading from empty from failed.

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `emulateLoading` | `bool` | `emulateStateChanged` | A catalog GET is in flight with nothing cached. |
| `emulateError` | `string` | `emulateStateChanged` | Localizable failure text; empty when there is no error. |
| `emulateStale` | `bool` | `emulateStateChanged` | The types on screen come from a prior success. |

### Settings

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `themeMode` | `int` (RW) | `themeModeChanged` | `0` Light, `1` Dark, `2` System. |
| `crashReportingEnabled` | `bool` (RW) | `crashReportingChanged` | Crash-reporting opt-out. Default on. |
| `railCollapsed` | `bool` (RW) | `railCollapsedChanged` | The nav rail's persisted collapse state. The title-bar hamburger writes it. |
| `lightbarFollowGame` | `bool` (RW) | `lightbarChanged` | Light-bar forwarding: true is "Follow game", false is "Off". |
| `onboardingNeeded` | `bool` | `onboardingNeededChanged` | The first-run welcome has not completed. Flips false after `markOnboardingComplete()`. |
| `appVersion` | `string` | CONSTANT | The CMake project version. |
| `donateSponsorsUrl` | `string` | CONSTANT | GitHub Sponsors URL. |
| `donateKofiUrl` | `string` | CONSTANT | Ko-fi URL. |
| `donateBmacUrl` | `string` | CONSTANT | Buy Me a Coffee URL. |

### Bluetooth radio

Two facts, not one, because an absent adapter and a switched-off radio need
different copy and only the second has an action.

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `bluetoothPresent` | `bool` | `bluetoothChanged` | A Bluetooth-class device exists. True even when the radio is switched off. |
| `bluetoothEnabled` | `bool` | `bluetoothChanged` | A radio handle opened. |

`bluetoothPresent && !bluetoothEnabled` reads "Bluetooth is off" and offers
`openBluetoothSettings()`. `!bluetoothPresent` reads "no adapter" and offers no
button. Neither ever blocks the setup wizard, because USB still works.

### Apply sequencer

One step per real asynchronous action, so the UI can show which one is running
and how long it has been running.

| Property | Type | NOTIFY | Meaning |
|---|---|---|---|
| `applyInFlight` | `bool` | `applyChanged` | A binding apply is running. |
| `applyConnectionState` | `string` | `applyChanged` | `"pending"` / `"active"` / `"done"` / `"failed"` / `"skipped"`, for the USB-path step. |
| `applyDestinationState` | `string` | `applyChanged` | Same vocabulary, for the REST round trip. |
| `applyElapsedMs` | `int` | `applyChanged` | Milliseconds on the **current** step. Show the slow hint past 4000. |
| `applyCancellable` | `bool` | `applyChanged` | True only while the Connection step is active. |

## `App` signals

| Signal | Args | Meaning |
|---|---|---|
| `stateChanged` | | Header, slot, connection or pairing state moved. |
| `telemetryChanged` | | Telemetry numbers moved (about 1 Hz). |
| `errorMessage` | `string message` | Transient one-shot failure. The shell's toast host listens; pages do not need to. |
| `discoveredChanged` | | The FOUND list moved: a scan landed, or the connection-row id set changed so the one-spot exclusion has to be re-read. |
| `scanningChanged` | | The `scanning` flag flipped. |
| `reversePairingChanged` | | A reverse-pairing transition. NOTIFY for all three `reversePairing*` properties. |
| `emulateStateChanged` | | The catalog fetch moved. |
| `themeModeChanged` | | `themeMode` moved. |
| `crashReportingChanged` | | `crashReportingEnabled` moved. |
| `onboardingNeededChanged` | | `onboardingNeeded` flipped. |
| `railCollapsedChanged` | | `railCollapsed` flipped. |
| `lightbarChanged` | | `lightbarFollowGame` flipped. |
| `deadzonesChanged` | | Deadzone device rows or their values moved. Re-pull `deadzoneDevices()`. |
| `bluetoothChanged` | | The radio presence/enabled pair moved. |
| `pairingSucceeded` | | One-shot: a connection reached `Connected` after a pair. The pairing sheet may close on it. Best effort. |
| `pairingFailed` | `string serverId, string reasonToken` | A forward PIN was rejected. `reasonToken` is `"wrongPin"` / `"versionMismatch"` / `"unreachable"` / `"pending"`. Match `serverId` against the sheet's own target before showing anything; the sheet stays open and marks the field inline. `errorMessage` also fires, and that duplication is intended. |
| `applyChanged` | | Any apply field moved. |
| `applyFinished` | `bool ok, string reasonToken, bool directFellBack` | Terminal, fired exactly once per run. `reasonToken` is `""` on success, else `"slotGone"` / `"hostUnreachable"` / `"bindRejected"` / `"cancelled"`. `directFellBack` means the Direct claim did not land and the pad streams over Standard: raise a **warning**, never an error. |
| `rawInputCaptured` | `string slotId, int kind, int index, int value` | A raw joystick input was observed for the slot currently capturing. `kind` is `0` axis, `1` button, `2` hat; `index` is the raw source index; `value` is the axis int16, `1` for a button, or the `SDL_HAT_*` bitmask for a hat. Fires only for the capturing slot. |

## `App` methods

### Slots and bindings

| Method | Args | Effect |
|---|---|---|
| `bindSlot(slotId, connectionId)` | `string, string` | Bind a slot to a connection. |
| `unbindSlot(slotId)` | `string` | Unbind the slot. |
| `setSlotPath(slotId, choice)` | `string, string` | Force the slot's USB input path: `"standard"` (SDL owns the pad), `"direct"` (raw-HID claim), `"auto"` (clear the override and let the resolution policy decide). No-op when the slot has no resolvable `(vid, pid)`. The slot roles refresh on the next `stateChanged`. Gate the control on the `pathSupported` role. |
| `availableConnectionsForSlot(slotId)` | `string` → `list` | Connections this slot may bind to, as `{ connectionId, label, dotColor, glyph }`. Computed by the pure `reducer::connectionsVisibleInPicker`: connections bound to another slot are excluded, live unbound ones are offered, and the slot's own current binding is held over even when offline. Read it once when a chooser opens; there is no NOTIFY. Gate a Bind button on this list being non-empty, not on `connectionModel.count`. |

### Emulation types

Two families: keyed on an existing binding, and keyed on a destination for a pad
that is not bound yet. The setup wizard and the binding editor need the second,
because the slot-keyed reads vend nothing before a binding exists.

| Method | Args | Effect |
|---|---|---|
| `refreshEmulate(slotId)` | `string` | Kick a best-effort catalog refresh for the slot's bound satellite. |
| `emulateTypes(slotId)` | `string` → `list` | Offerable types as `{ type:int, slug, name, shortName, description, known:bool }`. Empty when the slot is unbound or no catalog is cached. |
| `emulateCurrentType(slotId)` | `string` → `int` | The wire type id to pre-select: user override, else hardware class, else Xbox. |
| `refreshEmulateForHost(connectionId)` | `string` | The destination-keyed refresh. |
| `emulateTypesForHost(connectionId)` | `string` → `list` | The destination-keyed type list, same shape. |
| `emulateCurrentTypeForHost(connectionId, slotId)` | `string, string` → `int` | The destination-keyed pre-selection. |
| `setControllerType(slotId, type)` | `string, int` | Apply the choice and re-attach the slot so the new descriptor is sent. |

### Discovery, connect, pair

| Method | Args | Effect |
|---|---|---|
| `startDiscovery()` | | Begin a satellite discovery scan. |
| `isScanning()` | → `bool` | Point-in-time scan flag. Prefer the reactive `scanning` property for bindings. |
| `discoveredServers()` | → `list` | The FOUND list as an explicit re-pull. Prefer the reactive property. |
| `discoverySourceFor(serverId)` | `string` → `string` | The discovery-source label, addressable by id. |
| `connectByServerId(serverId)` | `string` | Connect to the discovered server with that stable id. Resolved out of the live list, so it cannot act on a stale index; a no-op when not found. |
| `reconnectConnection(connectionId)` | `string` | Reconnect a **remembered** satellite without a rescan and without re-pairing; the key persists. If the id is in the current scan it connects the fresh endpoint, otherwise it kicks a discovery relearn and tries the last-known endpoint now. Gate on the row **not** being `liveLink`. |
| `disconnectConnection(connectionId)` | `string` | Graceful disconnect of a live session **without** forgetting: the row and the pairing key survive. Gate on the row's `liveLink`. |
| `forgetConnection(connectionId)` | `string` | Forget a remembered connection: unbinds its slots and drops the key, the PIN and the row. |
| `pairByServerId(serverId, pin)` | `string, string` | Submit a 6-digit PIN for the discovered server with that stable id. Watch `pairingFailed` and `errorMessage` for failure, `isPairingInFlight` for the spinner. |
| `isPairingInFlight(serverId)` | `string` → `bool` | A pair request is in flight for that server id. |
| `clearPairingTarget()` | | Drop the one-shot pairing trigger. Call before opening the pairing sheet to avoid re-entry. |
| `requestReversePairing(serverId)` | `string` | Start host-initiated pairing: the dish shows a 4-digit PIN and the operator approves on the satellite. A second call cancels the first. |
| `cancelReversePairing()` | | Abort an in-flight reverse pair. Safe when idle. |

**Both pairing paths run at once.** Forward pairing is the operator reading a
PIN off the dish and typing it here. Reverse pairing is the inverse: the dish
shows a PIN and the operator approves on the satellite. The sheet runs both
simultaneously, and no tap gates either one.

On open it calls `cancelReversePairing()` (a stale phase from an earlier sheet
must not leak in) and then `requestReversePairing(server.id)` immediately, so
the operator is notified the moment the sheet opens. The 6-digit field stays
typeable throughout as the live fallback, submitting through `pairByServerId`.
Show the reverse block only while `reversePairingPhase !== "idle"`: an
unresolvable id leaves the phase idle and the sheet degrades to forward-only,
which is what a parked target whose satellite has left the scan looks like. On
`"approved"`, close. On `"declined"` or `"timedout"`, show the reason and offer
a new code. The poll budget is about two minutes, and a momentary network blip
during the wait does not abort it.

### Input tuning

| Method | Args | Effect |
|---|---|---|
| `deadzoneDevices()` | → `list` | Per-device rows `{ id, name, hasGyro, stickFlat, triggerFlat, forwardMotion }`. Re-pull on `deadzonesChanged`. |
| `setDeadzones(deviceId, stickFlat, triggerFlat)` | `string, int, int` | Persist the per-device override and push it into the live processor. |
| `setMotionEnabled(deviceId, on)` | `string, bool` | Persist motion forwarding, keyed by device id. |
| `motionEnabledFor(slotId)` | `string` → `bool` | The read seam for the same flag. A binding draft seeds from this rather than from the default, so applying cannot silently re-enable gyro the user turned off. |
| `slotRemap(slotId)` | `string` → `map` | The slot's effective raw-joystick remap: the stored override, else the default layout. Returns `{}` for a slot with no resolvable identity, which includes SDL-recognised game controllers and USB-direct synthetics. See the shape below. |
| `assignSlotInput(slotId, target, kind, index)` | `string, string, int, int` | Apply a capture result through the pure `withAssignment` helper and persist it. The store pushes into the live bridge, so it takes effect on the next report with no re-attach. |
| `setSlotInvert(slotId, which, on)` | `string, string, bool` | Flip a stick Y-invert flag. `which` is `"leftY"` or `"rightY"`. |
| `resetSlotRemap(slotId)` | `string` | Drop the stored override and clear it in the live bridge. |
| `startInputCapture(slotId)` | `string` | Arm capture and point the filter at that slot. Calling it for another slot re-points the filter. An axis fires only on a deliberate move, buttons on press, hats on a non-centred direction, so a resting pad never self-assigns. |
| `stopInputCapture()` | | Disarm capture. Call it when the user leaves the page. Safe when idle. |

`slotRemap` returns, with every index a raw source index and `-1` meaning
unassigned:

```js
{
  a, b, x, y,
  dpadUp, dpadDown, dpadLeft, dpadRight,
  leftShoulder, rightShoulder, back, start, leftThumb, rightThumb,   // int
  leftStickX, leftStickY, rightStickX, rightStickY, hatIndex,        // int
  leftTrigger, rightTrigger,        // { kind: "axis"|"button", index: int }
  invertLeftY, invertRightY         // bool
}
```

`assignSlotInput`'s `target` is one of `a`, `b`, `x`, `y`, `dpadUp`,
`dpadDown`, `dpadLeft`, `dpadRight`, `leftShoulder`, `rightShoulder`, `back`,
`start`, `leftThumb`, `rightThumb`, `leftStickX`, `leftStickY`, `rightStickX`,
`rightStickY`, `leftTrigger`, `rightTrigger`. Pass `kind` and `index` through
from `rawInputCaptured` verbatim. A trigger target tags its source from `kind`:
an axis capture becomes an analogue axis, a button capture becomes digital
full-scale-on-press. A hat capture on a dpad target routes the dpad to that hat;
a button capture routes that direction to the button. An unknown `target` is a
no-op, for forward compatibility.

The Configure-controls page drives that set in one shape. On open, read
`slotRemap(slotId)` and render which raw source each output reads plus the
invert toggles; `{}` means this slot takes no raw-joystick remap, so show the
not-remappable note instead. To reassign an output, call
`startInputCapture(slotId)`, prompt for the input, and on
`rawInputCaptured(slotId, kind, index, value)` call
`assignSlotInput(slotId, target, kind, index)` for the output being edited, then
`stopInputCapture()` and re-pull `slotRemap`. Always call `stopInputCapture()`
when leaving the page, or capture keeps streaming.

### Capability

```qml
App.capabilityForCandidate(slotId, type, hostKind, hostId,
                           desiredPath, motionOn, rumbleOn, touchpadMode)
```

`hostKind` is `"satellite"` or `"bluetooth"`. `hostId` is the stable satellite
id, and `""` means no destination chosen. `desiredPath` is `"standard"` or
`"direct"`. `touchpadMode` is `0` off, `1` pad, `2` mouse.

It returns seven rows in the fixed render order
`gamepad · triggers · motion · touchpad · mouse · rumble · lightbar`, each:

```js
{ feature, inOk, linkOk, typeOk, hostOk, verdict, failingLayer, hasFailingLayer }
```

`feature`, `verdict` and `failingLayer` are lowercase tokens. The C++ never
vends a sentence; QML localizes.

| `verdict` | Meaning | Render |
|---|---|---|
| `available` | Every layer carries it and the user has it on. | check, `Theme.success` |
| `unavailable` | A layer refuses it. `failingLayer` names the **first**: `input`, `link`, `type` or `host`. | cross, plus the reason line for that layer |
| `off` | Every layer carries it and the **user** switched it off. | "Off", plus a pointer at the switch |
| `pending` | The host or its catalog is unresolved. | em dash. **Never a cross**: a guessed "unsupported" is worse than no table. |

| Method | Returns |
|---|---|
| `typeFeatureSummary(hostId, type)` | `[{ feature, supported }]` for the type-preview pills. Empty while the catalog is unresolved. |
| `catalogResolvedFor(hostId)` | `false` means every capability row reads `pending`. |

### Host accounting, and the stale-slot-id trap

| Method | Returns |
|---|---|
| `hostBoundSlotCount(connectionId)` | Pads already bound to that host. |
| `hostSlotCapacity()` | `4`. Compose "\<n\> slots free" in QML, and **never assert a slot number before `applyBinding` allocates one**. |
| `displacedSlotName(connectionId)` | The pad a bind would push off a full host; `""` when there is room. |
| `resolveSlotIdForBind(slotId)` | The slot id re-resolved by `(vid, pid)`; `""` when the pad is gone. A Direct claim **retires** the framework slot id and publishes a synthetic twin, so an id a page opened with can be stale by the time the user presses Bind. |
| `isVerifiedModel(slotId)` | The raw-HID report layout for this model is known rather than guessed. Drives the Direct card's "Layout guessed" chip. Always `false` over Bluetooth. |

### Draft settings

| Method | State |
|---|---|
| `touchpadModeFor(connectionId)` → `"off"` / `"pad"` / `"mouse"` | Real. Per-satellite store; `"off"` when never picked. |
| `setTouchpadMode(connectionId, mode)` | Real. |
| `rumbleEnabledFor(slotId)` | **Stub: always `true`.** |
| `setRumbleEnabled(slotId, on)` | **Stub: no-op.** |

The rumble pair is honest about being a stub. No per-binding rumble store
exists; rumble rides the descriptor capabilities. Still render the Rumble row
and its switch: the capability verdict for it is real, so hiding the row would
hide true information. The switch simply has no durable effect yet.

### Apply

```qml
App.applyBinding(slotId, connectionId, type, desiredPath,
                 motionOn, rumbleOn, touchpadMode)
App.cancelApply()
```

`applyBinding` is the only write either binding surface makes. The setup
wizard's first four pages call no setter at all; every answer lands in the
draft and travels with this one call. It:

1. re-resolves the slot id, and fails immediately with `"slotGone"` if the pad
   went away;
2. switches the USB path only if it actually differs, budgeting **20 s**. A
   claim that times out is a fallback to Standard, not a failure: the run
   continues and `directFellBack` comes back true;
3. writes the type, motion and touchpad mode, then binds, budgeting **8 s**;
4. emits `applyChanged()` on every move and `applyFinished(...)` exactly once.

`cancelApply()` is accepted only while `applyCancellable`. Aborting a claim
drops back to Standard, which is safe. The REST round trip cannot be
half-applied and offers no escape.

On success, pop to Home and toast. On failure, **stay**: the draft is intact,
the reason goes in a toast, and the primary action is live again. The wire
renders as `transmitting`, never `live`, until success, because showing a
connection that never existed is worse than showing none.

### Bluetooth, licenses, onboarding, links

| Method | Effect |
|---|---|
| `refreshBluetoothState()` | Re-probe the radio. Cheap, and emits only on a real change. |
| `openBluetoothSettings()` | Open the desktop's Bluetooth settings, via the XDG settings portal. |
| `licenses()` | The bundled third-party manifest as `{ name, version, license, url }`. Unnamed entries are dropped. |
| `markOnboardingComplete()` | Persist the welcome-completed flag. `onboardingNeeded` then flips false. |
| `openExternalUrl(url)` | Open a URL through the shared `ExternalLink` path; a failure raises `errorMessage`. Not a raw `Qt.openUrlExternally`. |
| `setThemeMode(mode)` | Apply an appearance mode. Also available as the `themeMode` property write. |
| `setCrashReportingEnabled(on)` | Also available as the property write. |
| `setRailCollapsed(collapsed)` | Also available as the property write. |
| `setLightbarFollowGame(on)` | Also available as the property write. |

The setter invokables exist alongside the property writes because QML can only
assign a `WRITE` accessor, and the pages call these as functions.

## `SlotListModel`, bound as `App.slotModel`

A `QAbstractListModel`, one row per controller slot. It emits minimal
`rowsInserted` and `rowsRemoved` on a count change, and `dataChanged` scoped to
the moved roles on an in-place update, so a `ListView` never resets on a quiet
telemetry tick.

### Identity and binding

| Role | Type | Meaning |
|---|---|---|
| `slotId` | `string` | Stable slot id. |
| `name` | `string` | Display name, for example "DualSense". |
| `bound` | `bool` | The slot is bound to a connection. |
| `boundConnectionId` | `string` | The bound connection id; `""` when unbound. |
| `boundLabel` | `string` | Bound server label; `""` when unbound. |
| `emulateName` | `string` | Resolved emulation-type short name for the "as DualShock 4" suffix. Empty means omit the suffix. |
| `live` | `bool` | The bound session is `Connected`. |
| `dotColor` | `string` | `"success"` / `"warning"` / `"muted"`. |
| `registering` | `bool` | An attach is in flight. Render the busy card instead of chips and actions. |

### Hardware

| Role | Type | Meaning |
|---|---|---|
| `usbDirect` | `bool` | The slot is a USB-direct raw-HID synthetic. |
| `bluetooth` | `bool` | The pad is connected over Bluetooth, classic or BLE, classified at attach from the device path. Swap the card's glyph family to the `bluetooth*` set and show the Bluetooth transport chip. Always `false` for a USB-direct synthetic, and a Bluetooth slot is never `pathSupported`. |
| `remappable` | `bool` | A raw-joystick SDL pad whose DirectInput routing may be remapped. `false` for synthetics, the virtual slot, and SDL-recognised game controllers, which use SDL's own mapping. Gate the Configure-controls entry on this. |
| `hasMotion` | `bool` | The hardware has a gyro or accelerometer. |
| `hasLightbar` | `bool` | The hardware has an RGB LED. Show the Lightbar chip only when true. |
| `hasTouchpad` | `bool` | The pad reports a touch surface. Gates **both** the touchpad and the mouse capability rows, because mouse is a routing of the touchpad. |
| `verifiedModel` | `bool` | The raw-HID fast lane knows this model's report layout. Always `false` over Bluetooth. |
| `batteryLevel` | `int` | 0 to 100 percent, or `255` for unknown. |
| `batteryStatus` | `int` | Wire status: `2` charging, `3` full, `4` wired; anything else is discharging or unknown. |
| `batteryKnown` | `bool` | `batteryLevel != 255`. Show the battery chip only when true. |

### Rates

| Role | Type | Meaning |
|---|---|---|
| `gamepadHz` | `int` | Report-rate value. |
| `gamepadHzLive` | `bool` | The rate is a live measurement (USB-direct) rather than a peak estimate. |
| `gamepadHzShown` | `bool` | Whether to show the gamepad-rate chip. |
| `motionHz` | `int` | IMU sample rate. |
| `motionHzShown` | `bool` | Whether to show the motion-rate chip. |
| `pollHz` | `int` | Measured USB-direct poll rate (URB completion rate). |
| `pollHzShown` | `bool` | Whether to show the poll-rate chip. |

The `*Shown` and `*Live` booleans come from the pure `SlotLiveStats` mapper.
Do not format the numbers yourself; `Kit.LiveStat` owns rate and latency
formatting for the whole app.

### USB path

| Role | Type | Meaning |
|---|---|---|
| `pathPhase` | `string` | FSM phase token: `"routed"` / `"claiming"` / `"direct"` / `"awaitingFramework"` / `"restoreStuck"` / `"needsReplug"`. |
| `desiredPath` | `string` | The path the toggle reads as selected: `"standard"` or `"direct"`. `"auto"` is a `setSlotPath` input only; it resolves to one of these and never appears here. |
| `pathSupported` | `bool` | The device is raw-HID claimable. Show the path control only when true. An Xbox pad has no such path (xpad publishes no hidraw node), and a Bluetooth-connected pad never does, because the claim is USB-only. |
| `claimInProgress` | `bool` | `pathPhase === "claiming"`. Disable the toggle and show a spinner. |
| `directFailure` | `string` | Last Direct-claim failure token: `"permissionDenied"` / `"busy"` / `"initFailed"` / `"dropped"`, or `""`. Drives the inline note together with the `needsReplug` and `restoreStuck` phases. |

### The bound-satellite join

The Home signal-path row's right cell and its wire latency, joined per slot by
`boundConnectionId` against the derived connection rows. Same token vocabulary
as `ConnectionListModel`, so the satellite cell renders identically to a
Connections row by construction. All empty or zero for an unbound slot, or for
a binding whose row has vanished; render the ghost Bind action card then.

| Role | Type | Meaning |
|---|---|---|
| `satIp` | `string` | Bound satellite ip. |
| `satLinkState` | `string` | Link-state token. |
| `satChip` | `string` | Status-chip key. |
| `satDotColor` | `string` | `"success"` / `"primary"` / `"warning"` / `"muted"`. |
| `satGlyph` | `string` | `"satelliteBase"` / `"satelliteConnected"` / `"satelliteOff"`. |
| `satLatencyText` | `string` | Pre-formatted latency. |
| `satLatencySamples` | `int` | RTT samples in the window. Gate the wire's latency half on `satLatencySamples > 0 && (satLinkState === "connected" \|\| satChip === "unstable")`. |

The satellite cell's display name is the existing `boundLabel` role.

## `ConnectionListModel`, bound as `App.connectionModel`

One row per derived connection: the remembered set joined with live link state.
Same minimal-signal behaviour as the slot model.

| Role | Type | Meaning |
|---|---|---|
| `connectionId` | `string` | Stable connection id. |
| `label` | `string` | Server name, or the ip when the name is empty. |
| `ip` | `string` | IPv4 address. |
| `udpPort` | `int` | UDP stream port. |
| `linkState` | `string` | `"found"` / `"stale"` / `"saved"` / `"ready"` / `"connecting"` / `"connected"` / `"unstable"`. |
| `chip` | `string` | Status-chip key: `"found"` / `"needsPairing"` / `"offline"` / `"ready"` / `"connecting"` / `"online"` / `"unstable"`. Localize the chip text yourself. |
| `dotColor` | `string` | `"success"` / `"primary"` / `"warning"` / `"muted"`. |
| `glyph` | `string` | `"satelliteBase"` / `"satelliteConnected"` / `"satelliteOff"`. |
| `boundSlotId` | `string` | Slot bound to this connection; `""` when unbound. |
| `liveLink` | `bool` | The link is actively streaming (`Connected` or `Unstable`). **Gates the per-row buttons**: enable `disconnectConnection` only when `liveLink`, and `reconnectConnection` only when not. |
| `latencyText` | `string` | Pre-formatted one-way latency, for example `"~3.4 ms"`. Median heartbeat RTT halved, over a sliding 64-ping window, refreshed about 1 Hz. `""` until a live session has samples, and `"<1 ms"` below the millisecond, never `"~0.0 ms"`. |
| `latencySamples` | `int` | RTT samples in the window, 0 to 64. Gate the latency caption on `linkState === "connected" && latencySamples > 0`. |

The token vocabularies above are produced by
[`RenderTokens.h`](../src/qml/RenderTokens.h), one switch per enum, shared by
every surface that vends them. QML colours and localizes **from** the tokens and
never re-derives them.

## The shell

[`AppShell.qml`](../src/qml/AppShell.qml) owns navigation. Pages reach it
through `StackView.view.shellApi`, which is the shell itself:

```qml
readonly property var shellApi: StackView.view ? StackView.view.shellApi : null
```

Declare it `var`, not a typed item: `shellApi` is a dynamic property and a typed
read makes `qmllint`'s `missing-property` gate fire.

| Call | Effect |
|---|---|
| `selectDestination(index)` | Switch the rail destination. This **replaces** the content stack, clearing any pushed detail pages. |
| `pushDetail(url, title, props)` | Push a detail page with an explicit breadcrumb title. `props` is optional and is the initial-property map, for a page that needs its id before the component loads. |
| `openSetupWizard(slotId)` | Open the setup wizard. `slotId` is optional; `""` starts at the beginning. |
| `requestNavigation(action)` | Run `action` through the leave guard. |
| `toast(message, severity)` | Raise a transient notice on the shell's one toast host. `severity` is `"error"`, `"warning"` or `"success"`. |
| `currentTitle` | The breadcrumb title, read-mostly. |

`openSetupWizard` always selects Home first and then pushes, so popping on
success or cancel always lands on Home. The wizard's result is a Home row, so it
must never strand the user in Settings.

The five destinations, in rail order, are Home, Controllers and Connections at
the top, then Support Dish and Settings pinned to the footer. Between the two
groups sits the Set up action, which is an action and not a destination.
`selectDestination(2)` is Connections.

### Per-page header

The shell draws the header. A page, including a pushed detail, may declare any
of:

```qml
readonly property string headerTitle   // falls back to the rail label
readonly property string headerSub     // falls back to empty
readonly property string headerDot     // a StatusDot token; empty draws no dot
```

The streaming pill is the shell's own and is present on every page whenever
`App.keepAwakeActive`. Do not add a second one.

### Leave guard

A pushed page may declare:

```qml
readonly property bool suppressBack           // hide the header back chevron
readonly property bool blocksLeave            // ask before the stack is replaced
function requestLeave(proceed) { ... }        // call proceed() to allow it
```

Anything that would replace or unwind the content stack from outside the current
page runs through `requestNavigation()`, so the page gets first refusal. The
window's close handler goes through the same guard before the keep-awake quit
confirm.

`Alt+Left` is bound to the same action as the header chevron and is enabled
exactly when the chevron is visible, because two enabled `Shortcut`s on one
sequence is an ambiguous activation. `F6` cycles the rail and the content pane;
`Ctrl+,` opens Settings.

## Onboarding

`Main.qml` hosts a top-level `StackView` (`appRoot`) below the title bar whose
`initialItem` is the `AppShell`. A first-run flow is shown full-screen **over**
the shell rather than inside it:

```qml
if (App.onboardingNeeded) {
    const flow = appRoot.push(Qt.resolvedUrl("onboarding/OnboardingFlow.qml"));
    flow.completed.connect(function (runSetup) {
        appRoot.pop();
        App.markOnboardingComplete();
        if (runSetup)
            shell.openSetupWizard("");
    });
}
```

`completed(bool runSetup)` fires once. Skip is a completion too, or the welcome
loops forever.

## Appearance

`setThemeMode` forwards to `ThemePreferenceStore`. `ThemeController` resolves
the mode to a concrete appearance, System included, and swaps the active
`dish::ui::Theme` palette off its Observable. The entry point then refreshes the
QML `Theme` singleton, whose tokens are `NOTIFY paletteChanged` so every binding
re-reads, and flips the native chrome's `DWMWA_USE_IMMERSIVE_DARK_MODE` to match,
so the frame never drifts light while the body re-darks.

System mode reads the OS preference at startup and follows it live: the view
model connects `QStyleHints::colorSchemeChanged` and re-resolves the appearance
whenever the OS flips, but only while the stored mode is `System`. An explicit
Light or Dark pick ignores the OS.

The window body is always the themed solid `Theme.background`, so no desktop can
show through a rounded corner on a compositing WM.

## Examples

Stream a telemetry value:

```qml
Label {
    text: qsTr("events/s %1   sends/s %2").arg(App.eventsPerSec).arg(App.sendsPerSec)
    color: Theme.muted
}
```

Iterate the slot model:

```qml
ListView {
    anchors.fill: parent
    model: App.slotModel
    spacing: Tokens.s4
    delegate: ItemDelegate {
        required property string slotId
        required property string name
        required property bool bound
        required property string boundLabel
        required property string dotColor
        width: ListView.view.width
        contentItem: RowLayout {
            Kit.StatusDot { token: dotColor }
            ColumnLayout {
                Label { text: name; color: Theme.onSurface }
                Label {
                    text: bound ? qsTr("Bound to %1").arg(boundLabel) : qsTr("Unbound")
                    color: Theme.muted
                }
            }
        }
    }
}
```

Submit a pairing PIN:

```qml
Kit.KitTextField { id: pinField; maximumLength: 6 }
Kit.KitButton {
    text: qsTr("Pair")
    enabled: pinField.text.length === 6 && !App.isPairingInFlight(server.id)
    onClicked: App.pairByServerId(server.id, pinField.text)
}
Connections {
    target: App
    function onPairingFailed(serverId, reasonToken) {
        if (serverId === server.id)
            pinField.hasError = true;   // the sheet stays open
    }
}
```
