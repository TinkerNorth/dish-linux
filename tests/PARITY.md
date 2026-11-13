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
running rumble every 40 ms, because the pad stops one on its own. Open
follow-ups: the two trackpads, the four back paddles, the quick-access button,
the trigger and trackpad clicks and the capacitive touches have no wire field
and are not forwarded; and the Bluetooth identity (`28DE:1303`) is never
Direct, like every Bluetooth pad.

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
- Xbox Direct exists only on Android: xpad owns those pads here.
- Virtual-pad skins, lightbar wash, cutout theme, Play billing, store
  screenshots: phone-only by nature.
- Android's keep-awake is fixed (screen and wake lock while streaming); the
  desktop setting is about PC sleep, a different problem.
- Mouse mode on desktop is a recorded deferral, not an oversight.
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
