# Moonlight hosts: what Sunshine and Wolf actually do

The facts every Dish client's Moonlight path is written against, verified
against the hosts' own code (Wolf) or on the wire (Sunshine), and kept here
so the three clients stop re-learning them. The only protocol documentation
that can be copied from is the MIT-licensed [TinkerNorth/wolf](https://github.com/TinkerNorth/wolf)
fork; Sunshine is GPL.

## Pads

- There is no capability API. The host picks the emulated device from what
  the client declares in CONTROLLER_ARRIVAL.
- Wolf reads the ACCELEROMETER and GYRO bits at arrival and no other bit.
  Every PlayStation pad it builds is one DualSense with a touchpad, whatever
  the arrival said.
- A host keeps a controller number it holds and skips a second
  CONTROLLER_ARRIVAL for it. Changing the emulated device for a number costs
  a replug: an unplug and a new arrival under one hold of the send lock.
- Wolf's app ids are hashes of title and icon; an app id of 1 names nothing
  it has.

## Pairing and trust

- Plaintext `/serverinfo` reports `PairStatus` 0 for every caller on both
  Sunshine and Wolf. Trust comes only from the mutual-TLS answer, never from
  that field.
- Wolf publishes no mDNS TXT records, so no uniqueid arrives by discovery.
  Every Dish client keys a host on its address and keeps the uniqueid as the
  witness of which machine answers there.
- A pairing's phase 5 must trust the certificate phase 1 handed out and no
  other, and a completed pairing pins that certificate over an older pin.
  All three clients do this.
- The pairing PIN is four digits; keep a leading zero.

## Refusals and quitting

- Wolf refuses in the status line: HTTP 400 with `<root status_code="400"/>`,
  401 for a client it does not know. Sunshine refuses inside a 200 with its
  own `status_code`. Read both.
- `/cancel` answers 200 whether or not anything was running; ask the host
  again to learn the truth.

## Where each client keeps the rest

- dish-linux: `src/core/moonlight/` (pads, pairing, RTSP, control) and
  `src/source/moonlight/` (the manager and the standing bindings).
- dish-windows: `src/core/moonlight/` and `src/Network/MoonlightManager.*`.
- dish-android: `core/net/moonlight/` and `docs/contract.md`.
