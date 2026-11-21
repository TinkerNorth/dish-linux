# Parity: dish-linux against the other Dish clients

> **What this is.** The feature-parity ledger for this client, kept as a small
> matrix rather than a per-pull-request diary. The diary form went stale the
> moment a wave was ported in one PR without a row per Android PR, and by
> 2026-09-20 it had not been touched since 2026-08-18 while 24 Android PRs
> landed. A matrix cannot go stale that way: **a PR that changes a row updates
> this file in the same change**, and a reviewer checks the row, not the diary.
>
> Test coverage is no longer mapped file-by-file here. The Catch2 suite under
> `tests/` is the coverage truth and CI runs it on every push (gcc, clang, the
> two sanitizer lanes and coverage); the old android-test-file map claimed 0
> missing rows and said nothing a green suite does not.
>
> Legend: ✅ works · ⚠️ shipped, not yet verified on real hardware · ❌ not
> available · – not applicable. Verified against the code on 2026-09-29.

## 1. What works, per client

| Feature | Android | Windows | Linux (this) |
|---|---|---|---|
| Satellite host, protocol 4 | ✅ | ✅ | ✅ |
| Moonlight host (Sunshine / Apollo / Wolf) | ✅ | ✅ | ✅ |
| Mouse mode on the host | ✅ | ❌ | ❌ (deferred: `reducer::kClientRoutesTouchpadAsMouse`) |
| Steam Controller trackpad pick (stick / pad / mouse / off) | ⚠️ | ⚠️ (mouse behind the deferral above) | ⚠️ (mouse behind the deferral above) |
| Controller audio (mic + speaker) | ✅ | ✅ | ✅ |
| DualSense HD haptics | ⚠️ | ⚠️ | ⚠️ |
| Adaptive triggers, player LEDs, mic lamp | ✅ | ✅ | ✅ |
| Lightbar, motion, touchpad, battery | ✅ | ✅ | ✅ |
| Crash reports (opt-out) | ✅ Crashlytics | ✅ Sentry | ✅ Sentry |
| Update notice | – Play (the store updates it) · ✅ GitHub build | ✅ | ✅ |
| Runs in the background / window closed | ⚠️ Moonlight sessions stay up (foreground service) · satellite streaming stops when the app leaves the screen, by design: the phone is the pad | ✅ tray | ✅ tray |
| Survives PC sleep and resume | – | ✅ Satellite · ❌ Moonlight | ✅ Satellite · ❌ Moonlight |
| Keep-awake is user-configurable | ❌ (always on while streaming) | ✅ | ✅ |
| Diagnostics screen | ✅ | ✅ (rumble-only bench, see §5) | ✅ (rumble-only bench, see §5) |
| Link-tier cue on host rows (Fastest / Fast / Basic) | ✅ (on the section header) | ✅ | ✅ |
| Protocol chip ("Satellite update recommended / required", "Dish update required") | ✅ | ✅ | ✅ |
| App-wide microphone chip with mute-all | ✅ | ✅ | ✅ |
| Capability verdict word "Supported" | ✅ | ✅ | ✅ |
| Pairing secrets stored encrypted | ✅ Moonlight key (Keystore) · ❌ Satellite key (app-private prefs) | ✅ DPAPI | ✅ Satellite key (Secret Service, else a 0600 file) · ❌ Moonlight key (0600 file) |
| Six languages (en, bs, de, es, fr, pt-BR) | ✅ | ✅ | ✅ |

## 2. Input, pad → host, by path (cell = USB Direct · USB Standard · Bluetooth)

| | Android | Windows | Linux (this) |
|---|---|---|---|
| Buttons, sticks, triggers | ✅ · ✅ · ✅ | ✅ · ✅ · ✅ | ✅ · ✅ · ✅ |
| Motion | ✅ · ✅ · ✅ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Touchpad | ✅ · ⚠️ · ⚠️ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Battery | ⚠️ · ✅ · ✅ | ⚠️ · ✅ · ⚠️ | ⚠️ · ✅ · ⚠️ |
| Controller mic | ✅ · ✅ · ❌ | ✅ · ⚠️ · ❌ | ✅ · ⚠️ · ❌ |
| Extended buttons (paddles, the Steam extras, Capture; protocol 4) | ⚠️ · ❌ · ❌ | ⚠️ · ⚠️ · ⚠️ | ⚠️ · ⚠️ · ⚠️ |

