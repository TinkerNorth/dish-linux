# Input path benchmark

`bench_input_path` measures the dish's input send path on Linux: a controller report leaving the
thread that read it, through the processor, the routing, the session and the seal, until its
datagram reaches a satellite. It runs the real objects in process against loopback satellites, so it
needs no network, no satellite and no controller. `bench_input_path_lockstats` is the same bench in
a `DISH_LOCK_STATS` build, and adds what every lock was asked for by which thread. These numbers are
the record the hot-path work is measured against; the lock-law test (`tests/test_lock_laws.cpp`)
pins which locks each thread takes, this bench says what they cost. The bench is a port of
dish-windows' `tools/bench`, whose numbers are kept below under "Windows" for comparison.

## The path

The table is the 2.5.0 path the Before record measured; the After section below says what each
stage is now. On this branch no stage takes a lock.

| # | Stage | What it was in 2.5.0 | Locks it took in 2.5.0 |
|---|-------|------------|----------------------|
| 0 | SDL thread | `SDL_WaitEventTimeout` (with no video subsystem, SDL pumps the pads and sleeps `SDL_Delay(1)` until an event waits), then `rebuildState` per controller event: the device maps, the twin-suppression set, 21 SDL getters. Each axis change also arrives as a raw joystick event, which the bridge looks up and drops, and the loop drains the output queue and polls batteries after both | per report, as Windows' ViGEm mode measured it: `bridge.devices` 4 times, `bridge.output` twice, `bridge.suppressed` once |
| 0' | hidraw read thread | a Direct pad's `poll` and `read` on its `/dev/hidraw` node, the decoder, then `UsbGamepadManager`'s report sink, which publishes input, motion (at most 250 Hz) and touchpad (every report) | none of its own |
| 1 | Processor | `GamepadInputProcessor::publish`: dead zones, five map operations, the sender copied | `input.processor` |
| 2 | Routing | AppModel's report sender: `QString::fromStdString`, a `QHash` lookup (and the Moonlight table's, for a slot with no satellite sender) and a `std::function` copy | `app.routing` |
| 3 | Session | the hub's closure to `WifiConnection::sendReport`, which reads the bound slot and takes the client | `wifi.client` |
| 4 | Seal and send | `SatelliteClient::sendEncrypted`: two `std::vector`s, the session material, ChaCha20-Poly1305, a `sendto` with `MSG_DONTWAIT`, which drops the report when the socket buffer is full rather than wait | `satellite.material`, then `satellite.send` held across `sendto` |
| 5 | Loopback satellite | the bench's: `recvfrom`, the stamp, then the decrypt and the sequence | none (apparatus) |

The synthetic mode times stages 1 to 4 (plus the loopback hop) from the moment a producer
publishes. The receive side runs as background load in S4 to S7: the session's receive thread
decrypts each rumble and light-bar datagram and routes it through AppModel's feedback route to the
SDL output queue, taking `satellite.material`, `satellite.handlers`, `hub.bindings`,
`store.observable`, `bridge.devices` (twice), `app.audioRoutes` and `bridge.output`.

## How it is measured

### Synthetic mode

The wiring is `tests/HotPathRig.h`, which the lock-law test uses too. The processor, the SDL bridge,
the hub, the `WifiConnectionManager` sessions, the `SatelliteClient`s, the USB manager, the rumble
and motion stores and the playout engine are the production classes. Each session goes live through
the manager's real session PUT, which the suite's own TLS listener (`tests/FakePairingListener.h`)
answers on 127.0.0.1, and its UDP link opens to a loopback satellite that derives the same session
key, decrypts every datagram, acks every ping and sends feedback back. **AppModel itself is not
built:** it owns the tray icon, the real settings, the wake inhibitor and the update checker, so the
harness cannot instantiate it. Its routing is not mirrored, though: it lives in `SlotRouter`
(`src/SlotRouter.h`), which AppModel and the rig construct over the same objects, so the bench
measures the production routing; the rig binds no slot to a Moonlight host. (The Before record ran
an `AppGlue` that mirrored AppModel's glue of the time.)

The pads are SDL virtual game controllers that the real bridge has opened, so their dead zones,
rumble and light bar are what the bridge decided for them, and a rumble routes to the SDL output
queue as it would for a real pad. The producer publishes straight into the processor, so the SDL
thread's own work (stage 0) is not in a synthetic figure. Each report carries a sequence on the
right stick's X axis, encoded as 4096 plus the frame index mod 28672, because the bridge's stick
dead zone zeroes |v| <= 3277 (`tests/test_bench_support.cpp` checks the encoding against
`deadzone::applyStick`). The producer stamps `steady_clock` just before `publish`; the loopback
satellite stamps it just after `recvfrom` returns; the latency is the difference, on one clock. A
measured session's satellite thread spins on a non-blocking receive (`MSG_DONTWAIT`) while a run is
open, as the satellite's own receive loop does, so its wake-up is not in the figure; `--receiver
block` blocks it in `recvfrom` instead, for a small machine. Either way it keeps the default
scheduling policy: raising a thread needs `CAP_SYS_NICE`, which an unprivileged run does not have.

Every producer works to absolute deadlines: frame `k` of a stream is due `k` periods after the
common start, whatever happened to frame `k - 1`. It sleeps with
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` until 300 us before the deadline, then spins on
the pause instruction; `steady_clock` reads `CLOCK_MONOTONIC`, so the deadlines and the stamps are
on one clock. The GUI thread is the bench's main thread, which runs Qt's loop at 200 Hz on the same
pacer and does the GUI's work there.

| scenario | what runs |
|---|---|
| S1 | pad A at `--rate` (1000 Hz), nothing else |
| S2 | S1 plus pad B at 250 Hz, published by the same SdlInput thread, as the one SDL thread serves every pad. A dish binds one pad per satellite session (`ConnectionHub::bind` evicts the slot already bound to the connection, and `WifiConnection::sendTarget` names only its bound slot), so pad B streams to a second session |
| S3 | S2 plus the GUI while a pad plays: routing republish every 100 ms, `devices()` every 100 ms, `inspect()` at 30 Hz, `inputCounters()` at 1 Hz |
| S4 | S3 plus 100 rumbles and 10 light-bar colours a second from pad A's satellite to the receive thread, which routes them to the SDL output queue ("queued for SDL") |
| S5 | S4 plus the session's heartbeat thread (a ping every 2 s, acked) and an AudioCapture thread sending mic frames at `--mic-hz` (50) through pad A's mic sender (the hub's in 2.5.0, `SlotRouter`'s now) on pad A's session; `S5 (mic 1000 Hz)` is the `--mic-hz 1000` run |
| S6 | a Direct DualSense alone: a UsbReader thread hands `UsbGamepadManager`'s report sink a full USB DualSense report (input, motion, touchpad and battery valid, as the decoder leaves them) at `--rate`, on a third session |
| S7 | S4 plus the GUI binding and unbinding pad C on a fourth session every 200 ms, the desktop's equivalent of the satellite's 200 ms plug |

The heartbeat threads are stopped outside S5, so S5 is the only scenario with heartbeat traffic.
Each scenario runs `--secs` (3) seconds, `--reps` (3) times, interleaved after a discarded warm-up
pass of at most a second each, and the table shows the rep whose p99.9 of its first producer is the
median. `frames` counts the reports that arrived; `over 1 ms` counts those slower than 1 ms;
`longest gap ms` is the longest wait between two arrivals of that producer; `dgrams/report` is the
session's input, motion, touchpad and battery datagrams over the reports sent; `drops` is sent minus
arrived.

### ViGEm mode

`--mode vigem` is Windows-only: it plugs an Xbox 360 pad through ViGEmBus, a Windows bus driver,
and times it through the real SDL thread. The mode stays in the Linux build so the same commands run
on both systems; here it prints that it is not available and exits 0. Its Windows numbers are in the
Windows section.

### Lock statistics and the smoke run

`bench_input_path_lockstats` resets the lock counters as each run opens and reads them as it closes,
and prints, for the reported rep, every lock and thread role that took one: acquisitions, contended
acquisitions, failed `try_lock`s, the longest wait, the p99 wait from the log2 histogram, and the
longest hold. Its latencies carry a few clock reads per lock, so they are reported from
`bench_input_path` and only the locks from the twin. `--smoke` runs every scenario for 0.2 s, one
rep, no warm-up; ctest runs it as `bench_input_path_smoke` (or `bench_input_path_lockstats_smoke`)
with `--json`, alone, since a parallel test's load would skew its pacing verdict. Its exit code is
the harness's own accounting, never a latency: every producer sent and was heard, every frame left
at or after its deadline with the median within 1 ms (streams of 8 frames or more), the GUI's reads
ran, feedback reached the SDL queue, mic frames, the DualSense's motion and touchpad, and the
toggles happened, every datagram decrypted, and the results were written.

## Caveats

- **One machine, loopback.** The satellite is a thread on the same CPU, so the figure includes the
  loopback stack.
- **Synthetic mode has no SDL thread on the path**: its pump, `rebuildState`, the output drain and
  the battery poll are not in any scenario's figure; the pump's wait is measured on its own after the
  scenarios ("SDL pump wait" in the output, `pumpWait` in the JSON).
- **A Direct pad's gateway is a fake**: the hidraw read and its locks need hardware and are not
  exercised here. The write side runs through the claim's output worker and its `HidReportWriter`,
  whose real writer is tested against a pipe (`tests/test_output_worker.cpp`). A hidraw write has
  no cap of Dish's own: hidraw reports every node writable, and the USB core bounds an interrupt
  OUT transfer at 5 s, on the claim's worker and nowhere else.
- **SDL's HIDAPI rumble has a floor of its own on Windows**: SDL2 2.32.10's HIDAPI driver thread
  sleeps 10 ms after each OUT report it sends. The Linux builds' SDL 2.30 was not checked for it.
- **Default scheduling policy and timer slack**, other work on the machine as it happened. Compare
  runs of one binary on one machine.

## Linux: before, 2.5.0 input path

A GitHub-hosted `ubuntu-24.04` runner (image 20261026.354.1): AMD EPYC 7763 64-Core Processor, 4
logical CPUs, Ubuntu 24.04.5 LTS, Linux 7.0.0-1017-azure, Qt 6.10.3, SDL 2.30.0. Release build
(`-O3`, LTO) of this branch at 50c5581, taken by `.github/workflows/measurements.yml` with the label
`before-2.5.0`, at the defaults (3 s per run, 3 reps, pad A and the DualSense at 1000 Hz, pad B at
250 Hz, mic at 50 Hz, spinning receivers), plus `--mic-hz 1000 --scenarios S5`. Latencies are in
microseconds.

| mode | scenario | producer | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | dgrams/report | drops |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | A | 1000 | 3000 | 10.5 | 17.6 | 24.1 | 49.6 | 0 | 1.015 | 1.00 | 0 |
| synthetic | S2 | A | 1000 | 3000 | 12.6 | 18.7 | 25.4 | 60.4 | 0 | 1.089 | 1.00 | 0 |
| synthetic | S2 | B | 250 | 750 | 8.7 | 10.7 | 16.1 | 16.1 | 0 | 4.089 | 1.00 | 0 |
| synthetic | S3 | A | 1000 | 3000 | 11.8 | 17.4 | 22.7 | 51.4 | 0 | 2.610 | 1.00 | 0 |
| synthetic | S3 | B | 250 | 750 | 8.7 | 10.5 | 14.6 | 14.6 | 0 | 5.482 | 1.00 | 0 |
| synthetic | S4 | A | 1000 | 3000 | 11.1 | 20.1 | 26.2 | 30.8 | 0 | 1.455 | 1.00 | 0 |
| synthetic | S4 | B | 250 | 750 | 8.7 | 12.0 | 36.5 | 36.5 | 0 | 4.078 | 1.00 | 0 |
| synthetic | S5 | A | 1000 | 3000 | 12.2 | 18.6 | 46.3 | 82.9 | 0 | 2.483 | 1.00 | 0 |
| synthetic | S5 | B | 250 | 750 | 8.7 | 10.8 | 37.1 | 37.1 | 0 | 4.516 | 1.00 | 0 |
| synthetic | S6 | DualSense | 1000 | 3000 | 10.7 | 16.3 | 26.6 | 54.0 | 0 | 1.022 | 2.22 | 0 |
| synthetic | S7 | A | 1000 | 3000 | 11.8 | 19.3 | 24.9 | 55.9 | 0 | 3.606 | 1.00 | 0 |
| synthetic | S7 | B | 250 | 750 | 8.7 | 11.2 | 15.8 | 15.8 | 0 | 5.616 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | A | 1000 | 3000 | 32.1 | 59.5 | 132.4 | 732.5 | 0 | 1.698 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | B | 250 | 750 | 8.7 | 10.4 | 77.7 | 77.7 | 0 | 4.099 | 1.00 | 0 |

Side traffic in the same runs:

| scenario | rumbles sent | light-bars sent | queued for SDL | mic sent | mic received | pings | toggles |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| S1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| S2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| S3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| S4 | 300 | 30 | 330 | 0 | 0 | 0 | 0 |
| S5 | 300 | 30 | 330 | 150 | 150 | 2 | 0 |
| S6 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| S7 | 300 | 30 | 330 | 0 | 0 | 0 | 15 |
| S5 (mic 1000 Hz) | 300 | 30 | 330 | 3000 | 3000 | 2 | 0 |

A shared runner is noisy, and its noise floor is in the first row: with one pad and nothing else, S1
had a max of 49.6 us and a longest gap of 1.015 ms. A difference inside that is not a finding; each
release's `measurements-<version>.md` prints this floor beside its headline.

Pad A alone costs 10.5 us at the median, 17.6 us at p99 and 24.1 us at p99.9 from publish to the
loopback satellite (14.9, 26.6 and 36.9 us on the Windows machine below), and its median stays
between 10.5 and 12.6 us from S1 to S7. Every report pays the five locks it pays on Windows
(`input.processor`, `app.routing`, `wifi.client`, `satellite.material`, `satellite.send`; the
SdlInput thread took exactly 5.00 a report in S1) and the same three heap allocations, and
`satellite.send` is held across `sendto` although the Linux `sendto` never waits for buffer space:
up to 62 us with one pad alone, and once 672 us in S4. Contention appears where threads share a
lock, as on Windows. The 1 kHz mic shares pad A's session: the SdlInput thread waited on
`satellite.send` for 1,177 of its 3,750 sends (up to 1.24 ms), which takes pad A's median from 12.2
to 32.1 us and its max to 733 us. The receive thread
takes `bridge.devices` twice for every feedback message and meets the GUI's `devices()` reads there
(waits up to 40 us), and the GUI's routing republish meets the SDL thread on `app.routing` (up to
28 us). A Direct DualSense sends 2.22 datagrams a report: its touchpad goes out with every report,
its motion at the processor's 250 Hz cap. No producer sent a frame before its deadline; pad A's
median frame left 0.1 to 0.2 us after it, and pad B's 11 to 34 us, because B's deadlines fall on
A's and its thread sends A's frame first.

