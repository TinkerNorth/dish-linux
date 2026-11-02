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

| # | Stage | What it is | Locks it takes today |
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
harness cannot instantiate it. Its glue is mirrored verbatim in the rig's `AppGlue`: the four
processor senders (the report sender is `src/AppModel.cpp:269-282`, with its fall-through to the
Moonlight tables, to which the rig binds no slot), `republishRouting`, and the feedback route from
`onRumbleMessage` and `onLightbarMessage` through `feedbackInputs` to the bridge or the USB manager.
A change to that glue in AppModel must be made to `AppGlue` as well.

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
| S2 | S1 plus pad B at 250 Hz, published by the same SdlInput thread, as the one SDL thread serves every pad. A dish binds one pad per satellite session (`ConnectionHub::bind` evicts the slot already bound to the connection, and `WifiConnection::sendReport` sends only for its bound slot), so pad B streams to a second session |
| S3 | S2 plus the GUI while a pad plays: routing republish every 100 ms, `devices()` every 100 ms, `inspect()` at 30 Hz, `inputCounters()` at 1 Hz |
| S4 | S3 plus 100 rumbles and 10 light-bar colours a second from pad A's satellite to the receive thread, which routes them to the SDL output queue ("queued for SDL") |
| S5 | S4 plus the session's heartbeat thread (a ping every 2 s, acked) and an AudioCapture thread sending mic frames at `--mic-hz` (50) through the hub's mic sender on pad A's session; `S5 (mic 1000 Hz)` is the `--mic-hz 1000` run |
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
  the battery poll are not in any Linux figure.
- **AppModel's glue is a mirror** (see above), and **a Direct pad's gateway is a fake**: the hidraw
  read and write and their locks (`hid.claims`, `hid.write`) need hardware and are not exercised.
  `hid.write` is held across a `write` to the hidraw node that has no timeout.
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
re-establishing a session, and held one session's `wifi.client` for 285 and 323 ms. The loopback
satellite answers the address of the first datagram it received, so a session that gets a new client
stops hearing its acks and falters after two missed pings; each discovery scan ends in
`autoReconnectAll`, whose `connectTo` takes a Faltering session straight to Linking without tearing
its client down; `markConnected` then hands the new client to `ClientRef::set`, which lets go of the
old one under `wifi.client`, and when that is its last reference the destructor joins the heartbeat
thread (up to 100 ms) and the receive thread (up to 500 ms in `recvfrom`) with the lock held. The
SDL thread did not wait on it in these runs, so the lock was another session's, but a report on a
Faltering session would wait out both joins. What made the first session falter in these runs is
not established.

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