The Battery row is what the host is sent: the lower of the pad's own reading
and the PC's battery, on every path and to a Moonlight host too, and the PC's
alone when the pad reports none. A wired pad that reports its own charge
competes like any other, a tie goes to the pad, and an unknown level loses to
a known one. Direct reads the pad's charge from its own report; Standard and
Bluetooth read SDL's coarse level, which SDL leaves unknown for a pad it sees
as wired. The controller card keeps showing the pad's own reading. Android
applies the same rule with the phone's battery.

## 3. Receiving, host → pad, by path (cell = USB Direct · USB Standard · Bluetooth)

| | Android | Windows | Linux (this) |
|---|---|---|---|
| Rumble | ✅ · ✅ · ✅ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Lightbar | ✅ · ❌ · ⚠️ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Adaptive triggers | ✅ · ❌ · ❌ | ✅ · ⚠️ · ⚠️ | ✅ · ⚠️ · ⚠️ |
| Player LEDs | ✅ · ❌ · ❌ | ✅ · ⚠️ · ⚠️ | ✅ · ⚠️ · ⚠️ |
| Mic lamp | ✅ · ❌ · ❌ | ✅ · ⚠️ · ❌ | ✅ · ⚠️ · ❌ |
| Controller speaker | ✅ · ✅ · ❌ | ✅ · ⚠️ · ❌ | ✅ · ⚠️ · ❌ |
| HD haptics | ⚠️ · ⚠️ · ❌ | ⚠️ · ⚠️ · ❌ | ⚠️ · ⚠️ · ❌ |