**Session churn, Linux only.** A heartbeat thread pinged about every 2 s in every scenario, although
the bench stops every session's heartbeat after setup, and in S3 and S7 the GUI thread took
`satellite.material` once and set three satellite handlers, which is `WifiConnection::markConnected`
re-establishing a session, and held one session's `wifi.client` for 323 ms in S3 and 285 ms in S7.
The loopback satellite answers the address of the first datagram it received, so a session that gets
a new client stops hearing its acks and falters after two missed pings; each discovery scan ends in
`autoReconnectAll`, whose `connectTo` takes a Faltering session straight to Linking without tearing
its client down; `markConnected` then hands the new client to `ClientRef::set`, which lets go of the
old one under `wifi.client`, and when that is its last reference the destructor joins the heartbeat
thread (up to 100 ms) and the receive thread (up to 500 ms in `recvfrom`) with the lock held. The
SDL thread did not wait on it in these runs, so the lock was another session's, but a report on a
Faltering session would wait out both joins. What replaced the first client is not established;
After, below, re-takes this record with the loopback satellite answering the address the dish last
sent from, which measures no such hold.

The lock table, from `bench_input_path_lockstats` at the same settings, for the reported rep of each
scenario (times in microseconds):

| mode | scenario | lock | role | acquisitions | contended | try fails | max wait us | p99 wait us | max hold us |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | input.processor | SdlInput | 3000 | 0 | 0 | 0.7 | 0.3 | 3.2 |
| synthetic | S1 | app.routing | SdlInput | 3000 | 0 | 0 | 1.9 | 0.1 | 4.6 |
| synthetic | S1 | satellite.material | SdlInput | 3000 | 0 | 0 | 0.2 | 0.1 | 2.2 |
| synthetic | S1 | satellite.material | Heartbeat | 1 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S1 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 2.0 |
| synthetic | S1 | wifi.client | Gui | 24 | 0 | 0 | 0.6 | 1.0 | 0.5 |
| synthetic | S1 | wifi.client | SdlInput | 3000 | 0 | 0 | 0.8 | 0.1 | 1.2 |
| synthetic | S1 | satellite.send | SdlInput | 3000 | 0 | 0 | 0.1 | 0.1 | 62.0 |
| synthetic | S1 | satellite.send | Heartbeat | 1 | 0 | 0 | 0.0 | 0.1 | 25.3 |
| synthetic | S2 | input.processor | SdlInput | 3750 | 0 | 0 | 1.6 | 0.5 | 3.5 |
| synthetic | S2 | app.routing | SdlInput | 3750 | 0 | 0 | 0.6 | 0.3 | 5.5 |
| synthetic | S2 | satellite.material | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 1.6 |
| synthetic | S2 | satellite.material | Heartbeat | 2 | 0 | 0 | 0.1 | 0.3 | 0.2 |
| synthetic | S2 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 1.8 |
| synthetic | S2 | wifi.client | Gui | 21 | 0 | 0 | 0.3 | 0.5 | 0.5 |
| synthetic | S2 | wifi.client | SdlInput | 3750 | 0 | 0 | 0.8 | 0.1 | 0.7 |
| synthetic | S2 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 50.2 |
| synthetic | S2 | satellite.send | Heartbeat | 2 | 0 | 0 | 0.0 | 0.0 | 15.5 |
| synthetic | S3 | input.processor | Gui | 99 | 0 | 0 | 1.9 | 2.0 | 1.1 |
| synthetic | S3 | input.processor | SdlInput | 3750 | 0 | 0 | 1.5 | 0.3 | 3.3 |
| synthetic | S3 | bridge.devices | Gui | 30 | 0 | 0 | 0.2 | 0.3 | 4.6 |
| synthetic | S3 | app.routing | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 6.7 |
| synthetic | S3 | app.routing | SdlInput | 3750 | 0 | 0 | 1.1 | 0.1 | 5.4 |
| synthetic | S3 | satellite.material | Gui | 1 | 0 | 0 | 0.1 | 0.3 | 0.1 |
| synthetic | S3 | satellite.material | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 0.4 |
| synthetic | S3 | satellite.material | Heartbeat | 2 | 0 | 0 | 0.1 | 0.3 | 0.5 |
| synthetic | S3 | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.3 | 1.8 |
| synthetic | S3 | wifi.client | Gui | 22 | 0 | 0 | 0.2 | 0.3 | 323252.4 |
| synthetic | S3 | wifi.client | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 1.1 |
| synthetic | S3 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 116.6 |
| synthetic | S3 | satellite.send | Heartbeat | 2 | 0 | 0 | 0.0 | 0.1 | 26.2 |
| synthetic | S3 | satellite.handlers | Gui | 3 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S3 | store.observable | Gui | 90 | 0 | 0 | 0.4 | 0.5 | 0.5 |
| synthetic | S4 | input.processor | Gui | 99 | 1 | 0 | 2.5 | 4.1 | 1.1 |
| synthetic | S4 | input.processor | SdlInput | 3750 | 0 | 0 | 1.5 | 0.3 | 3.1 |
| synthetic | S4 | bridge.devices | Gui | 30 | 1 | 0 | 2.0 | 2.0 | 12.7 |
| synthetic | S4 | bridge.devices | Receive | 660 | 1 | 0 | 14.3 | 0.1 | 12.2 |
| synthetic | S4 | app.routing | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 15.5 |
| synthetic | S4 | app.routing | SdlInput | 3750 | 9 | 0 | 27.8 | 0.1 | 3.8 |
| synthetic | S4 | bridge.output | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 71.3 |
| synthetic | S4 | satellite.material | SdlInput | 3750 | 2 | 0 | 0.3 | 0.1 | 0.5 |
| synthetic | S4 | satellite.material | Receive | 660 | 1 | 0 | 21.3 | 0.5 | 0.3 |
| synthetic | S4 | satellite.material | Heartbeat | 2 | 0 | 0 | 0.2 | 0.3 | 0.3 |
| synthetic | S4 | satellite.latency | Gui | 15 | 0 | 0 | 0.1 | 0.3 | 1.8 |
| synthetic | S4 | wifi.client | Gui | 27 | 0 | 0 | 0.7 | 1.0 | 0.4 |
| synthetic | S4 | wifi.client | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 1.2 |
| synthetic | S4 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 671.9 |
| synthetic | S4 | satellite.send | Heartbeat | 2 | 0 | 0 | 0.0 | 0.1 | 53.1 |
| synthetic | S4 | satellite.handlers | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.6 |
| synthetic | S4 | hub.bindings | Receive | 330 | 0 | 0 | 0.2 | 0.3 | 0.3 |
| synthetic | S4 | store.observable | Gui | 90 | 0 | 0 | 0.7 | 1.0 | 0.2 |
| synthetic | S4 | store.observable | Receive | 300 | 0 | 0 | 0.4 | 0.1 | 0.5 |
| synthetic | S4 | app.audioRoutes | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S5 | input.processor | Gui | 99 | 0 | 0 | 0.3 | 0.5 | 13.5 |
| synthetic | S5 | input.processor | SdlInput | 3750 | 0 | 0 | 0.8 | 0.3 | 8.5 |
| synthetic | S5 | bridge.devices | Gui | 30 | 0 | 0 | 0.1 | 0.3 | 4.4 |
| synthetic | S5 | bridge.devices | Receive | 660 | 1 | 0 | 1.5 | 0.1 | 24.1 |
| synthetic | S5 | app.routing | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 6.7 |
| synthetic | S5 | app.routing | SdlInput | 3750 | 0 | 0 | 0.3 | 0.1 | 13.0 |
| synthetic | S5 | bridge.output | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 14.3 |
| synthetic | S5 | satellite.material | SdlInput | 3750 | 1 | 0 | 0.3 | 0.1 | 1.6 |
| synthetic | S5 | satellite.material | Receive | 664 | 2 | 0 | 0.4 | 0.5 | 0.6 |
| synthetic | S5 | satellite.material | Heartbeat | 3 | 0 | 0 | 0.1 | 0.3 | 0.2 |
| synthetic | S5 | satellite.material | AudioCapture | 150 | 1 | 0 | 0.4 | 0.3 | 0.4 |
| synthetic | S5 | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.3 | 1.3 |
| synthetic | S5 | satellite.latency | Receive | 2 | 0 | 0 | 0.0 | 0.1 | 0.2 |
| synthetic | S5 | wifi.client | Gui | 21 | 0 | 0 | 0.3 | 0.5 | 0.3 |
| synthetic | S5 | wifi.client | SdlInput | 3750 | 1 | 0 | 0.4 | 0.1 | 1.4 |
| synthetic | S5 | wifi.client | AudioCapture | 150 | 1 | 0 | 0.6 | 0.5 | 0.6 |
| synthetic | S5 | satellite.send | SdlInput | 3750 | 13 | 0 | 33.1 | 0.1 | 31.3 |
| synthetic | S5 | satellite.send | Heartbeat | 3 | 0 | 0 | 0.0 | 0.1 | 43.9 |
| synthetic | S5 | satellite.send | AudioCapture | 150 | 10 | 0 | 89.3 | 131.1 | 23.0 |
| synthetic | S5 | satellite.handlers | Receive | 332 | 0 | 0 | 0.1 | 0.1 | 0.7 |
| synthetic | S5 | hub.bindings | Receive | 330 | 0 | 0 | 0.2 | 0.3 | 0.4 |
| synthetic | S5 | store.observable | Gui | 90 | 0 | 0 | 0.6 | 1.0 | 0.3 |
| synthetic | S5 | store.observable | Receive | 300 | 0 | 0 | 0.1 | 0.3 | 0.5 |
| synthetic | S5 | app.audioRoutes | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.4 |
| synthetic | S6 | input.processor | UsbReader | 9000 | 0 | 0 | 1.1 | 0.3 | 3.0 |
| synthetic | S6 | app.routing | UsbReader | 6671 | 0 | 0 | 0.9 | 0.1 | 4.3 |
| synthetic | S6 | satellite.material | UsbReader | 6671 | 0 | 0 | 0.2 | 0.1 | 0.6 |
| synthetic | S6 | satellite.material | Heartbeat | 2 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S6 | satellite.latency | Gui | 12 | 0 | 0 | 0.4 | 0.5 | 1.6 |
| synthetic | S6 | wifi.client | Gui | 22 | 0 | 0 | 0.6 | 1.0 | 0.6 |
| synthetic | S6 | wifi.client | UsbReader | 6671 | 0 | 0 | 0.3 | 0.1 | 0.3 |
| synthetic | S6 | satellite.send | UsbReader | 6671 | 0 | 0 | 0.2 | 0.1 | 58.4 |
| synthetic | S6 | satellite.send | Heartbeat | 2 | 0 | 0 | 0.0 | 0.1 | 25.1 |
| synthetic | S7 | input.processor | Gui | 100 | 0 | 0 | 0.4 | 0.5 | 0.9 |
| synthetic | S7 | input.processor | SdlInput | 3750 | 0 | 0 | 1.1 | 0.3 | 3.4 |
| synthetic | S7 | bridge.devices | Gui | 30 | 0 | 0 | 0.1 | 0.3 | 4.4 |
| synthetic | S7 | bridge.devices | Receive | 660 | 1 | 0 | 2.9 | 0.1 | 24.6 |
| synthetic | S7 | app.routing | Gui | 45 | 0 | 0 | 0.1 | 0.1 | 7.3 |
| synthetic | S7 | app.routing | SdlInput | 3750 | 6 | 0 | 17.7 | 0.1 | 5.5 |
| synthetic | S7 | bridge.output | Receive | 330 | 0 | 0 | 0.2 | 0.1 | 109.0 |
| synthetic | S7 | satellite.material | Gui | 1 | 0 | 0 | 0.1 | 0.3 | 0.1 |
| synthetic | S7 | satellite.material | SdlInput | 3750 | 1 | 0 | 3.3 | 0.1 | 0.2 |
| synthetic | S7 | satellite.material | Receive | 660 | 0 | 0 | 0.5 | 0.5 | 0.5 |
| synthetic | S7 | satellite.material | Heartbeat | 3 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S7 | satellite.latency | Gui | 13 | 0 | 0 | 0.1 | 0.3 | 1.5 |
| synthetic | S7 | wifi.client | Gui | 22 | 0 | 0 | 0.2 | 0.3 | 285111.7 |
| synthetic | S7 | wifi.client | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S7 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 41.1 |
| synthetic | S7 | satellite.send | Heartbeat | 3 | 0 | 0 | 0.0 | 0.1 | 24.5 |
| synthetic | S7 | satellite.handlers | Gui | 3 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S7 | satellite.handlers | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.8 |
| synthetic | S7 | hub.bindings | Gui | 15 | 0 | 0 | 0.1 | 0.1 | 1.1 |
| synthetic | S7 | hub.bindings | Receive | 330 | 0 | 0 | 0.4 | 0.3 | 0.4 |
| synthetic | S7 | store.observable | Gui | 158 | 0 | 0 | 0.5 | 0.5 | 0.3 |
| synthetic | S7 | store.observable | Receive | 300 | 0 | 0 | 0.2 | 0.3 | 0.6 |
| synthetic | S7 | app.audioRoutes | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S5 (mic 1000 Hz) | input.processor | Gui | 99 | 0 | 0 | 0.2 | 0.3 | 1.7 |
| synthetic | S5 (mic 1000 Hz) | input.processor | SdlInput | 3750 | 0 | 0 | 1.4 | 0.3 | 2.7 |
| synthetic | S5 (mic 1000 Hz) | bridge.devices | Gui | 30 | 2 | 0 | 1.7 | 2.0 | 1.9 |
| synthetic | S5 (mic 1000 Hz) | bridge.devices | Receive | 660 | 4 | 0 | 40.0 | 0.1 | 4.0 |
| synthetic | S5 (mic 1000 Hz) | app.routing | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 5.4 |
| synthetic | S5 (mic 1000 Hz) | app.routing | SdlInput | 3750 | 1 | 0 | 17.2 | 0.1 | 3.2 |
| synthetic | S5 (mic 1000 Hz) | bridge.output | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 28.5 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | SdlInput | 3750 | 3 | 0 | 1.2 | 0.1 | 1.3 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | Receive | 664 | 0 | 0 | 0.3 | 0.3 | 0.4 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | Heartbeat | 3 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | AudioCapture | 3000 | 1 | 0 | 0.3 | 0.1 | 0.5 |
| synthetic | S5 (mic 1000 Hz) | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.3 | 1.5 |
| synthetic | S5 (mic 1000 Hz) | satellite.latency | Receive | 2 | 0 | 0 | 0.0 | 0.1 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | Gui | 22 | 0 | 0 | 0.4 | 0.5 | 0.7 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | SdlInput | 3750 | 4 | 0 | 1.1 | 0.1 | 1.6 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | AudioCapture | 3000 | 4 | 0 | 1.3 | 0.3 | 1.4 |
| synthetic | S5 (mic 1000 Hz) | satellite.send | SdlInput | 3750 | 1177 | 0 | 1237.4 | 32.8 | 19.9 |
| synthetic | S5 (mic 1000 Hz) | satellite.send | Heartbeat | 3 | 0 | 0 | 0.0 | 0.1 | 18.1 |
| synthetic | S5 (mic 1000 Hz) | satellite.send | AudioCapture | 3000 | 117 | 0 | 25.0 | 32.8 | 58.2 |
| synthetic | S5 (mic 1000 Hz) | satellite.handlers | Receive | 332 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S5 (mic 1000 Hz) | hub.bindings | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | store.observable | Gui | 90 | 0 | 0 | 0.2 | 0.3 | 13.9 |
| synthetic | S5 (mic 1000 Hz) | store.observable | Receive | 300 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | app.audioRoutes | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.3 |

