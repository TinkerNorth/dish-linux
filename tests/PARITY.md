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
> Legend: ✅ works · ⚠️ shipped, not yet verified on real hardware · 📱 works
> through the phone's own mic/speaker, not the pad's (Android only) · ❌ not
> available · – not applicable. Verified against the code on 2026-09-20.

## 1. What works, per client

| Feature | Android | Windows | Linux (this) |
|---|---|---|---|
| Satellite host, protocol 3 | ✅ | ✅ | ✅ |
| Moonlight host (Sunshine / Apollo / Wolf) | ✅ | ✅ | ✅ |
| Mouse mode on the host | ✅ | ❌ | ❌ (deferred: `hostMouseControl=false`) |
| Controller audio (mic + speaker) | ✅ | ✅ | ✅ |
| DualSense HD haptics | ⚠️ | ⚠️ | ⚠️ |
| Adaptive triggers, player LEDs, mic lamp | ✅ | ✅ | ✅ |
| Lightbar, motion, touchpad, battery | ✅ | ✅ | ✅ |
| Crash reports (opt-out) | ✅ Crashlytics | ✅ Sentry | ✅ Sentry |
| Update notice | ✅ Play · ❌ GitHub build | ✅ | ✅ |
| Runs in the background / window closed | ✅ | ❌ (closing quits) | ✅ tray |
| Survives PC sleep and resume | – | ❌ | ✅ |
| Keep-awake is user-configurable | ❌ (always on while streaming) | ✅ | ✅ |
| Diagnostics screen | ✅ | ❌ | ❌ |
| Link-tier cue on host rows (Fastest / Fast / Basic) | ✅ | ✅ | ✅ |
| Protocol chip ("Satellite update recommended / required", "Dish update required") | ✅ | ✅ | ✅ |
| App-wide microphone chip with mute-all | ✅ | ✅ | ✅ |
| Capability verdict word "Supported" | ✅ | ✅ | ✅ |
| Pairing secrets stored encrypted | ✅ Keystore | ❌ plaintext (documented) | ✅ SecretService (plaintext file when no keyring) |
| Six languages (en, bs, de, es, fr, pt-BR) | ✅ | ✅ | ✅ |

## 2. Input, pad → host, by path (cell = USB Direct · USB Standard · Bluetooth)

| | Android | Windows | Linux (this) |
|---|---|---|---|
| Buttons, sticks, triggers | ✅ · ✅ · ✅ | ✅ · ✅ · ✅ | ✅ · ✅ · ✅ |
| Motion | ✅ · ✅ · ✅ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Touchpad | ✅ · ⚠️ · ⚠️ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Battery | ⚠️ · ✅ · ✅ | ⚠️ · ✅ · ⚠️ | ⚠️ · ✅ · ⚠️ |
| Controller mic | ✅ · ✅ · 📱 | ✅ · ⚠️ · ❌ | ✅ · ⚠️ · ❌ |

## 3. Receiving, host → pad, by path (cell = USB Direct · USB Standard · Bluetooth)

| | Android | Windows | Linux (this) |
|---|---|---|---|
| Rumble | ✅ · ✅ · ✅ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Lightbar | ✅ · ❌ · ⚠️ | ✅ · ✅ · ⚠️ | ✅ · ✅ · ⚠️ |
| Adaptive triggers | ✅ · ❌ · ❌ | ✅ · ⚠️ · ⚠️ | ✅ · ⚠️ · ⚠️ |
| Player LEDs | ✅ · ❌ · ❌ | ✅ · ⚠️ · ⚠️ | ✅ · ⚠️ · ⚠️ |
| Mic lamp | ✅ · ❌ · ❌ | ✅ · ⚠️ · ⚠️ | ✅ · ⚠️ · ⚠️ |
| Controller speaker | ✅ · ✅ · 📱 | ✅ · ⚠️ · ❌ | ✅ · ⚠️ · ❌ |
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
| Xbox 360 / One / Series wired | ✅ | ❌ Standard only (XUSB owns it) | ❌ Standard only (xpad owns it) |
| Stadia | ✅ | ❌ | ❌ |
| Steam Controller | ✅ | ⚠️ never verified | ⚠️ never verified; the mainline hid-steam driver exposes an evdev twin and manages lizard mode itself |

## 5. Decisions, not gaps

- Android routes controller audio to the phone's own mic/speaker for any
  emulated DualSense / DualShock 4 v2; this client routes only to the pad's own
  endpoints and refuses on ambiguity. Deliberate.
- Xbox and Stadia Direct exist only on Android: xpad owns those pads here.
- Virtual-pad skins, lightbar wash, cutout theme, Play billing, store
  screenshots: phone-only by nature.
- Android's keep-awake is fixed (screen and wake lock while streaming); the
  desktop setting is about PC sleep, a different problem.
- Mouse mode on desktop is a recorded deferral, not an oversight.
- The Android protocol chip keys on the satellite's advertised version and
  reads "Dish update required" for a newer satellite that still accepts this
  build; this client keys the chip on the negotiation, so that case reads as
  fine. Red here means the session cannot open.

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
| #177 input-path-honest capabilities | this wave ("Supported") | ported; the Unknown state cannot arise here (Direct only on known models) |
| #178 controller audio (+ app-wide mic chip) | #48 + this wave | ported |
| #182, #183, #186, #190, #197 changelog / notes | – | N/A |
| #184 privacy policy | #60 | ported |
| #185 Diagnostics | – | **gap** |
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