The Android Standard column is ❌ for lightbar, triggers, LEDs and the lamp
because the framework has no API for them over USB. The Bluetooth column here
is ⚠️ because SDL's HIDAPI driver only opens a Bluetooth Sony pad in enhanced
mode since the 2026-09-20 wave, it needs the shipped udev rule's Bluetooth
clauses (`packaging/udev/70-dish-hidraw.rules`, #71) installed, and nobody has
paired one against it yet.

## 4. Which controllers can use USB Direct (Bluetooth is never Direct)

| Controller | Android | Windows | Linux (this) |
|---|---|---|---|
| DualShock 4 | ✅ | ✅ | ✅ |
| DualSense (+Edge on desktop) | ✅ | ✅ | ✅ |
| Switch Pro | ✅ | ✅ | ✅ |
| Generic PDP Switch pads | ✅ | ✅ | ✅ |
| 8BitDo | ✅ known XInput models | ✅ in HID mode (generic HID parser); Standard only in XInput mode (XUSB owns it) | ✅ in HID mode (generic HID parser); Standard only in XInput mode (xpad owns it) |
| Xbox 360 / One / Series wired | ✅ | ❌ Standard only (XUSB owns it) | ❌ Standard only (xpad owns it) |
| Stadia | ✅ | ⚠️ generic HID parser, manual pick | ⚠️ generic HID parser, manual pick |
| Steam Controller (2015) | ✅ | ⚠️ never verified; each controller on the receiver a pad of its own | ⚠️ never verified; each controller on the receiver a pad of its own (see below); the mainline hid-steam driver exposes an evdev twin and manages lizard mode itself |
| Steam Controller (2026) | ❌ | ⚠️ never verified, ported from SDL | ⚠️ never verified, ported from SDL (see below) |

The 2026 Steam Controller (wired `28DE:1302`, dongles `28DE:1304` and
`28DE:1305`) is ported from SDL's driver (`SDL_hidapi_steam_triton.c`) and no
pad has been on hand. Like the 2015 pad it is never claimed on its own, only
through a Direct pick on its card. Its output worker sends the quiet settings
(lizard mode off, raw accelerometer and gyro on) at the claim and every 3 s,
because the pad's firmware turns lizard mode back on otherwise, and repeats a
running rumble every 40 ms, because the pad stops one on its own. Its four back
paddles, quick-access button, trigger clicks and stick and grip touches ride the
protocol 4 extended-buttons word (see below), and its two trackpads go where the
slot's trackpad pick puts them (below). Open follow-up: the Bluetooth identity
(`28DE:1303`) is never Direct, like every Bluetooth pad.

The trackpad pick, per Steam Controller slot (`reducer::TrackpadPickResolve`,
`usbparse::routeSteamTrackpads`, the same ladder and router as dish-android's):
`stick | pad | mouse | off`, stored by slot id (`TrackpadPickRepository`), the
2015 pad defaulting to `stick` (its right pad the right stick and its click R3,
the behaviour before the pick) and the 2026 pad, which has a real right stick
and no `stick` choice, to `pad` when the emulated type has a touch surface,
else `off`. `pad` puts the left pad on the left half of the one wire surface
(finger 0, `x/2 - 16384`) and the right pad on the right half (finger 1,
`x/2 + 16384`), either click the surface's click; `off` drops both pads and
sends their clicks as `leftPadClick` / `rightPadClick`; `mouse` makes the right
pad the cursor and left button, the left pad's click the right button and its
vertical travel the wheel (4096 units a notch, the remainder carried), through
the protocol 2 POINTER frame's buttons byte and `scrollV`, which the Direct path
now carries end to end. The pick in force is held to what the path carries:
`pad` needs the type's surface, `mouse` the host's grant AND this client's
mouse routing (`reducer::kClientRoutesTouchpadAsMouse`, still deferred, so the
picker lists no Mouse yet). `test_steam_trackpads` pins, over every pick and
both models, that each pad position lands in at most one wire field and each
click in exactly one, and that outside `stick` the 2015 right stick stays
centred and nothing is R3. The pick reaches the claim as one atomic the read
loop copies before each decode; the slot's declared `touchpadMode` is the
pick's projection (`pad` → `ds4`, `mouse` → `mouse`, else `off`), not the
satellite's per-host touchpad pick, which a Steam slot's Apply leaves alone.
Unverified on hardware: the 2015 left pad's rotation sign follows SDL's
`RotatePad(-angle)`; the wheel's 4096 units a notch is a chosen feel.

The extended-buttons word (satellite `docs/contract.md` "Extended buttons",
SDL's names) is filled by the pads that have the buttons, each from the layout
SDL reads and none yet checked on hardware: the 2015 Steam Controller's two
grips as `leftPaddle1` / `rightPaddle1`, its full-pull trigger bits as the
trigger clicks, and a left-pad click under a finger as `leftPadClick` (with no
finger it stays the stick's L3; the right pad's click stays R3 while the right
pad is the right stick); the 2026 pad's QAM as `misc1`, R4/L4/R5/L5 as paddle
1-4, its pad and trigger clicks and its stick and grip touches; the DualSense
Edge's (`054C:0DF2`, never the plain DualSense) back and Fn buttons from input
byte 10 as paddle 1 and paddle 2; the Switch Pro's and Charging Grip's Capture
(shared byte 0x20) as `misc1`. On the Standard path SDL2's `paddle1`-`paddle4`
and `misc1` map the same way, so a pad whose SDL mapping names them (an Xbox
Elite through a HIDAPI driver, a DualSense Edge, the Steam Deck's own controls
on Linux) sources the word too. A slot claims `extendedButtons` exactly when
its path's pad has one of these (`SlotRouter::SlotHardware::hasExtendedButtons`).
What the host renders is the emulated type's business; the DualSense Edge type
and the trackpad pick are the next changes.

A Steam Controller receiver (the 2015 `28DE:1142`, the 2026 `28DE:1304` and
`28DE:1305`) serves up to four paired controllers, one HID interface each: 1 to
4 on the 2015 receiver, its keyboard on 0, and 2 to 5 on the 2026 ones (SDL's
`HIDAPI_DriverSteam_IsSupportedDevice` and
`HIDAPI_DriverSteamTriton_IsSupportedDevice`, the kernel's
`steam_is_valve_interface`). The Direct pick is the receiver's and claims every
slot, all of them or none. A slot is a pad of its own, with its own card and
binding, while its controller is there: from its connect event (the 2015 pad's
`ID_CONTROLLER_WIRELESS`, connected or newly paired, the 2026 pad's
`0x46`/`0x79`) or its first report, until its disconnect event, which the next
1 s scan turns into the card appearing or leaving. An empty slot stays claimed
to listen and is no pad; while no controller is on, the receiver keeps its own
card, which says so. The receiver's card carries the path toggle only and offers
no binding or type pick (`bindable` in `docs/QML_CONTRACT.md`): it stands for
the receiver, not for a pad, and its id is no pad's. A controller's binding and
type are picked on its own card, which keeps them across the controller turning
off and on. A slot's id is its interface, so a controller that comes back lands
on its card and its Moonlight binding; a satellite binding leaves with the
controller, as it does for any pad that goes, and never moves onto another
slot's. Its battery reading leaves with it too, and is shown again as soon as
the controller is back, changed or not. Known limitation: two receivers of the
same model collapse into one. The manager keys a receiver by (vid, pid,
interface) and the gateway re-finds a slot by vid, pid and interface, first
match wins, so only one receiver's slots are read and the other's controllers
reach nothing. The fix is to key a receiver by its device path and carry the
path in `UsbDeviceInfo`. The 2015 receiver's slot is asked what it holds
(`ID_DONGLE_GET_WIRELESS_STATE`) when it opens, the request SDL and hid-steam
both send; the answer is read off the slot's input stream the way SDL's
`HIDAPI_DriverSteam_InitDevice` reads it, where hid-steam's
`steam_get_conn_status` reads it back as a feature report. Every Steam
Controller's settings are its claim's output worker's: the 2015 pad's quiet
sequence goes when the pad is there (at the claim for a wired pad, on the
connect event or first report for a receiver's controller) and its restore
sequence when the claim is handed back while the pad still holds them; the 2026
pad's settings go on the worker's 3 s clock and are undone the same way. The
read thread only flags the arrival and the departure, so a slot's reports never
wait on a feature write. A pad that is unplugged is let go with nothing written
to it, and a feature write that fails because the pad is gone is not retried. No
receiver has been on hand: how an empty slot answers the feature reports (the
2015 state request, the 2026 pad's 3 s settings) is unverified. Open follow-up:
a binding made on a receiver controller's SDL twin (hid-steam's evdev pad) is
not matched to that controller's slot on a Direct pick, because SDL does not say
which slot the twin rides; it moves by model, onto the first unbound slot's pad,
or is dropped.

## 5. Decisions, not gaps

- Android's on-screen pad takes controller audio from the phone's own mic and
  speaker. A physical pad, on every client, uses only its own USB endpoints and
  refuses on ambiguity; a Bluetooth pad gets none.
- The three audio caps are promises the host streams on alone, so every client
  advertises one only while it will carry the direction right now: the pad's
  route, the slot's switch, the host's grant as the client last read it, and
  no endpoint the platform refused to open. A refused open is kept per slot,
  lane and endpoint (`core/audio/AudioRefusal.h` here, `PadAudioRefusalStore`
  on Android, `RefusedEndpoint` on the Mac), is not retried at that endpoint,
  and lapses when the endpoint changes or the device departs and returns. A
  moved grant re-sends the bound slots' descriptors; the grant is read when a
  session opens, so a direction the host switches on reaches a connected client
  at its next session.
- Xbox Direct exists only on Android: xpad owns those pads here.
- Virtual-pad skins, lightbar wash, cutout theme, Play billing, store
  screenshots: phone-only by nature.
- Android's keep-awake is fixed (screen and wake lock while streaming); the
  desktop setting is about PC sleep, a different problem.
- Mouse mode on desktop is a recorded deferral, not an oversight.
- The DualSense Edge type (catalog id 4, `dualsenseedge`, protocol 4) is the
  DualSense's bundled feature set plus `extendedButtons`, the four paddles the
  host renders as its back and Fn buttons. It is listed last and is never the
  type a slot defaults to: only a real Edge's own `emulates` hint or the user's
  pick lands on it. The desktops carry no per-type art or labels, so the picker
  shows the satellite catalog's name for it, as for every type.
- The Android protocol chip keys on the satellite's advertised version and
  reads "Dish update required" for a newer satellite that still accepts this
  build; this client keys the chip on the negotiation, so that case reads as
  fine. Red here means the session cannot open.
- The Diagnostics page leaves out these parts of Android's. Screen hold:
  keep-awake is the user's own setting here, and a page that held the display
  would be a second writer to the same inhibitor. The latency profiling bench:
  the desktop input path carries no per-stage timestamps, so the page shows the
  session's measured round trip instead. Host cards for Moonlight and Bluetooth
  hosts: every fact the desktop holds about a Moonlight session already renders
  on the Moonlight hosts page, the host app and GFE versions and per-pad report
  counts Android adds have no desktop source, and this client never pairs to a
  host as a Bluetooth gamepad. A controller bound to a Moonlight host still
  names it.
- Of Android's three radio cards the page shows Bluetooth, with the adapter's
  state; that card's permission, link-type and host-role rows have no desktop
  counterpart. The Wi-Fi card is left out because the desktop has no WLAN
  source, and the USB card because each controller card already shows its path
  and poll rate, while the endpoint packet facts Android adds have no desktop
  source.
- The Diagnostics feature bench drives rumble only, through the ungated path, so
  a pad whose rumble switch is off can still be tested. The lightbar, player
  LEDs, trigger effects and mic lamp are host-owned state a test would
  overwrite, and the lamp is the mute indicator. Controller audio has no test
  tone in the desktop engines.
- The Diagnostics event log is localized like every other desktop string: the
  C++ vends tokens and the QML words them. Android keeps its log in English as
  export material for bug reports.

## 5a. Controller database and diagnostics (dish-website `docs/controller-db.md`, `docs/diagnostics-bundle.md`)

Shipped in 2.8.0, the hand port of dish-windows#138, ⚠️ throughout until a
hardware pass: the suite covers every decision, but no build of it has met a
real pad or a real mail client yet. Where this client does not do what the
contracts say, the row says so and why.

| Contract item | Windows | Linux (this) |
|---|---|---|
| Bundled copy of dbVersion 1 (272 records), checked against its manifest at every load | ⚠️ `:/controllers/v1` | ⚠️ `:/controllers/v1`, compiled into `dish_core` (`resources/controllerdb.qrc`); `test_controller_db_bundled` reads it both through the resource and from `assets/controllers/v1`, which `.gitattributes` marks `-text` |
| Index `identity.usb` / `identity.bluetooth` → record; cached copy in force only when its `dbVersion` is greater | ⚠️ | ⚠️ |
| Check: 10 s after launch, every 6 h, `If-None-Match`, SHA-256 + size per file, schema 1, atomic swap | ⚠️ | ⚠️ the swap is a `current` file renamed over (`QSaveFile`) after the version's own folder under `~/.local/share/TinkerNorth/Dish/controllers/v1/` is written |
| "Check for controller updates automatically" (default on) and "Check now" | ⚠️ | ⚠️ Settings → Updates, and "Check now" on the Diagnostics card too; both "Check now" buttons are disabled while the switch is off, and the coordinator refuses one regardless, so with it off no request leaves the machine. Default on in the Flatpak too, unlike the update check, which starts off there |
| Compiled tables generated from the file, a test asserting they agree | ❌ deferred | ❌ deferred: `kKnownModels` and the fast-lane vendors (`controllerdb::vendorFastLane`) stay hand-written and `test_controller_db_bundled` requires each row to match its record (usb id, family, and for `hid` the mapping) and each fast-lane vendor to own a `verified` record; `test_pad_model` holds the pinned model of every compiled id to the compiled functions' answers |
| Served records classify ids the compiled tables lack | ⚠️ | ⚠️ applied: the gateway that first enumerates a pad resolves its model once (`controllerdb::PadModelResolver`, an immutable snapshot of the active and bundled catalogs the app swaps whole) and pins it per attach, by the kernel's HID device name (`0003:054C:0CE6.0007`, whose sequence number is new each time the kernel adds the device; `controllerdb::ModelPins`), on the pad's `UsbDeviceInfo`; the capability solve, the claim, the read loop, the output worker, the health judgement and the SDL path's effect builders read it. Compiled tables first, then the row in force, then the vendor fallback. A served row supplies the family's parser, the mapping's order, the init kind, the Direct rule (`auto` on a `verified` row goes Direct by itself, `whenOsCannot` only for a pad with no framework gamepad, `optIn` never) and whether it is verified. Extended buttons, Steam trackpads and the framework gamepad follow from the family and the product id, as dish-android's `usb_parsers.cpp` derives them, not from the row's `capabilities`: dbVersion 1 says `extendedButtons: false` for the Switch Pro and the charging grip, whose Capture button the decoder reads (`test_pad_model`). A receiver's slots are compiled only (dish-android's `ReceiverSlots.kt` too), so a served row is never a receiver. The init kind rides the model but this path runs a family's own sequence (the Steam pads' settings by the output worker; the Switch Pro's handshake is hid-nintendo's, before hidraw reads), and every runnable dbVersion 1 record names its family's (`controllerdb::initOfFamily`) |
| `requires` → inert (name and issues only), bundled row for the id stays in force | ⚠️ | ⚠️ `controllerdb::isInert`, `directRecordFor`, which also lets a `flagged` row give way to a live bundled row for its id (docs/controller-db.md 4.1, "a client prefers its bundled row"; a flagged row with no bundled twin still applies). The Xbox families are inert here by construction: xpad binds those pads as evdev and publishes no hidraw node |
| `expect.endpoints` at attach | ⚠️ HID report lengths | ⚠️ against the claimed interface's endpoints as sysfs lists them (`ep_XX` with the direction bit), as dish-android's check reads its claim's endpoint pair; `null` where sysfs was not read. usbhid sends an output report over the interrupt OUT pipe when there is one and as SET_REPORT(Output) otherwise, so `endpoints.out: false` is a fact about the pipe, not about whether rumble lands. Checked on the Direct path only |
| "Last write" (route, report id, ok, OS error, time) | ⚠️ Win32 code | ⚠️ from each claim's output worker through a seqlock. `route` is `interruptOut` for `write()` on the node and `setReport` for `HIDIOCSFEATURE`; which pipe usbhid used for a `write()` is not observable. `err` is the `errno` name (`EPIPE`, `ENODEV`, …, else `errno:<n>`); a short write is a failure with no code |
| "Last feedback" (host to client) | ⚠️ per lane | ⚠️ the top-level `lastFeedback` stays `null`; the same facts are now per lane under `output` (below) |
| Pad tests with "Did it work?" outcomes, last twenty, in memory | ⚠️ | ⚠️ `rumble.weak`, `rumble.strong`, `rumble.both` with `worked` / `didNotWork`, on the inspector's existing bench; the bundle contract's other tests (trigger pulse, lamp, tone, mic level, the stick tests) are not offered (§5 says why the other surfaces have no bench here) |
| Issue shown while `open`, for this transport on this machine model | ⚠️ `windows` key | ⚠️ `fixed` and `wontfix` never show; the model is DMI's `sys_vendor` + `product_name` ("Linux PC" when unreadable, never the host name); an `os` map matches only through its `linux` key, read as the **kernel's** major version (a number, or a string such as `6.8`), because the contract names no Linux version and the kernel is the one every Linux machine has |
| Diagnostics: bundled / active `dbVersion`, source, last check time and result, per controller record id, verification and mismatches | ⚠️ | ⚠️ |
| Bundle `dish-diagnostics.v1` | ⚠️ | ⚠️ `path` is `direct` or `sdl`; a Direct pad's `hardware` carries vid, pid, `bcdDevice` (sysfs), product, manufacturer, every interface with its endpoints (parsed from sysfs's `descriptors`, `test_usb_hardware` on a DualSense's and two root hubs' real bytes), `endpoints` and `descriptorSha256`, the SHA-256 of the raw USB descriptors as the bus delivered them (sysfs's `descriptors`: the device descriptor, then every configuration in index order), which is dish-android's `getRawDescriptors()` byte for byte, so one pad and firmware hash the same on both; `firmware` is `null`; an SDL pad's are `null`. There is no `standard` block (SDL is the reader); a Satellite's `version` is the `serverVersion` its capabilities last reported, `null` before any; `app.flavor` is `flatpak`, `appimage`, else `github` (a package or a source build) |
| Email: file under the data folder, `xdg-email --attach --subject --body support@tinkernorth.com`, else `mailto:` and the folder revealed | ⚠️ MAPI | ⚠️ the file goes to `~/.local/share/TinkerNorth/Dish/diagnostics/`. `xdg-email` runs with one argv entry per argument, never through a shell; exit 0, or still running after 10 s (a mail client it started in the foreground), is the hand-off; any other exit, a crash, a start failure or no `xdg-email` on the PATH opens `mailto:` (cut at 2000 characters, at a whole character) and shows the folder through `QDesktopServices` (xdg-open, or the OpenURI portal in the Flatpak). A folder, not the file: xdg-open cannot select one. Decisions in `core/diagnostics/MailHandoff.h`, tested against a stand-in `xdg-email` |
| "Report a problem" / "Ask a question": category, own words, controller, plain-words list, "Preview file", the never-holds sentence, "Continue in your mail app" | ⚠️ | ⚠️ one page (`ReportProblemPage.qml`) in two starting categories, reached from the Diagnostics page's "Send to support" card, which keeps *Save diagnostics*. The controller picker starts on the first attached pad. "Continue" waits for the snapshot answers (two seconds at most). Subject prefix `Dish support:` English in every locale |
| `report` in the file and first in the mail body | ⚠️ | ⚠️ `controller` is the record id, else the slot's product string, else `null` |
| `hosts[i].snapshot` from `GET /api/connections/{id}/diagnostics` | ⚠️ | ⚠️ every live Satellite link, through `WifiConnectionManager`'s pinned HTTPS and proof, two-second deadline. Only a JSON object of `satellite-connection-diagnostics.v1` is embedded; its host's label and address are replaced by the alias wherever they appear, keys included. 404 is `unknownConnection`, 401/403 `unauthorized`, the deadline `timeout`, a 5xx or a 200 whose body is not a snapshot `badReply`, no answer at all `offline` |
| `olderSatellite` from the version, the route not called | ⚠️ | ⚠️ below 2.3.0 by the capabilities' `serverVersion`; a version that does not read as numbers is asked. `satellite-capabilities` carries `protocol` as `[1, <capabilities' protocolVersion>]`: the capabilities route names only the ceiling, and every Satellite speaks 1 |
| `moonlight-serverinfo` | ⚠️ | ⚠️ `appVersion` only, from the last plaintext `/serverinfo` that answered; `state` and `pairStatus` are `null`. This client does ask `/serverinfo` over mutual TLS too, where `PairStatus` is true, but that answer is the host card's trust verdict and the file keeps Windows's shape. Moonlight's `session` lists the bound slots (record, the emulated-device pick as `requestedType`) and is `null` for ack, round trips and counts: GameStream reports nothing back and this client counts nothing on that path |
| `hosts[i].session`: slots, ack, RTT, `sent`/`received` per kind | ⚠️ | ⚠️ `requestedType`/`appliedType` are the satellite catalog's slug, or the wire number as text while no catalog is cached; `backend` is the host's active backend from its capabilities (the apply result names none per slot); `rtt` is the heartbeat window's p50/p99. `sent`/`received` are new relaxed counters in `SatelliteClient` (hot path); the heartbeat and its ack are not counted |
| Controller `input` (frames, rateHz, last) | ⚠️ | ⚠️ the processor's existing per-device counter and seqlocked last wire report; `rateHz` the slot's current gamepad rate |
| Controller `output` per lane: count, target, last, `lastWrite` | ⚠️ | ⚠️ new relaxed counters in `SlotRouter` (hot path), counted when the router hands a lane to the pad, the host's and the bench's alike; `target` is `directUsb` / `framework` (SDL) / `none`; `lastWrite` rides `rumble`. `triggerRumble` is `null`: no desktop path delivers one. The trigger-effect lane keeps only when, not its 22 bytes |
| `speaker` / `haptics` voices | ⚠️ | ⚠️ `open` / `closed` / `refused` from the playout engine, `device` `pad` while open (it opens only the pad's endpoint); `frames` is `null`, no per-voice count is kept; `null` for a pad with no such lane on this PC |
| `bluetooth` | ⚠️ HID path text | ⚠️ `profile` is always `hid` (the OS's Bluetooth HID reads every pad; this client has no BLE client of its own). `transportLayer` is `classic` for a HID device whose parent is an HCI connection (`hciN:handle`: net/bluetooth/hidp/core.c parents it on `session->conn->hcon`), and for a device uhid made it is BlueZ's `org.bluez.Device1.UUIDs` for the address in its `HID_UNIQ` (0x1124 alone `classic`, 0x1812 alone `le`, both or neither `null`), asked at `/org/bluez/hciN/dev_…` over the system bus (the Flatpak holds `--system-talk-name=org.bluez`), 500 ms at most, `null` when BlueZ cannot answer. BlueZ sets the uhid device's `uniq` to the remote address on both paths: 5.72 `profiles/input/device.c` `uhid_connadd` (`ba2strlc(&idev->dst, (char *) ev.u.create.uniq)`) and `hog-lib.c` (`BT_IO_OPT_DEST, ev.u.create2.uniq`), master `src/shared/uhid.c` `bt_uhid_create` (`sprintf(ev.u.create2.uniq, … dst->b[5] …)`, called from `profiles/input/device.c` and `src/shared/hog.c`; `hog-lib.c` is gone on master), and the kernel copies it to `HID_UNIQ` (uhid.c `strscpy(hid->uniq, ev->u.create2.uniq…)`, hid-core.c `add_uevent_var(env, "HID_UNIQ=%s", hdev->uniq)`). The all-zero address BlueZ writes when it cannot read the peer finds nothing. The address builds the object path and is gone |
| `device.battery`, `powerSave` | ⚠️ `GetSystemPowerStatus` | ⚠️ the first `Battery` entry of `/sys/class/power_supply` whose `scope` is not `Device` (a pad's own battery is one), `null` with none; an unreadable or out-of-range `capacity` is a `null` level; `charging` is `status` `Charging`. `powerSave` is `/sys/firmware/acpi/platform_profile` reading `low-power`, `null` where the platform has no profile file; power-profiles-daemon's D-Bus profile is not asked |
| `device.thermal` | ❌ `null` | ⚠️ the worst of every `/sys/class/thermal/thermal_zone*` that has a `temp` and a trip point: an `active` trip crossed is `fair` (a fan stepped up), `passive` `serious` (throttling), `hot` or `critical` `critical`; below every trip `nominal`; no zone with both is `null`. A trip at 0 or below is a disabled one |
| `device.wifi` | ❌ `null` | ❌ `null`: this client links no WLAN library (no NetworkManager or nl80211 client), so band and RSSI are not read |
| `device.usbHost` | ⚠️ SetupAPI, in its third cut | ⚠️ `true` when `/sys/bus/usb/devices` lists a root hub `usbN` (one per host controller's bus), `false` when it lists none, `null` when the directory cannot be read (`source::usb::countRootHubs`, `diagnostics::usbHostOf`). Flatpak shares `/sys/bus`, `/sys/class` and `/sys/devices` read-only with every sandbox (flatpak `common/flatpak-run.c`, `sysfs_dirs` bound in `flatpak_run_setup_base_argv`), so the Flatpak reads the same tree; Docker's Linux 7.0.14 VM lists `usb1` and `usb2` (vhci_hcd), which reads `true` |
| Capture, `usb-descriptors-v1` | ⚠️ | ⚠️ *Capture this controller* on Report a problem, for the picked pad when it is a Direct claim: the claim's read loop keeps its next twenty input reports (64 bytes each at most, stamped on the steady clock and moved onto the wall clock by age) in slots fixed with the claim, then disarms itself; the page polls ten times a second for two seconds, so a pad that reports only on change may give fewer than twenty. `device` is the hex of the 18-byte device descriptor and `configuration` of every configuration in index order, sysfs's `descriptors` split after 18 bytes whatever `bLength` says, as the kernel lays the file out (`descriptors_read` writes `sizeof(struct usb_device_descriptor)` first); `strings` is `{ "manufacturer", "product" }` as text by role, sysfs's `manufacturer` and `product` for the device descriptor's `iManufacturer` and `iProduct`, `null` for a role whose index is 0 or the serial number's, and never the serial number; `hidReportDescriptors` is sysfs's `report_descriptor` with `source: os`: it is the kernel's copy, after the HID driver's `report_fixup` (hid-core.c `report_descriptor_read` copies `hdev->rdesc`), the pad's own bytes only for a driver without one; `device` is kept for bytes read off the pad itself, which this client never asks for. Only the capture of the controller the report names enters the file |
| hidraw access for the records the Direct path runs | – | ⚠️ `packaging/udev/70-dish-hidraw.rules` covers every bundled record the Direct path runs (`test_udev_rules_coverage`, 70 ids). A row a later download adds for a vendor no rule covers fails its claim `PermissionDenied` and stays on SDL until a rule grants it, except on SteamOS, which tags every hidraw node itself |
| Satellite's `/api/controllers/v1` mirror | – | – clients do not read it in schema 1 |

## 6. Android PRs since the diary stopped (2026-08-18 → 2026-09-20)

| Android PR | Counterpart here | Disposition |
|---|---|---|
| #162 Guide bit in XInput360 decode | – | N/A (no XInput Direct) |
| #163 offer the wired path when a Bluetooth pad is plugged in | same wave as win #52 | ported |
| #164, #165, #169 Play notes, donation flavour, APK name | #45 (stable download aliases) | N/A / done |
| #170 Moonlight | #46 | ported |
| #173 link-tier cues | this wave | ported |
| #174 per-type skins (+ protocol chip) | this wave (chip) | skins N/A, chip ported |
| #175 protocol 2 | #47 | ported |
| #176 lightbar wash | – | N/A |
| #177 input-path-honest capabilities | this wave ("Supported") | ported; this client's capability solver has no Unknown state |
| #178 controller audio (+ app-wide mic chip) | #48 + this wave | ported |
| #182, #183, #186, #190, #197 changelog / notes | – | N/A |
| #184 privacy policy | #60 | ported |
| #185 Diagnostics | this wave | ported; what the desktop leaves out is in §5 |
| #187 to #206 Play billing, products, screenshots, promote | – | N/A |
| #191 cutout theme | – | N/A |
| #198, #199 test / CI only | – | N/A |
| #210 haptics, framework lightbar/touchpad, Direct battery | #69 (+ #71 udev) | ported (⚠️ until a hardware pass) |
| #213 fake satellite applied state | – | test infra |
| #214 to #218 Crashlytics-driven fixes | checked: not needed here | Android-only |

Desktop PRs with no Android counterpart and no action needed: keep-awake modes
(#34), tray and suspend survival (#33), keyring-backed secrets (#34), the
release and packaging work (#35 to #45), the shared build story (#49), Sentry
releases (#63).