### Commands (Linux)

The measurements workflow takes the record; a run of it on a branch is

```
gh workflow run measurements.yml --ref <branch> -f version=<label>
```

and on the runner it does, from the repository root:

```
cmake --preset release -B build-measure-plain -DDISH_BUILD_BENCH=ON -DDISH_LOCK_STATS=OFF
cmake --preset release -B build-measure-lockstats -DDISH_BUILD_BENCH=ON -DDISH_LOCK_STATS=ON
cmake --build build-measure-plain --target bench_input_path
cmake --build build-measure-lockstats --target bench_input_path_lockstats DishLockLawTests
(cd build-measure-lockstats && ctest -R "^lock law")
build-measure-plain/tools/bench/bench_input_path --json measurements-raw/bench_input_path.json
build-measure-plain/tools/bench/bench_input_path --mic-hz 1000 --scenarios S5 \
    --json measurements-raw/bench_input_path-mic1000.json
build-measure-lockstats/tools/bench/bench_input_path_lockstats \
    --json measurements-raw/bench_input_path_lockstats.json
build-measure-lockstats/tools/bench/bench_input_path_lockstats --mic-hz 1000 --scenarios S5 \
    --json measurements-raw/bench_input_path_lockstats-mic1000.json
python3 scripts/measurements-report.py --input-dir measurements-raw --output-dir measurements \
    --version <label> --previous-dir previous-measurements

# The smoke runs: linux-ci.yml's Release build has -DDISH_BUILD_BENCH=ON, so
ctest --preset release -R bench_input_path
```

## Linux: after, this branch

The same runner image and settings as the Before record (a GitHub-hosted `ubuntu-24.04` runner,
image 20261026.354.1: AMD EPYC 7763, 4 logical CPUs, Linux 7.0.0-1017-azure), Release builds, taken
by `measurements.yml` at `ead82af` with the label `after-2.5.0`; the commits after it change a test
and this file. The before figures are the Before record re-taken on another runner of the same
image, at 02184bb with one change to the rig, the loopback satellite answering the address the dish
last sent from as this branch's rig does (label `before-2.5.0-rig`). Its latency rows match the
first take's within the runner's noise, and its GUI held no `wifi.client` longer than 0.5 us, where
the first take held one for 323 ms in S3 and 285 ms in S7. S6's finger now moves with the stick (a
resting finger is gated to nothing after its two heals, which the bench's accounting would read as a
lost touchpad); the before bench holds it still, and 2.5.0 sends it with every report either way.

Every report now leaves with no lock and no allocation: the bridge reads a pad from its own record
on the SDL thread, the processor writes the device's record and latches an unchanged report,
`SlotRouter` reads the published table inside a grace bracket, and the session seals on the stack
and sends with `MSG_DONTWAIT` under a compare-and-swap gate. Feedback from the receive thread and
the mic's frames go through the same table.

Paired, microseconds:

| scenario | producer | p50 before | p50 after | p99 before | p99 after | p99.9 before | p99.9 after | dgrams/report before | dgrams/report after |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| S1 | A | 10.8 | 9.6 | 17.9 | 15.8 | 23.6 | 21.2 | 1.00 | 1.00 |
| S2 | A | 11.5 | 10.7 | 18.1 | 15.3 | 24.3 | 23.0 | 1.00 | 1.00 |
| S2 | B | 8.4 | 8.1 | 10.3 | 9.6 | 53.7 | 30.7 | 1.00 | 1.00 |
| S3 | A | 11.4 | 10.3 | 16.9 | 17.7 | 25.9 | 21.6 | 1.00 | 1.00 |
| S3 | B | 8.4 | 8.0 | 10.4 | 9.7 | 25.9 | 150.6 | 1.00 | 1.00 |
| S4 | A | 11.8 | 11.0 | 16.1 | 14.1 | 22.1 | 18.4 | 1.00 | 1.00 |
| S4 | B | 8.5 | 8.0 | 9.9 | 8.7 | 14.2 | 12.4 | 1.00 | 1.00 |
| S5 | A | 11.0 | 10.6 | 19.6 | 32.3 | 41.1 | 109.1 | 1.00 | 1.00 |
| S5 | B | 8.4 | 22.4 | 9.8 | 30.6 | 23.1 | 48.2 | 1.00 | 1.00 |
| S6 | DualSense | 10.1 | 10.3 | 17.5 | 15.1 | 23.4 | 21.7 | 2.22 | 1.33 |
| S7 | A | 11.1 | 10.9 | 18.3 | 14.9 | 25.0 | 23.9 | 1.00 | 1.00 |
| S7 | B | 8.5 | 8.0 | 10.4 | 12.1 | 26.5 | 160.9 | 1.00 | 1.00 |
| S5 (mic 1000 Hz) | A | 31.0 | 24.6 | 40.9 | 32.6 | 60.1 | 354.0 | 1.00 | 1.00 |
| S5 (mic 1000 Hz) | B | 8.5 | 8.0 | 9.5 | 11.3 | 11.5 | 26.3 | 1.00 | 1.00 |

Pad A's median is 0.2 to 1.2 us lower from S1 to S7 (S1: 10.8 to 9.6 us at the median, 17.9 to 15.8
us at p99), small next to the runner's noise: as on Windows, the five locks and three allocations a
report paid were cheap while no other thread held them. What they cost was the waits. The 1 kHz mic
made the SdlInput thread wait on `satellite.send` for 2,320 of its 3,750 sends, up to 801 us, and
took pad A's median from 11.0 to 31.0 us; now it is 24.6 us there. What is left is the effect
Windows measured: reports and mic frames share the session's send gate, the bench paces both from
one start, and at 1 kHz every mic frame is due with one of pad A's reports, so a report that loses
the gate waits out one mic send. A Direct DualSense sends 1.33 datagrams a report instead of 2.22:
its touchpad goes out the way Android sends it, a move at most every 8 ms, instead of with every
report.

The rows that read worse are single runs. Pad B's p99.9 is its slowest frame (750 frames a run), so
one preemption sets it, and S5's pad B median of 22.4 us is one rep's: the lockstats twin in the
same run measured 8.2 us there. The PR's measurements run of the same tree, on another runner at the
same time, measured S5 at 11.8, 16.4 and 22.1 us (p50, p99, p99.9) for pad A and 8.1, 14.1 and 24.2
us for pad B, pad B's p99.9 at 14.8 us in S3 and 96.5 us in S7, and pad A with the 1 kHz mic at
20.6, 29.1 and 118.4 us.

The after runs in full, the same columns as Before:

| mode | scenario | producer | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | dgrams/report | drops |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | A | 1000 | 3000 | 9.6 | 15.8 | 21.2 | 22.1 | 0 | 1.277 | 1.00 | 0 |
| synthetic | S2 | A | 1000 | 3000 | 10.7 | 15.3 | 23.0 | 50.1 | 0 | 3.275 | 1.00 | 0 |
| synthetic | S2 | B | 250 | 750 | 8.1 | 9.6 | 30.7 | 30.7 | 0 | 6.274 | 1.00 | 0 |
| synthetic | S3 | A | 1000 | 3000 | 10.3 | 17.7 | 21.6 | 24.3 | 0 | 2.832 | 1.00 | 0 |
| synthetic | S3 | B | 250 | 750 | 8.0 | 9.7 | 150.6 | 150.6 | 0 | 4.150 | 1.00 | 0 |
| synthetic | S4 | A | 1000 | 3000 | 11.0 | 14.1 | 18.4 | 52.7 | 0 | 1.345 | 1.00 | 0 |
| synthetic | S4 | B | 250 | 750 | 8.0 | 8.7 | 12.4 | 12.4 | 0 | 4.031 | 1.00 | 0 |
| synthetic | S5 | A | 1000 | 3000 | 10.6 | 32.3 | 109.1 | 152.6 | 0 | 3.072 | 1.00 | 0 |
| synthetic | S5 | B | 250 | 750 | 22.4 | 30.6 | 48.2 | 48.2 | 0 | 6.056 | 1.00 | 0 |
| synthetic | S6 | DualSense | 1000 | 3000 | 10.3 | 15.1 | 21.7 | 28.3 | 0 | 1.059 | 1.33 | 0 |
| synthetic | S7 | A | 1000 | 3000 | 10.9 | 14.9 | 23.9 | 27.4 | 0 | 1.571 | 1.00 | 0 |
| synthetic | S7 | B | 250 | 750 | 8.0 | 12.1 | 160.9 | 160.9 | 0 | 4.571 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | A | 1000 | 3000 | 24.6 | 32.6 | 354.0 | 688.7 | 0 | 3.566 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | B | 250 | 750 | 8.0 | 11.3 | 26.3 | 26.3 | 0 | 5.573 | 1.00 | 0 |

The side traffic is the Before record's: 300 rumbles and 30 light-bar colours in S4, S5 and S7, all
330 queued for SDL, 150 mic frames in S5 and 3,000 with `--mic-hz 1000`, two pings, 15 binding
toggles in S7.

The lock table from `bench_input_path_lockstats` at the same settings. No row belongs to the SDL
input, USB reader, receive or audio capture roles, where the re-take counted 127,500, 35,700, 11,844
and 9,450 acquisitions across the scenarios, and the measurements gate, enforced now, passed on all
78 rows. What remains is the GUI's own work and the session reaper's (`wifi.reaper`, role Other).
The GUI's republish holds `app.routes` while it waits out the reports still reading the table it
replaced, up to 44 us; no hot thread takes that lock.

| mode | scenario | lock | role | acquisitions | contended | try fails | max wait us | p99 wait us | max hold us |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | wifi.reaper | Gui | 3 | 0 | 0 | 0.0 | 0.1 | 0.2 |
| synthetic | S1 | wifi.reaper | Other | 5 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S1 | satellite.material | Gui | 2 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S1 | satellite.latency | Gui | 14 | 0 | 0 | 0.1 | 0.1 | 2.2 |
| synthetic | S1 | wifi.client | Gui | 32 | 0 | 0 | 0.7 | 1.0 | 0.5 |
| synthetic | S1 | satellite.handlers | Gui | 14 | 0 | 0 | 0.0 | 0.1 | 0.7 |
| synthetic | S2 | wifi.reaper | Gui | 2 | 0 | 0 | 0.0 | 0.1 | 0.3 |
| synthetic | S2 | wifi.reaper | Other | 5 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S2 | satellite.material | Gui | 3 | 0 | 0 | 0.1 | 0.3 | 0.2 |
| synthetic | S2 | satellite.latency | Gui | 14 | 0 | 0 | 0.1 | 0.3 | 1.8 |
| synthetic | S2 | wifi.client | Gui | 29 | 0 | 0 | 0.9 | 1.0 | 0.6 |
| synthetic | S2 | satellite.handlers | Gui | 21 | 0 | 0 | 0.0 | 0.1 | 0.8 |
| synthetic | S3 | wifi.reaper | Gui | 3 | 0 | 0 | 0.0 | 0.1 | 0.4 |
| synthetic | S3 | wifi.reaper | Other | 6 | 0 | 0 | 0.6 | 1.0 | 0.2 |
| synthetic | S3 | bridge.devices | Gui | 150 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S3 | usb.manager | Gui | 60 | 0 | 0 | 0.1 | 0.3 | 0.5 |
| synthetic | S3 | satellite.material | Gui | 3 | 0 | 0 | 0.1 | 0.3 | 0.2 |
| synthetic | S3 | satellite.latency | Gui | 15 | 0 | 0 | 0.1 | 0.3 | 1.7 |
| synthetic | S3 | wifi.client | Gui | 123 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S3 | satellite.handlers | Gui | 21 | 0 | 0 | 0.1 | 0.1 | 0.9 |
| synthetic | S3 | hub.bindings | Gui | 30 | 0 | 0 | 0.3 | 0.5 | 0.5 |
| synthetic | S3 | store.observable | Gui | 180 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S3 | app.audioRoutes | Gui | 90 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S3 | app.routes | Gui | 30 | 0 | 0 | 0.1 | 0.3 | 11.3 |
| synthetic | S4 | wifi.reaper | Gui | 3 | 0 | 0 | 0.0 | 0.1 | 0.2 |
| synthetic | S4 | wifi.reaper | Other | 6 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S4 | bridge.devices | Gui | 150 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S4 | usb.manager | Gui | 60 | 0 | 0 | 0.1 | 0.3 | 0.8 |
| synthetic | S4 | satellite.material | Gui | 3 | 0 | 0 | 0.1 | 0.3 | 0.2 |
| synthetic | S4 | satellite.latency | Gui | 15 | 0 | 0 | 0.1 | 0.3 | 1.4 |
| synthetic | S4 | wifi.client | Gui | 123 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S4 | satellite.handlers | Gui | 21 | 0 | 0 | 0.1 | 0.3 | 0.8 |
| synthetic | S4 | hub.bindings | Gui | 30 | 0 | 0 | 0.5 | 1.0 | 0.4 |
| synthetic | S4 | store.observable | Gui | 180 | 0 | 0 | 0.4 | 0.1 | 0.2 |
| synthetic | S4 | app.audioRoutes | Gui | 90 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S4 | app.routes | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 8.1 |
| synthetic | S5 | wifi.reaper | Gui | 3 | 0 | 0 | 0.0 | 0.1 | 0.2 |
| synthetic | S5 | wifi.reaper | Other | 5 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 | bridge.devices | Gui | 150 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S5 | usb.manager | Gui | 60 | 0 | 0 | 0.4 | 0.5 | 0.6 |
| synthetic | S5 | satellite.material | Gui | 3 | 0 | 0 | 0.3 | 0.3 | 0.2 |
| synthetic | S5 | satellite.latency | Gui | 15 | 0 | 0 | 0.4 | 0.5 | 1.9 |
| synthetic | S5 | wifi.client | Gui | 123 | 0 | 0 | 0.5 | 0.3 | 0.5 |
| synthetic | S5 | satellite.handlers | Gui | 21 | 0 | 0 | 0.1 | 0.3 | 0.9 |
| synthetic | S5 | hub.bindings | Gui | 30 | 0 | 0 | 0.4 | 0.5 | 0.2 |
| synthetic | S5 | store.observable | Gui | 180 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S5 | app.audioRoutes | Gui | 90 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S5 | app.routes | Gui | 30 | 0 | 0 | 0.1 | 0.3 | 43.8 |
| synthetic | S6 | wifi.reaper | Gui | 2 | 0 | 0 | 0.0 | 0.1 | 0.5 |
| synthetic | S6 | wifi.reaper | Other | 4 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S6 | satellite.material | Gui | 3 | 0 | 0 | 0.1 | 0.3 | 0.2 |
| synthetic | S6 | satellite.latency | Gui | 14 | 0 | 0 | 0.2 | 0.3 | 1.7 |
| synthetic | S6 | wifi.client | Gui | 29 | 0 | 0 | 0.8 | 1.0 | 0.3 |
| synthetic | S6 | satellite.handlers | Gui | 21 | 0 | 0 | 0.0 | 0.1 | 0.8 |
| synthetic | S7 | wifi.reaper | Gui | 1 | 0 | 0 | 0.0 | 0.0 | 0.2 |
| synthetic | S7 | wifi.reaper | Other | 2 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S7 | bridge.devices | Gui | 256 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S7 | usb.manager | Gui | 90 | 0 | 0 | 0.1 | 0.3 | 0.6 |
| synthetic | S7 | satellite.material | Gui | 1 | 0 | 0 | 0.1 | 0.1 | 0.4 |
| synthetic | S7 | satellite.latency | Gui | 17 | 0 | 0 | 0.2 | 0.3 | 1.2 |
| synthetic | S7 | wifi.client | Gui | 185 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S7 | satellite.handlers | Gui | 7 | 0 | 0 | 0.0 | 0.1 | 0.8 |
| synthetic | S7 | hub.bindings | Gui | 60 | 0 | 0 | 0.4 | 0.5 | 1.3 |
| synthetic | S7 | store.observable | Gui | 308 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S7 | app.audioRoutes | Gui | 158 | 0 | 0 | 0.1 | 0.1 | 0.4 |
| synthetic | S7 | app.routes | Gui | 45 | 0 | 0 | 0.1 | 0.1 | 14.3 |
| synthetic | S5 (mic 1000 Hz) | wifi.reaper | Gui | 3 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | wifi.reaper | Other | 6 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | bridge.devices | Gui | 150 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S5 (mic 1000 Hz) | usb.manager | Gui | 60 | 0 | 0 | 0.1 | 0.3 | 0.7 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | Gui | 3 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | satellite.latency | Gui | 15 | 0 | 0 | 0.1 | 0.3 | 1.5 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | Gui | 123 | 0 | 0 | 0.4 | 0.5 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | satellite.handlers | Gui | 21 | 0 | 0 | 0.2 | 0.3 | 0.8 |
| synthetic | S5 (mic 1000 Hz) | hub.bindings | Gui | 30 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | store.observable | Gui | 180 | 0 | 0 | 0.4 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | app.audioRoutes | Gui | 90 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | app.routes | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 35.2 |

**Session churn, after.** Sessions still re-establish during the Linux runs, as they did in the
re-take: the GUI applies new session material and handlers one to three times a scenario and hands
one to three replaced clients to the reaper. The Windows record, from the same rig, shows none, and
what starts it here is not established. A replaced client now closes on the reaper's thread, so
neither the GUI nor a hot thread waits for its joins.

**The SDL pump's wait.** Measured on its own after the scenarios, with the waiter and the schedule
the SDL thread uses, 4,001 cycles at the 500 us period woke 65.1 us late at the median, 78.0 us at
p99 and 93.8 us at worst, and never early (61.7 to 76.1 us at the median across the run's four bench
invocations). That is the 50 us timer slack a normal Linux thread has, plus the wake-up. The
deadlines are absolute, so the lateness moves when the pump samples, not how often.

**Thread priority.** The SDL input, USB reader and receive threads ask rtkit for a nice level of -10
(a Flatpak asks the realtime portal), as AppModel wires them; the bench builds no requester, so both
records run at the default policy and neither measures the request.

### Commands (Linux, after)

Both records are runs of the measurements workflow, as in Commands (Linux) above:

```
gh workflow run measurements.yml --ref perf/hot-path-measured -f version=after-2.5.0
# the re-take: a branch holding 02184bb plus 22bfb43's change to tests/HotPathRig.h
gh workflow run measurements.yml --ref <that branch> -f version=before-2.5.0-rig
```

## Windows: before, 2.5.0 input path

dish-windows' record (its `tools/bench/README.md` at b06e6eb), taken with that repository's build of
this bench, ViGEm mode included, on the development machine. There each producer slept on a
high-resolution waitable timer to 800 us before its deadline, and the loopback satellites ran at
`THREAD_PRIORITY_TIME_CRITICAL`; the rest of the method is the one above. Its own caveats: blocking
the receiver instead of spinning it added 4 to 9 us at 1 and 2 kHz; pad A alone cost about 5 us a
report at 2 kHz, where its producer never sleeps, 15 to 17 us at 1 kHz and 35 us at 250 Hz (single
2 s runs of `--scenarios S1 --rate <hz>`), because a thread that sleeps between reports, as the SDL
thread does in its poll loop, meets cold caches, and pad B's 250 Hz frames, due with pad A's, go out
right behind them at 5 to 7 us; another agent's Gradle daemon and test runs were resident, and the
CPU load sampled before and after each run was 0 to 5 percent.

AMD Ryzen 9 7900X 12-Core Processor (24 logical CPUs), Windows 11 build 26300, Release build
(`/O2`, LTO) of dish-windows at b06e6eb, at the defaults (3 s per run, 3 reps, pad A and the
DualSense at 1000 Hz, pad B at 250 Hz, mic at 50 Hz, spinning receivers), plus `--mic-hz 1000
--scenarios S5`. Latencies are in microseconds.

| mode | scenario | producer | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | dgrams/report | drops |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | A | 1000 | 3000 | 14.9 | 26.6 | 36.9 | 49.7 | 0 | 1.134 | 1.00 | 0 |
| synthetic | S2 | A | 1000 | 3000 | 13.5 | 29.3 | 41.8 | 57.6 | 0 | 1.103 | 1.00 | 0 |
| synthetic | S2 | B | 250 | 750 | 6.3 | 12.8 | 19.6 | 19.6 | 0 | 4.098 | 1.00 | 0 |
| synthetic | S3 | A | 1000 | 3000 | 14.2 | 26.0 | 41.6 | 45.0 | 0 | 1.178 | 1.00 | 0 |
| synthetic | S3 | B | 250 | 750 | 5.4 | 12.9 | 33.8 | 33.8 | 0 | 4.169 | 1.00 | 0 |
| synthetic | S4 | A | 1000 | 3000 | 13.4 | 25.6 | 34.3 | 127.3 | 0 | 1.520 | 1.00 | 0 |
| synthetic | S4 | B | 250 | 750 | 5.9 | 12.6 | 20.5 | 20.5 | 0 | 4.068 | 1.00 | 0 |
| synthetic | S5 | A | 1000 | 3000 | 11.5 | 38.6 | 56.5 | 66.6 | 0 | 1.121 | 1.00 | 0 |
| synthetic | S5 | B | 250 | 750 | 5.9 | 13.9 | 24.1 | 24.1 | 0 | 4.064 | 1.00 | 0 |
| synthetic | S6 | DualSense | 1000 | 3000 | 13.7 | 26.9 | 43.3 | 57.0 | 0 | 1.386 | 2.22 | 0 |
| synthetic | S7 | A | 1000 | 3000 | 13.3 | 29.0 | 34.5 | 158.9 | 0 | 1.555 | 1.00 | 0 |
| synthetic | S7 | B | 250 | 750 | 6.0 | 15.9 | 32.9 | 32.9 | 0 | 4.125 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | A | 1000 | 3000 | 28.2 | 53.3 | 76.7 | 210.1 | 0 | 1.175 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | B | 250 | 750 | 6.4 | 13.5 | 111.6 | 111.6 | 0 | 4.144 | 1.00 | 0 |

Side traffic in the same runs:

| scenario | rumbles sent | light-bars sent | queued for SDL | mic sent | mic received | pings | toggles | effective Hz |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| S1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| S2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| S3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| S4 | 300 | 30 | 330 | 0 | 0 | 0 | 0 | 0.0 |
| S5 | 300 | 30 | 330 | 150 | 150 | 2 | 0 | 0.0 |
| S6 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| S7 | 300 | 30 | 330 | 0 | 0 | 0 | 15 | 0.0 |
| S5 (mic 1000 Hz) | 300 | 30 | 330 | 3000 | 3000 | 2 | 0 | 0.0 |

ViGEm mode, at its defaults, on the same machine:

| mode | scenario | producer | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | dgrams/report | drops |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vigem | V1 | visible to recv | 1000 | 1955 | 530.4 | 981.6 | 1061.2 | 1084.5 | 12 | 2.973 | 1.30 | 1044 |
| vigem | V1 | submit to visible | 1000 | 2996 | 10.3 | 36.7 | 64.2 | 87.5 | 0 | 2.973 | 1.30 | 1044 |
| vigem | V1 | submit to recv | 1000 | 1956 | 551.6 | 987.5 | 1079.0 | 1092.8 | 15 | 2.973 | 1.30 | 1044 |

The satellite saw 652.0 reports a second.

Every report pays five locks on its way out (`input.processor`, `app.routing`, `wifi.client`,
`satellite.material`, `satellite.send`) and three heap allocations (the routing's `QString`,
`sendEncrypted`'s two vectors), and `satellite.send` is held across `sendto`, for up to 176 us in
these runs. Nothing contends while one pad streams alone; the cost
appears when another thread shares a lock. The mic stream shares pad A's session: at 50 Hz pad A
waited on `satellite.send` 113 times in 3 s (up to 45 us), and at 1 kHz the SdlInput thread waited
on 2,560 of its 3,750 sends (up to 180 us), which takes pad A's median from 11.5 to 28.2 us. The receive thread takes `bridge.devices`
twice for every feedback message and meets the GUI's `devices()` reads there (waits up to 14 us in
S5), and the GUI's routing republish meets the SDL thread on `app.routing` (up to 10 us). A Direct
DualSense sends 2.22 datagrams per report: its touchpad goes out with every report, its motion at
the processor's 250 Hz cap. Through SDL, a 1 kHz pad is sampled every millisecond or so: 35 percent
of the stimulus reports were never sent, the rest arrived 530 us after XInput showed them at the
median and up to 1.1 ms after, and each sent two datagrams, one per stick axis SDL reported moved.

The lock table, from `bench_input_path_lockstats` at the same settings, for the reported rep of each
scenario (times in microseconds):

| mode | scenario | lock | role | acquisitions | contended | try fails | max wait us | p99 wait us | max hold us |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | input.processor | SdlInput | 3000 | 0 | 0 | 0.7 | 0.5 | 1.3 |
| synthetic | S1 | app.routing | SdlInput | 3000 | 0 | 0 | 0.2 | 0.3 | 12.5 |
| synthetic | S1 | satellite.material | SdlInput | 3000 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S1 | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.1 | 0.7 |
| synthetic | S1 | wifi.client | Gui | 24 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S1 | wifi.client | SdlInput | 3000 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S1 | satellite.send | SdlInput | 3000 | 0 | 0 | 0.1 | 0.1 | 65.5 |
| synthetic | S2 | input.processor | SdlInput | 3750 | 0 | 0 | 1.1 | 0.5 | 1.4 |
| synthetic | S2 | app.routing | SdlInput | 3750 | 0 | 0 | 0.3 | 0.3 | 4.4 |
| synthetic | S2 | satellite.material | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S2 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.6 |
| synthetic | S2 | wifi.client | Gui | 24 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S2 | wifi.client | SdlInput | 3750 | 0 | 0 | 14.5 | 0.1 | 0.4 |
| synthetic | S2 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 150.5 |
| synthetic | S3 | input.processor | Gui | 99 | 0 | 0 | 0.3 | 0.5 | 0.6 |
| synthetic | S3 | input.processor | SdlInput | 3750 | 1 | 0 | 1.0 | 0.5 | 2.5 |
| synthetic | S3 | bridge.devices | Gui | 30 | 0 | 0 | 0.2 | 0.3 | 2.8 |
| synthetic | S3 | app.routing | Gui | 30 | 0 | 0 | 0.2 | 0.3 | 5.0 |
| synthetic | S3 | app.routing | SdlInput | 3750 | 3 | 0 | 1.2 | 0.1 | 4.4 |
| synthetic | S3 | satellite.material | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| synthetic | S3 | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.1 | 0.7 |
| synthetic | S3 | wifi.client | Gui | 24 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S3 | wifi.client | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S3 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 58.9 |
| synthetic | S3 | store.observable | Gui | 90 | 0 | 0 | 0.2 | 0.3 | 0.8 |
| synthetic | S4 | input.processor | Gui | 99 | 0 | 0 | 0.4 | 0.5 | 0.7 |
| synthetic | S4 | input.processor | SdlInput | 3750 | 0 | 0 | 0.8 | 0.5 | 9.4 |
| synthetic | S4 | bridge.devices | Gui | 30 | 0 | 0 | 0.2 | 0.3 | 2.5 |
| synthetic | S4 | bridge.devices | Receive | 660 | 0 | 0 | 0.3 | 0.3 | 3.1 |
| synthetic | S4 | app.routing | Gui | 30 | 2 | 0 | 0.7 | 1.0 | 3.2 |
| synthetic | S4 | app.routing | SdlInput | 3750 | 3 | 0 | 2.2 | 0.3 | 5.9 |
| synthetic | S4 | bridge.output | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 42.5 |
| synthetic | S4 | satellite.material | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| synthetic | S4 | satellite.material | Receive | 660 | 0 | 0 | 0.5 | 0.5 | 0.4 |
| synthetic | S4 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.6 |
| synthetic | S4 | wifi.client | Gui | 24 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S4 | wifi.client | SdlInput | 3750 | 0 | 0 | 0.3 | 0.3 | 0.2 |
| synthetic | S4 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.1 | 0.1 | 108.1 |
| synthetic | S4 | satellite.handlers | Receive | 330 | 0 | 0 | 0.2 | 0.3 | 1.0 |
| synthetic | S4 | hub.bindings | Receive | 330 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S4 | store.observable | Gui | 90 | 0 | 0 | 0.3 | 0.5 | 0.8 |
| synthetic | S4 | store.observable | Receive | 300 | 0 | 0 | 0.2 | 0.3 | 0.3 |
| synthetic | S4 | app.audioRoutes | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S5 | input.processor | Gui | 99 | 0 | 0 | 0.3 | 0.5 | 0.6 |
| synthetic | S5 | input.processor | SdlInput | 3750 | 0 | 0 | 0.8 | 0.5 | 1.7 |
| synthetic | S5 | bridge.devices | Gui | 30 | 6 | 0 | 2.6 | 4.1 | 12.2 |
| synthetic | S5 | bridge.devices | Receive | 660 | 8 | 0 | 14.0 | 2.0 | 2.1 |
| synthetic | S5 | app.routing | Gui | 30 | 1 | 0 | 0.6 | 1.0 | 4.0 |
| synthetic | S5 | app.routing | SdlInput | 3750 | 4 | 0 | 10.2 | 0.1 | 4.1 |
| synthetic | S5 | bridge.output | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 7.1 |
| synthetic | S5 | satellite.material | SdlInput | 3750 | 4 | 0 | 1.5 | 0.1 | 0.2 |
| synthetic | S5 | satellite.material | Receive | 664 | 0 | 0 | 0.5 | 0.5 | 0.3 |
| synthetic | S5 | satellite.material | Heartbeat | 2 | 0 | 0 | 0.2 | 0.3 | 0.1 |
| synthetic | S5 | satellite.material | AudioCapture | 150 | 3 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.5 |
| synthetic | S5 | satellite.latency | Receive | 2 | 0 | 0 | 0.1 | 0.1 | 0.1 |
| synthetic | S5 | wifi.client | Gui | 24 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 | wifi.client | SdlInput | 3750 | 12 | 0 | 1.0 | 0.1 | 0.4 |
| synthetic | S5 | wifi.client | AudioCapture | 150 | 2 | 0 | 1.0 | 1.0 | 0.3 |
| synthetic | S5 | satellite.send | SdlInput | 3750 | 113 | 0 | 45.1 | 32.8 | 34.4 |
| synthetic | S5 | satellite.send | Heartbeat | 2 | 0 | 0 | 0.0 | 0.0 | 47.5 |
| synthetic | S5 | satellite.send | AudioCapture | 150 | 37 | 0 | 23.8 | 32.8 | 35.1 |
| synthetic | S5 | satellite.handlers | Receive | 332 | 0 | 0 | 0.2 | 0.1 | 0.6 |
| synthetic | S5 | hub.bindings | Receive | 330 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| synthetic | S5 | store.observable | Gui | 90 | 0 | 0 | 0.3 | 0.5 | 0.6 |
| synthetic | S5 | store.observable | Receive | 300 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S5 | app.audioRoutes | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S6 | input.processor | UsbReader | 9000 | 0 | 0 | 16.4 | 0.5 | 16.2 |
| synthetic | S6 | app.routing | UsbReader | 6655 | 0 | 0 | 7.1 | 0.1 | 15.3 |
| synthetic | S6 | satellite.material | UsbReader | 6655 | 0 | 0 | 39.6 | 0.1 | 0.2 |
| synthetic | S6 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.8 |
| synthetic | S6 | wifi.client | Gui | 24 | 0 | 0 | 0.3 | 0.5 | 0.2 |
| synthetic | S6 | wifi.client | UsbReader | 6655 | 0 | 0 | 13.8 | 0.1 | 5.1 |
| synthetic | S6 | satellite.send | UsbReader | 6655 | 0 | 0 | 5.9 | 0.1 | 176.4 |
| synthetic | S7 | input.processor | Gui | 100 | 1 | 0 | 0.7 | 0.5 | 0.7 |
| synthetic | S7 | input.processor | SdlInput | 3750 | 3 | 0 | 1.1 | 0.5 | 2.0 |
| synthetic | S7 | bridge.devices | Gui | 30 | 1 | 0 | 1.8 | 2.0 | 2.9 |
| synthetic | S7 | bridge.devices | Receive | 660 | 2 | 0 | 2.4 | 0.3 | 2.9 |
| synthetic | S7 | app.routing | Gui | 45 | 1 | 0 | 1.8 | 2.0 | 4.6 |
| synthetic | S7 | app.routing | SdlInput | 3750 | 4 | 0 | 1.9 | 0.1 | 4.2 |
| synthetic | S7 | bridge.output | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 0.3 |
| synthetic | S7 | satellite.material | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| synthetic | S7 | satellite.material | Receive | 660 | 0 | 0 | 0.4 | 0.5 | 0.2 |
| synthetic | S7 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.6 |
| synthetic | S7 | wifi.client | Gui | 24 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S7 | wifi.client | SdlInput | 3750 | 0 | 0 | 0.3 | 0.1 | 0.4 |
| synthetic | S7 | satellite.send | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 53.4 |
| synthetic | S7 | satellite.handlers | Receive | 330 | 0 | 0 | 0.2 | 0.1 | 0.6 |
| synthetic | S7 | hub.bindings | Gui | 15 | 0 | 0 | 0.1 | 0.1 | 0.6 |
| synthetic | S7 | hub.bindings | Receive | 330 | 0 | 0 | 0.2 | 0.3 | 0.3 |
| synthetic | S7 | store.observable | Gui | 158 | 0 | 0 | 0.2 | 0.3 | 0.7 |
| synthetic | S7 | store.observable | Receive | 300 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S7 | app.audioRoutes | Receive | 330 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | input.processor | Gui | 99 | 0 | 0 | 0.3 | 0.5 | 0.6 |
| synthetic | S5 (mic 1000 Hz) | input.processor | SdlInput | 3750 | 0 | 0 | 0.5 | 0.5 | 9.3 |
| synthetic | S5 (mic 1000 Hz) | bridge.devices | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 1.7 |
| synthetic | S5 (mic 1000 Hz) | bridge.devices | Receive | 660 | 0 | 0 | 0.2 | 0.1 | 2.2 |
| synthetic | S5 (mic 1000 Hz) | app.routing | Gui | 30 | 0 | 0 | 0.2 | 0.3 | 3.3 |
| synthetic | S5 (mic 1000 Hz) | app.routing | SdlInput | 3750 | 0 | 0 | 0.2 | 0.1 | 13.8 |
| synthetic | S5 (mic 1000 Hz) | bridge.output | Receive | 330 | 0 | 0 | 0.1 | 0.1 | 9.2 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | SdlInput | 3750 | 53 | 0 | 1.7 | 0.5 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | Receive | 664 | 0 | 0 | 0.4 | 0.5 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | Heartbeat | 2 | 0 | 0 | 0.1 | 0.1 | 0.1 |
| synthetic | S5 (mic 1000 Hz) | satellite.material | AudioCapture | 3000 | 43 | 0 | 0.7 | 0.3 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.1 | 0.6 |
| synthetic | S5 (mic 1000 Hz) | satellite.latency | Receive | 2 | 0 | 0 | 0.1 | 0.1 | 0.1 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | Gui | 24 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | SdlInput | 3750 | 35 | 0 | 0.9 | 0.3 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | AudioCapture | 3000 | 12 | 0 | 0.9 | 0.5 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | satellite.send | SdlInput | 3750 | 2560 | 0 | 180.4 | 32.8 | 36.8 |
| synthetic | S5 (mic 1000 Hz) | satellite.send | Heartbeat | 2 | 0 | 0 | 0.0 | 0.0 | 32.6 |
| synthetic | S5 (mic 1000 Hz) | satellite.send | AudioCapture | 3000 | 427 | 0 | 35.2 | 32.8 | 173.8 |
| synthetic | S5 (mic 1000 Hz) | satellite.handlers | Receive | 332 | 0 | 0 | 0.1 | 0.1 | 0.5 |
| synthetic | S5 (mic 1000 Hz) | hub.bindings | Receive | 330 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | store.observable | Gui | 90 | 0 | 0 | 0.3 | 0.5 | 0.6 |
| synthetic | S5 (mic 1000 Hz) | store.observable | Receive | 300 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | app.audioRoutes | Receive | 330 | 0 | 0 | 0.2 | 0.1 | 0.3 |
| vigem | V1 | input.processor | SdlInput | 3774 | 0 | 0 | 0.2 | 0.1 | 1.1 |
| vigem | V1 | bridge.devices | SdlInput | 15096 | 0 | 0 | 0.3 | 0.3 | 2.6 |
| vigem | V1 | bridge.suppressed | SdlInput | 3774 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| vigem | V1 | app.routing | SdlInput | 3774 | 0 | 0 | 0.2 | 0.1 | 2.1 |
| vigem | V1 | bridge.output | SdlInput | 7548 | 0 | 0 | 0.2 | 0.1 | 0.1 |
| vigem | V1 | satellite.material | SdlInput | 3774 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| vigem | V1 | satellite.latency | Gui | 3 | 0 | 0 | 0.2 | 0.3 | 0.8 |
| vigem | V1 | wifi.client | Gui | 6 | 0 | 0 | 0.3 | 0.5 | 0.1 |
| vigem | V1 | wifi.client | SdlInput | 3774 | 0 | 0 | 0.2 | 0.1 | 0.2 |
| vigem | V1 | satellite.send | SdlInput | 3774 | 0 | 0 | 0.1 | 0.1 | 42.0 |

### Commands (Windows)

```
cmake --preset release -B build-bench-rel -DDISH_BUILD_BENCH=ON
cmake --build build-bench-rel --target bench_input_path
cmake --preset release -B build-bench-rel-lockstats -DDISH_BUILD_BENCH=ON -DDISH_LOCK_STATS=ON
cmake --build build-bench-rel-lockstats --target bench_input_path_lockstats

# Qt's bin directory on PATH, then from each build directory:
bench_input_path --json plain.json --csv plain.csv
bench_input_path --mic-hz 1000 --scenarios S5 --json plain-mic1000.json --csv plain-mic1000.csv
bench_input_path --mode vigem --json vigem.json --csv vigem.csv
bench_input_path_lockstats --json lockstats.json --csv lockstats.csv
bench_input_path_lockstats --mic-hz 1000 --scenarios S5 --json lockstats-mic1000.json
bench_input_path_lockstats --mode vigem --json lockstats-vigem.json

# The smoke runs, from a Debug tree configured with -DDISH_BUILD_BENCH=ON (and the lockstats twin
# from one with -DDISH_LOCK_STATS=ON as well):
ctest --test-dir build -R bench_input_path
```

## Windows: on this branch, one change at a time

Each change below is measured against the commit before it: both Release builds of the bench, run
back to back on the machine above, with the CPU load sampled before, during and after each run
(the bench's own spinning threads read as 1 to 12 percent).

### The SDL thread pumps on a 500 us timer

With no SDL window, `SDL_WaitEventTimeout` is a pump and a 1 ms `Sleep`, so a pad was sampled every
1.5 to 2 ms and the output queue was drained only after an event. While a pad is open the SDL thread
now runs an explicit cycle: `SDL_PumpEvents`, every queued event through `SDL_PeepEvents`, the output
queue, and the batteries once a second; then it sleeps to the next deadline on a high-resolution
waitable timer. The deadlines are absolute, cycle n due n periods after the first, so a late cycle
runs at once, the ones it missed are skipped and the schedule does not drift
(`tests/test_pump_pacing.cpp`). With no pad open the thread blocks in `SDL_WaitEventTimeout` as it
always did, so hotplug and idle CPU are unchanged.

ViGEm mode at its defaults (`--mode vigem`), two runs of each; `visible to recv` in microseconds:

| build | p50 | p99 | p99.9 | max | over 1 ms | stimulus reports never sent | dgrams/report | effective Hz |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| before (`e6208be`) | 481.7 | 1032.7 | 1132.8 | 1202.2 | 64 | 935 | 1.38 | 688.3 |
| before (`e6208be`) | 536.8 | 1029.3 | 1099.5 | 1113.5 | 41 | 1079 | 1.28 | 640.3 |
| 500 us timer | 140.2 | 688.4 | 966.6 | 1013.1 | 1 | 4 | 2.00 | 998.7 |
| 500 us timer | 199.1 | 770.0 | 988.6 | 1030.7 | 2 | 14 | 1.99 | 995.3 |
| 500 us spin | 305.8 | 708.7 | 982.0 | 1053.0 | 1 | 9 | 1.99 | 997.0 |
| 500 us spin | 430.3 | 882.8 | 1007.7 | 1023.1 | 3 | 20 | 1.99 | 993.3 |

Nearly every stimulus report now leaves the dish, and the median wait for the next sample falls
from about half a millisecond to under a quarter. Each report still goes out twice, once per stick
axis SDL reports moved, which is why datagrams per report rose to 2; the next change batches them.

Two settings keys are an experiment, read once when Dish starts and not in the GUI (they live under
`HKEY_CURRENT_USER\Software\Dish\Dish\input` on Windows, and in the `[input]` group of
`~/.config/com.tinkernorth.Dish/dish.conf` on Linux): `pumpPeriodUs`, the period (125 to 4000,
default 500), and `pumpSpin`, which when `true` spins one core on `YieldProcessor` (x86's `pause` on
Linux) up to each deadline instead of sleeping on the timer. The bench takes the same choice as
`--pump timer|spin` and `--pump-us`.
Spinning bought nothing here: its cycles are exact, so they phase-lock with a stimulus at twice the
period and each run's median is whatever phase it started at (306 and 430 us above), while the
timer's wake-up jitter spreads the phase. The timer stays the default.

```
bench_input_path --mode vigem --json vigem.json
bench_input_path --mode vigem --pump spin --json vigem-spin.json
```

### One report, one datagram

A stick moved diagonally arrives from SDL as one controller event per axis, and the bridge rebuilt
and sent the pad's state for each. The cycle now marks a moved pad in a dirty set
(`core/input/DirtySet.h`, keyed by the pad's slot in a table only the SDL thread touches) and rebuilds
each marked pad once after the last event. `GamepadInputProcessor::publish` latches: a report equal,
after the dead zones, to the last one forwarded for the device is counted as an event and not sent;
the first report after a device appears or is removed always goes, and `zeroAndSendAll` always
sends. The raw joystick events SDL raises beside a game controller's own no longer take
`bridge.devices` or build a string to find out they are not a raw joystick's: the slot table says so.

ViGEm mode at its defaults, two runs of each; `visible to recv` in microseconds:

| build | p50 | p99 | p99.9 | max | over 1 ms | stimulus reports never sent | dgrams/report | effective Hz |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| before | 168.6 | 872.2 | 1005.6 | 1063.6 | 4 | 20 | 1.99 | 993.3 |
| before | 139.1 | 740.3 | 1026.1 | 1041.9 | 7 | 4 | 2.00 | 998.7 |
| one per report | 371.4 | 904.4 | 1029.7 | 1179.4 | 8 | 44 | 0.99 | 985.3 |
| one per report | 136.7 | 864.7 | 998.5 | 1025.6 | 1 | 17 | 0.99 | 994.3 |

The first run of the change met another process's load (68 percent at its end); the second pair
ran at 2 to 12 percent throughout.

### No lock on the input thread's own work

Three commits take every lock the input threads took for their own state. A session's key and
token sit behind a seqlock and a compare-and-swap gate covers the counter draw, the seal and a
`sendto` that no longer blocks (the socket is non-blocking; a full send buffer drops the
datagram), so `satellite.send` is no longer held across `sendto` and the mic stops queueing pad
reports behind it. The processor keeps a fixed record per device that its publishing thread writes
with no lock and the inspector copies out through a seqlock. The SDL thread owns every open pad's
record, reads pads from their events instead of 21 getters, and trades the device list and the
GUI's inputs with the GUI through single-consumer handoffs. What is left on the input threads is
the routing (`app.routing`, `wifi.client`) and the output queue (`bridge.output`), which the next
changes take.

Synthetic mode at its defaults and with `--mic-hz 1000`, before (`e6208be`, whose synthetic path
items 1 and 2 did not touch) and after, back to back at 0 to 8 percent load; microseconds:

| scenario | producer | p50 before | p50 after | p99 before | p99 after | p99.9 before | p99.9 after |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| S1 | A | 14.1 | 14.1 | 27.3 | 23.2 | 34.5 | 28.6 |
| S2 | B | 5.6 | 5.5 | 16.1 | 8.0 | 36.5 | 12.4 |
| S5 | A | 13.1 | 13.6 | 39.2 | 32.7 | 56.6 | 43.1 |
| S5 | B | 6.2 | 5.2 | 15.1 | 8.9 | 52.0 | 11.1 |
| S6 | DualSense | 13.3 | 12.3 | 28.9 | 25.1 | 49.4 | 31.1 |
| S5 (mic 1000 Hz) | A | 27.9 | 22.1 | 53.4 | 37.7 | 66.8 | 45.0 |

Pad A's median at 1 kHz is still its cold path (see the caveats): a producer that sleeps a
millisecond between reports meets cold caches whatever it locks.

## Windows: after, this branch

The same machine and settings as the Before record, Release builds, each run paired back to back
with the Before binary: before is the bench built at `e6208be` (the 2.5.0 input path with the
claim fix, whose synthetic and SDL paths are 2.5.0's), after is this branch. The CPU load sampled
before, during and after each run was 0 to 17 percent, with single samples of 28 and 31 percent
during two runs; the lead repeats both on a quiet machine before the merge. S6's finger now moves
with the stick (a resting finger is gated to nothing after its two heals, which the bench's
accounting would read as a lost touchpad); the before binary sends it every report either way.

Every report now leaves with no lock and no allocation: the bridge reads a pad from its own record
on the SDL thread, the processor writes the device's record and latches an unchanged report, the
router reads the published table inside a grace bracket, and the session seals on the stack and
sends under a compare-and-swap gate on a socket that never blocks.

Paired, microseconds:

| scenario | producer | p50 before | p50 after | p99 before | p99 after | p99.9 before | p99.9 after | dgrams/report before | dgrams/report after |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| S1 | A | 15.3 | 12.1 | 32.8 | 20.9 | 37.9 | 27.5 | 1.00 | 1.00 |
| S2 | A | 13.4 | 12.2 | 24.8 | 22.1 | 35.0 | 26.6 | 1.00 | 1.00 |
| S2 | B | 5.3 | 5.5 | 13.9 | 8.9 | 44.3 | 12.3 | 1.00 | 1.00 |
| S3 | A | 15.3 | 12.4 | 28.8 | 22.7 | 35.9 | 28.9 | 1.00 | 1.00 |
| S3 | B | 5.8 | 5.6 | 14.4 | 9.1 | 29.9 | 12.9 | 1.00 | 1.00 |
| S4 | A | 15.1 | 13.2 | 28.0 | 23.7 | 32.0 | 28.5 | 1.00 | 1.00 |
| S4 | B | 5.9 | 5.8 | 18.6 | 17.0 | 23.9 | 40.2 | 1.00 | 1.00 |
| S5 | A | 11.5 | 12.5 | 38.4 | 24.3 | 49.9 | 37.1 | 1.00 | 1.00 |
| S5 | B | 5.9 | 5.6 | 11.1 | 10.7 | 21.7 | 12.5 | 1.00 | 1.00 |
| S6 | DualSense | 15.6 | 12.1 | 31.9 | 23.8 | 36.7 | 27.7 | 2.22 | 1.34 |
| S7 | A | 13.1 | 12.1 | 25.8 | 23.3 | 37.0 | 30.9 | 1.00 | 1.00 |
| S7 | B | 5.9 | 5.5 | 14.1 | 10.4 | 30.3 | 13.9 | 1.00 | 1.00 |
| S5 (mic 1000 Hz) | A | 29.0 | 12.1 | 52.5 | 35.6 | 67.2 | 45.7 | 1.00 | 1.00 |
| S5 (mic 1000 Hz) | B | 7.3 | 5.9 | 13.4 | 9.7 | 15.3 | 13.5 | 1.00 | 1.00 |

The after runs in full, the same columns as Before:

| mode | scenario | producer | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | dgrams/report | drops |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | A | 1000 | 3000 | 12.1 | 20.9 | 27.5 | 30.5 | 0 | 1.021 | 1.00 | 0 |
| synthetic | S2 | A | 1000 | 3000 | 12.2 | 22.1 | 26.6 | 35.6 | 0 | 1.020 | 1.00 | 0 |
| synthetic | S2 | B | 250 | 750 | 5.5 | 8.9 | 12.3 | 12.3 | 0 | 4.023 | 1.00 | 0 |
| synthetic | S3 | A | 1000 | 3000 | 12.4 | 22.7 | 28.9 | 36.8 | 0 | 1.053 | 1.00 | 0 |
| synthetic | S3 | B | 250 | 750 | 5.6 | 9.1 | 12.9 | 12.9 | 0 | 4.054 | 1.00 | 0 |
| synthetic | S4 | A | 1000 | 3000 | 13.2 | 23.7 | 28.5 | 43.6 | 0 | 1.032 | 1.00 | 0 |
| synthetic | S4 | B | 250 | 750 | 5.8 | 17.0 | 40.2 | 40.2 | 0 | 4.055 | 1.00 | 0 |
| synthetic | S5 | A | 1000 | 3000 | 12.5 | 24.3 | 37.1 | 61.7 | 0 | 1.133 | 1.00 | 0 |
| synthetic | S5 | B | 250 | 750 | 5.6 | 10.7 | 12.5 | 12.5 | 0 | 4.041 | 1.00 | 0 |
| synthetic | S6 | DualSense | 1000 | 3000 | 12.1 | 23.8 | 27.7 | 34.1 | 0 | 1.029 | 1.34 | 0 |
| synthetic | S7 | A | 1000 | 3000 | 12.1 | 23.3 | 30.9 | 43.4 | 0 | 1.064 | 1.00 | 0 |
| synthetic | S7 | B | 250 | 750 | 5.5 | 10.4 | 13.9 | 13.9 | 0 | 4.028 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | A | 1000 | 3000 | 12.1 | 35.6 | 45.7 | 356.8 | 0 | 1.484 | 1.00 | 0 |
| synthetic | S5 (mic 1000 Hz) | B | 250 | 750 | 5.9 | 9.7 | 13.5 | 13.5 | 0 | 4.033 | 1.00 | 0 |
| vigem | V1 | visible to recv | 1000 | 2969 | 216.8 | 900.3 | 1006.4 | 1017.3 | 4 | 2.286 | 0.99 | 15 |
| vigem | V1 | submit to visible | 1000 | 2988 | 9.6 | 25.5 | 46.0 | 193.0 | 0 | 2.286 | 0.99 | 15 |
| vigem | V1 | submit to recv | 1000 | 2985 | 225.9 | 911.2 | 1016.4 | 1029.7 | 7 | 2.286 | 0.99 | 15 |

The side traffic is the Before record's: 300 rumbles and 30 light-bar colours delivered in S4, S5
and S7 (all 330 now reach the pad's mailbox), 150 mic frames in S5 and 3,000 with `--mic-hz 1000`,
two pings, 15 binding toggles in S7.

ViGEm mode, paired:

| build | stage | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | dgrams/report | stimulus reports never sent | effective Hz |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| before | visible to recv | 1938 | 425.7 | 1000.5 | 1052.0 | 1086.2 | 20 | 1.29 | 1058 | 647.3 |
| before | submit to visible | 2993 | 9.9 | 29.4 | 50.3 | 63.5 | 0 | 1.29 | 1058 | 647.3 |
| before | submit to recv | 1942 | 432.1 | 1009.5 | 1060.5 | 1095.2 | 23 | 1.29 | 1058 | 647.3 |
| after | visible to recv | 2969 | 216.8 | 900.3 | 1006.4 | 1017.3 | 4 | 0.99 | 15 | 995.0 |
| after | submit to visible | 2988 | 9.6 | 25.5 | 46.0 | 193.0 | 0 | 0.99 | 15 | 995.0 |
| after | submit to recv | 2985 | 225.9 | 911.2 | 1016.4 | 1029.7 | 7 | 0.99 | 15 | 995.0 |

The lock table from `bench_input_path_lockstats` at the same settings. No row belongs to the SDL
input, USB reader or receive roles: what remains is the GUI's work and the mic thread's
`wifi.client`, taken by the hub's mic sender of the time, which the next section takes off.

| mode | scenario | lock | role | acquisitions | contended | try fails | max wait us | p99 wait us | max hold us |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| synthetic | S1 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.8 |
| synthetic | S1 | wifi.client | Gui | 24 | 0 | 0 | 0.3 | 0.5 | 0.3 |
| synthetic | S2 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.8 |
| synthetic | S2 | wifi.client | Gui | 24 | 0 | 0 | 0.4 | 0.5 | 0.4 |
| synthetic | S3 | bridge.devices | Gui | 150 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S3 | usb.manager | Gui | 60 | 0 | 0 | 0.2 | 0.3 | 0.6 |
| synthetic | S3 | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 1.0 |
| synthetic | S3 | wifi.client | Gui | 114 | 0 | 0 | 0.4 | 0.5 | 0.3 |
| synthetic | S3 | hub.bindings | Gui | 30 | 0 | 0 | 0.5 | 0.5 | 0.3 |
| synthetic | S3 | store.observable | Gui | 180 | 0 | 0 | 0.2 | 0.1 | 0.7 |
| synthetic | S3 | app.audioRoutes | Gui | 90 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S3 | app.routes | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 16.7 |
| synthetic | S4 | bridge.devices | Gui | 150 | 0 | 0 | 0.1 | 0.1 | 0.2 |
| synthetic | S4 | usb.manager | Gui | 60 | 0 | 0 | 0.2 | 0.3 | 0.7 |
| synthetic | S4 | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.1 | 0.8 |
| synthetic | S4 | wifi.client | Gui | 114 | 0 | 0 | 0.4 | 0.5 | 0.4 |
| synthetic | S4 | hub.bindings | Gui | 30 | 0 | 0 | 0.4 | 0.5 | 0.3 |
| synthetic | S4 | store.observable | Gui | 180 | 0 | 0 | 0.1 | 0.1 | 0.8 |
| synthetic | S4 | app.audioRoutes | Gui | 90 | 0 | 0 | 0.2 | 0.3 | 0.3 |
| synthetic | S4 | app.routes | Gui | 30 | 0 | 0 | 0.1 | 0.1 | 11.2 |
| synthetic | S5 | bridge.devices | Gui | 150 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 | usb.manager | Gui | 60 | 0 | 0 | 0.2 | 0.3 | 0.9 |
| synthetic | S5 | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.1 | 0.8 |
| synthetic | S5 | wifi.client | Gui | 114 | 0 | 0 | 0.2 | 0.3 | 0.3 |
| synthetic | S5 | wifi.client | AudioCapture | 150 | 0 | 0 | 0.6 | 1.0 | 0.3 |
| synthetic | S5 | hub.bindings | Gui | 30 | 0 | 0 | 0.4 | 0.5 | 0.3 |
| synthetic | S5 | store.observable | Gui | 180 | 0 | 0 | 0.2 | 0.3 | 0.8 |
| synthetic | S5 | app.audioRoutes | Gui | 90 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 | app.routes | Gui | 30 | 0 | 0 | 0.2 | 0.3 | 12.2 |
| synthetic | S6 | satellite.latency | Gui | 16 | 0 | 0 | 0.2 | 0.3 | 1.0 |
| synthetic | S6 | wifi.client | Gui | 32 | 0 | 0 | 0.3 | 0.5 | 0.3 |
| synthetic | S7 | bridge.devices | Gui | 256 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S7 | usb.manager | Gui | 90 | 0 | 0 | 0.2 | 0.3 | 0.6 |
| synthetic | S7 | satellite.latency | Gui | 12 | 0 | 0 | 0.1 | 0.1 | 1.1 |
| synthetic | S7 | wifi.client | Gui | 174 | 0 | 0 | 0.3 | 0.5 | 0.3 |
| synthetic | S7 | hub.bindings | Gui | 60 | 0 | 0 | 0.5 | 0.5 | 2.5 |
| synthetic | S7 | store.observable | Gui | 308 | 0 | 0 | 0.2 | 0.3 | 1.0 |
| synthetic | S7 | app.audioRoutes | Gui | 158 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S7 | app.routes | Gui | 45 | 0 | 0 | 0.2 | 0.3 | 17.2 |
| synthetic | S5 (mic 1000 Hz) | bridge.devices | Gui | 150 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | usb.manager | Gui | 60 | 0 | 0 | 0.2 | 0.3 | 0.5 |
| synthetic | S5 (mic 1000 Hz) | satellite.latency | Gui | 12 | 0 | 0 | 0.2 | 0.3 | 0.7 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | Gui | 114 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | wifi.client | AudioCapture | 3000 | 0 | 0 | 3.3 | 0.5 | 1.8 |
| synthetic | S5 (mic 1000 Hz) | hub.bindings | Gui | 30 | 0 | 0 | 0.3 | 0.5 | 0.3 |
| synthetic | S5 (mic 1000 Hz) | store.observable | Gui | 180 | 0 | 0 | 0.2 | 0.3 | 0.7 |
| synthetic | S5 (mic 1000 Hz) | app.audioRoutes | Gui | 90 | 0 | 0 | 0.2 | 0.3 | 0.2 |
| synthetic | S5 (mic 1000 Hz) | app.routes | Gui | 30 | 0 | 0 | 0.2 | 0.3 | 12.9 |
| vigem | V1 | satellite.latency | Gui | 3 | 0 | 0 | 0.1 | 0.1 | 0.9 |
| vigem | V1 | wifi.client | Gui | 6 | 0 | 0 | 0.3 | 0.5 | 0.3 |

### Thread priority and the timer

The SDL input, USB reader and receive threads now ask MMCSS for "Games" (falling back to
`THREAD_PRIORITY_TIME_CRITICAL`), and `main` asks for the 1 ms timer itself. ViGEm mode measures the
SDL thread's share, built with and without the request on the SDL and receive threads, two runs
each:

| priority | p50 us | p99 us | p99.9 us | max us | over 1 ms | stimulus reports never sent | effective Hz |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| normal | 228.6 | 958.2 | 1021.8 | 1034.9 | 10 | 123 | 959.0 |
| normal | 239.7 | 885.4 | 997.9 | 1117.6 | 2 | 2 | 999.3 |
| raised | 167.1 | 773.8 | 998.8 | 1056.8 | 2 | 20 | 993.3 |
| raised | 167.2 | 826.2 | 982.4 | 1031.7 | 2 | 16 | 994.7 |

### The mic sender reads the published table

The hub's mic sender read the session's slot map, which the GUI rewrites, with nothing between them,
then took `wifi.client` for the client. A mic frame now leaves through `SlotRouter`'s table in a
grace bracket, like a report. The lockstats twin's S5 runs, at 50 Hz and at 1 kHz, record the eight
GUI locks of the table above and no AudioCapture row, and law 6 of the lock-law test holds the mic
thread to no lock.

Pad A, paired with the bench built at `f0a2bc4` (the record above), two runs of each, on the same
machine with a `satellite` process running beside it, CPU load 0 to 16 percent:

| scenario | build | p50 us | p99 us | p99.9 us | max us |
| --- | --- | ---: | ---: | ---: | ---: |
| S5 | before | 15.7 | 21.7 | 35.4 | 51.5 |
| S5 | before | 13.4 | 22.6 | 35.4 | 59.2 |
| S5 | after | 15.3 | 30.7 | 49.0 | 129.2 |
| S5 | after | 12.6 | 29.5 | 40.1 | 45.0 |
| S5 (mic 1000 Hz) | before | 14.0 | 40.6 | 46.8 | 169.9 |
| S5 (mic 1000 Hz) | before | 13.4 | 40.1 | 46.8 | 53.7 |
| S5 (mic 1000 Hz) | after | 18.4 | 46.4 | 50.3 | 214.3 |
| S5 (mic 1000 Hz) | after | 23.2 | 42.8 | 56.5 | 233.9 |
| S5 (mic 997 Hz) | before | 12.1 | 25.7 | 34.6 | 97.0 |
| S5 (mic 997 Hz) | before | 12.8 | 24.3 | 34.9 | 42.8 |
| S5 (mic 997 Hz) | after | 13.1 | 25.3 | 35.4 | 47.5 |
| S5 (mic 997 Hz) | after | 13.0 | 22.3 | 32.3 | 51.0 |

Pad A's reports and the mic frames share the session's send gate, and the bench paces both from one
start: at 50 Hz every mic frame is due with one of pad A's reports, and at 1 kHz every one of them
is. The hub's sender reached the gate after the report and mostly lost; the table's sender gets there
as fast and wins about as often as not, so those reports wait out one mic send, 5 to 10 us. That is
pad A's p99 at 50 Hz (one report in twenty) and its median at 1 kHz. At 997 Hz, where the two
drift past each other, after matches before. A capture thread is clocked by its audio device and not
by the pump, so the app meets the 997 Hz case.

### Commands

```
cmake --build build-bench-rel --target bench_input_path
cmake --build build-bench-rel-lockstats --target bench_input_path_lockstats

bench_input_path --json plain.json --csv plain.csv
bench_input_path --mic-hz 1000 --scenarios S5 --json plain-mic1000.json
bench_input_path --mode vigem --json vigem.json
bench_input_path_lockstats --json lockstats.json
bench_input_path_lockstats --mic-hz 1000 --scenarios S5 --json lockstats-mic1000.json
bench_input_path_lockstats --mode vigem --json lockstats-vigem.json
```
