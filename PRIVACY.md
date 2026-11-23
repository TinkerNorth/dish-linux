# Dish for Linux: Privacy Policy

**Effective date:** 2026-11-22.
**Canonical copy:** this file, at
[`https://github.com/TinkerNorth/dish-linux/blob/main/PRIVACY.md`](https://github.com/TinkerNorth/dish-linux/blob/main/PRIVACY.md).
The app links to it from Help, and the privacy index on
[dish.tinkernorth.com](https://dish.tinkernorth.com/privacy/) points here, so
the code and the policy ship together.

This document describes what data the Dish Linux client collects, why, how
long it is retained, and the choices you have over it. The product as a whole
spans several repositories (`satellite`, `dish-android`, `dish-windows`,
`dish-linux`, `dish-mac`); this policy is specific to the Linux desktop
client. The server (`satellite`) runs on your own machine and has its own
policy. The Android client has a separate policy and different behaviour, so
do not read one as describing the other.

---

## 1. Short version

- Dish for Linux turns a Linux machine into a wireless gamepad for a
  `satellite` server, or for a Moonlight host (Sunshine, Apollo, Wolf).
  Controller input goes from your machine, over your own network, to the
  host you paired with. It does not stream to any TinkerNorth-operated
  server. TinkerNorth does not operate a server for Dish at all.
- **Three things can leave your machine on their own**, and you can turn
  each one off: an update check against GitHub, on by default except in the
  Flatpak, where it starts off, described in section 2.4; a
  check for a newer list of the controllers Dish recognises, against
  `dish.tinkernorth.com`, described in section 2.5; and, when the app
  crashes, one crash report to Sentry, described in section 3. There is no
  analytics SDK, no telemetry, no advertising identifier, no usage reporting
  and no account. Both checks ask for files and send no identifier with the
  request.
- **Report a problem** (section 2.6) happens only when you click *Report a
  problem* or *Ask a question*: Dish saves a file and opens your own mail app
  with it attached, addressed to `support@tinkernorth.com`. Nothing is sent
  until you click Send in your mail app.
- Crash diagnostics are always written to your own disk, at
  `$XDG_STATE_HOME/dish/crash.log` (`~/.local/state/dish/crash.log` by
  default). With *Share crash reports* on (the default), an official build
  also sends one report per crash to Sentry: a stack trace and the app and
  system versions. Never your controller input, pairings or keys. Section 3
  has the full list.
- Settings, remembered servers, and pairing keys live in your own home
  directory under `~/.config/`. Nothing is synced to a cloud account by the
  app.
- We do not sell, share, or rent your data. We do not show ads. We do not
  profile you.

---

## 2. What data is processed

### 2.1 Stays on your PC

Persisted with `QSettings`, which on Linux writes INI files under
`$XDG_CONFIG_HOME` (`~/.config` by default). The cross-client settings schema
lands in `~/.config/Dish/Dish.conf`. Preferences that only exist on the desktop
clients are written through the app's default organisation name and land in
`~/.config/com.tinkernorth.Dish/dish.conf` instead: the navigation preference
`ui_rail_collapsed` and the update settings listed after the table.

| Setting | Holds | Used for |
|---|---|---|
| `satellite_list` | JSON array of remembered satellites: display name, IP, UDP port, HTTPS port, and the server's machine id | Reconnecting to hosts you already paired with |
| `satellite_shared_key:<id>` | **The libsodium-derived pairing key for that satellite**, hex encoded | Deriving the per-session ChaCha20-Poly1305 key for the gamepad wire protocol. This is secret material. Anyone who can read your config directory can read it. |
| `satellite_cert_pin:<ip>` | SHA-256 fingerprint of the satellite's self-signed TLS certificate | Trust-on-first-use pinning, so a later HTTPS call is talking to the same box |
| `moonlight_host_list` | JSON array of remembered Moonlight hosts: display name, IP, HTTP and HTTPS ports, the host's id, its pinned certificate, the app last launched there, the controller type you chose, and whether pairing completed | Reconnecting to Moonlight (Sunshine, Apollo, Wolf) hosts you paired with, and checking that a later call is talking to the same host |
| `moonlight_identity_cert`, `moonlight_identity_key`, `moonlight_identity_uniqueid` | **This install's Moonlight client identity: a self-signed certificate, its private key, and a random client id** | Proving to a Moonlight host that this is the machine it paired with. Secret material, like the pairing key above. Sent **only** to Moonlight hosts you pair with, never to us. |
| `deviceId` | A random UUID generated on first run, with the dashes stripped | A stable per-install identifier the satellite uses to recognise this client across restarts and IP changes. It is sent **only** to satellites you pair with, never to us. |
| `deadzone:<deviceId>` | Per-controller stick and trigger deadzone profile | Restoring your calibration |
| `motion_preferences`, `touchpad_mode_preferences` | Per-slot motion and touchpad routing toggles | Restoring your setup |
| `usb_path_choices` | Per `vid:pid` choice between the SDL path and the USB-direct raw-HID path | Restoring your setup |
| `joystick_remaps` | Per-device button, stick, and trigger remapping | Restoring your setup |
| `feature_lightbar_mode` | `followGame` or `off` | Light-bar behaviour |
| `theme_mode` | `system`, `light`, or `dark` | Appearance |
| `onboarding_welcome_completed`, `onboarding_dashboard_hint_dismissed` | Booleans | Not showing first-run screens again |
| `crashlytics_collection_enabled` | Boolean, default `true` | The *Share crash reports* switch; section 3. The key name is inherited from the Android client for schema continuity; this client reports to Sentry, not Crashlytics. |
| `ui_rail_collapsed` | Boolean | Navigation-rail width |

Legacy values `wifi_list` and `wifi_shared_key/<id>` from older builds are
migrated in place on first run so you do not have to re-pair.

These three values live in `~/.config/com.tinkernorth.Dish/dish.conf` and cover the
update check described in section 2.4. They record your choice and the app's
own bookkeeping. None of them is transmitted anywhere.

| Setting | Holds | Used for |
|---|---|---|
| `updates_check_enabled` | Boolean, default `true`, `false` in the Flatpak | The *Check for updates automatically* switch. When off, the app makes no update-related network request at all. |
| `updates_skipped_version` | A version string, default empty | The one version you pressed *Skip this version* on, so it stops being offered |
| `updates_last_check_utc_ms` | Timestamp | Not checking more than once an hour at startup |

There is no downloaded-update folder, because Dish for Linux never downloads or
installs an update; see section 2.4.

Five more values in the same file cover the controller database check
described in section 2.5. None of them is transmitted anywhere either; the
ETag is the value the server itself handed back, and goes back to it only as
the `If-None-Match` of the next check.

| Setting | Holds | Used for |
|---|---|---|
| `controller_db_check_enabled` | Boolean, default `true` | The *Check for controller updates automatically* switch. When off, the app makes no request for the controller list at all. |
| `controller_db_last_check_utc_ms`, `controller_db_last_check_result` | Timestamp and a word (`upToDate`, `updated`, `offline`, `invalid`, `off`) | Showing on the Diagnostics page when the last check ran and how it ended |
| `controller_db_manifest_etag`, `controller_db_manifest_etag_version` | The server's ETag for the manifest, and the list version it belongs to | Asking "anything newer?" without downloading the manifest again |

A newer controller list, when one has been downloaded, is kept under
`~/.local/share/TinkerNorth/Dish/controllers/v1/` (in the Flatpak, under
`~/.var/app/com.tinkernorth.Dish/data/`): the list, its strings in six
languages and the manifest they came with. A diagnostics file you save or
send is kept under `~/.local/share/TinkerNorth/Dish/diagnostics/` until you
delete it (section 2.6).

The app also **reads** (never writes) the XDG desktop portals for your
light/dark preference and your reduced-motion preference, and reads
`/sys/class/bluetooth` and BlueZ over the system bus to tell "no Bluetooth
adapter" from "adapter switched off".

Held in memory only, never written and never transmitted anywhere except to
the satellite you bound the controller to:

- Gamepad input events: buttons, sticks, triggers, motion, touchpad.
- Battery readings reported by a connected controller, and this computer's own
  battery level and charging state, read from `/sys/class/power_supply` (a
  system battery only, never a peripheral's), so that a controller reports the
  lower of its battery and the computer's, or the computer's when the
  controller has none. A computer without a battery adds nothing. The
  computer's level and charging state also go into a support report you send
  (section 2.6).
- Raw HID reports read from a controller in USB-direct mode.
- Your answers to *Did it work?* after each test in the controller inspector,
  the last twenty per controller, kept until the app quits and written out
  only into a diagnostics file you save or send (section 2.6).
- What a controller's own microphone hears, only while it is open as described
  below. It is encoded as it arrives and never recorded or kept.

The app reads input from connected game controllers, through SDL2 and, in
USB-direct mode, by reading raw HID input reports itself. It also writes back to
them what the game on your PC sends: rumble, the light-bar colour, the player
lights, adaptive-trigger effects and the mute lamp, through SDL2 or, in
USB-direct mode, as HID output reports it writes itself. All of this is device
IO on your own machine. It requires no administrator rights and no driver
install.

**A controller's own speaker and microphone.** A DualSense or DualShock 4 on a
USB cable has an audio device of its own. While that controller's *Speaker*
switch is on, which is the default, the sound your PC sends for it, and on a
DualSense the haptics, play on that device. Apart from the inspector's test below, its microphone
is opened only while all of these hold: that controller's *Microphone* switch
is on, which it is not until you turn it on; the controller is streaming to a
Satellite that carries the microphone; and it is not muted. What it hears is
then sent to that Satellite, as section 2.2 describes. Turning the switch off,
muting it or ending the stream closes the microphone. The app never opens the
computer's own microphone, or any microphone but a controller's own.

**The controller inspector's tests.** From the Diagnostics page, a
controller's inspector can test each thing its connection carries: rumble and
trigger rumble run for a moment; the light bar, the player lights, the
adaptive triggers and the mute lamp change for a moment and then go back to
what the game had set; the test tone plays on the controller's own speaker, or
on its haptics; and the microphone test opens the controller's own microphone
for up to eight seconds to show how loud it hears you. What the microphone
hears stays in the app and goes no further: it is not recorded, kept or sent,
and only its level is shown. Your answer to *Did it work?* is kept in memory
for this run, and leaves your machine only inside a diagnostics file you save
or send (section 2.6).

### 2.2 Sent to your own LAN, not to TinkerNorth

Everything the app puts on the network goes to a `satellite` server you chose,
on an address you can see in the app.

- **Discovery.** When you scan, the app sends a multicast DNS PTR query for
  `_satellite._udp.local.` to `224.0.0.251:5353`, and listens for the legacy
  UDP broadcast beacon on port 9879. The mDNS query contains the service name
  it is asking about and nothing about you. Both stay on your local network.
- **Pairing.** When you pair, the app makes an HTTPS `POST` to
  `/api/pair` on the satellite (port 9443 by default). The satellite presents
  a self-signed certificate, which the app pins on first use and checks on
  every later call. The request carries your device id, a device label, and
  either the PIN shown on the satellite or a PIN the app displays for you to
  type there. The response carries the shared pairing key, which is then
  stored as described above. `DELETE /api/pair` unpairs, which also removes
  the server-side record.
- **Control plane.** Once paired, the app calls the satellite's HTTPS API on
  the same port: `PUT`/`DELETE /api/connections` and
  `/api/connections/<id>/controllers/<slot>` to declare which controllers are
  bound and how they should be emulated, `GET /api/server/capabilities`, and
  `GET /api/catalog` for the localised list of emulatable controller types.
  These carry your device id, an HMAC proof computed from the pairing key, and
  your controller topology.
- **Gamepad stream.** The data plane is raw UDP to the satellite, port 9876
  by default, encrypted and authenticated with ChaCha20-Poly1305 under a
  session key derived from the pairing key (the pairing key itself never
  travels over UDP). Upstream frames carry controller state, motion, battery,
  and touchpad and, while a controller's microphone is open as section 2.1
  describes, what it hears, as 20 ms Opus frames. Downstream frames carry
  rumble, light-bar colour, the player lights, adaptive-trigger effects, the
  mute lamp, the sound and haptics for a controller's own speaker, heartbeat
  acknowledgements, and session close.
- **Moonlight hosts.** Dish can also be the controller for a Sunshine, Apollo
  or Wolf host, speaking the Moonlight (GameStream) protocol instead of the
  satellite one. Hosts are found over mDNS (`_nvstream._tcp`) or added by
  address. Pairing follows that protocol: a few HTTP requests to the host on
  port 47989, which the protocol fixes as plain HTTP because no shared secret
  exists yet. The PIN never travels over the wire; both sides derive a key
  from it and prove they hold it. From then on every call is mutual TLS on
  port 47984 with this install's client certificate, and the host's
  certificate is pinned so a swapped host is refused. To open a controller
  session the app launches or resumes an app on the host, then sends your
  controller input, motion, touchpad and battery over the protocol's
  encrypted (AES-GCM) control channel. The host also streams its screen and
  audio at the lowest settings it allows, because the protocol needs a stream
  to hold the session open; Dish discards those packets without decoding
  them and never stores or shows them. The host learns this install's client
  id, a device name and the client certificate. All of this stays on your
  local network.

A host firewall may need to allow the app to send and receive on your local
network. That affects your LAN only.

### 2.3 Sent to TinkerNorth or a third party

There is no TinkerNorth server for this app to talk to. The one thing that
reaches us is a crash report, through Sentry, and only if you leave *Share
crash reports* on.

Three things leave your network by default, two in the Flatpak, where the
update check starts off; you can turn each one off.
The update check described in **section 2.4** goes to GitHub: a plain HTTPS
request for a file on the public releases page, with no identifier. The
controller database check described in **section 2.5** goes to
`dish.tinkernorth.com`, the same way. A crash report, described in
**section 3**, goes to Sentry, which processes it for us, and only when the
app has actually crashed. A fourth, the support report in **section 2.6**,
never leaves on its own: you send it from your own mail app.

Beyond those there is no analytics library, no error tracking beyond the
crash report, and no advertising identifier. Apart from the updater, the
controller database check and the crash reporter, the only component in the
app that makes an outbound HTTPS request is the satellite API client, and it
only ever addresses the IP of a satellite you selected.

The other way this app causes a request to a TinkerNorth or third-party
address is if you click a link, which hands the URL to your default browser
and is then between you and that site. Those links are:

| Where | Opens |
|---|---|
| Help, privacy policy | `https://github.com/TinkerNorth/dish-linux/blob/main/PRIVACY.md` |
| Help, project page | `https://github.com/TinkerNorth` |
| Welcome, Connections, and the setup wizard, when you have no server yet | `https://dish.tinkernorth.com/downloads/satellite` |
| Support links | `https://github.com/sponsors/TinkerNorth`, `https://ko-fi.com/tinkernorth`, `https://buymeacoffee.com/tinkernorth` |

The app does not embed a browser and does not pass any identifier along with
these URLs.

### 2.4 Update check (GitHub)

When *Check for updates automatically* is on, which is the default everywhere
but the Flatpak, the app sends an HTTPS `GET` to `github.com` for the file
`latest.json` attached to the newest release of this project. GitHub answers with a redirect to its own
download CDN, so the request finishes against
`objects.githubusercontent.com`. The file is about a kilobyte of JSON: a version number, a minimum supported
version, and a link to the release notes.

**When it happens.** About 15 seconds after you launch the app, at most once
an hour; every four hours while the app stays open; and whenever you press
*Check for updates* in Settings. Nothing runs while the app is closed. There is
no service, no scheduled task, and no update agent.

**What the request carries.** Your IP address, as GitHub sees it, which is
unavoidable for any HTTPS request. A `User-Agent` header of the form
`Dish/<version> (Linux; x86_64)`, `Accept: application/json`, and
`Accept-Language: *`, which is the same for everyone and never your system's
languages. Beside those, only the headers Qt puts on every request: the host
name, the compressions it can read (`Accept-Encoding`), `Cache-Control` and
`Pragma` asking for a fresh copy, and `Connection`. No device id, no account
data, no settings, no usage data, no cookies, no query parameters, and no
conditional-request identifiers. GitHub's own
[privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement)
governs what GitHub logs about the request.

**Nothing is downloaded.** Dish for Linux never downloads or installs its own
update: your package manager owns the binary. If the release is newer, the app
shows a pill linking to the release page and stops. That one `GET` for
`latest.json` is the entire network footprint of the feature.

**Turning it off.** *Check for updates automatically* is the master switch. Off
means the app makes no update-related network request of any kind, arms no
timer, and creates no network stack for it. In the Flatpak the switch starts
off, because Flathub delivers the app's updates itself; turned on there, it
works as described above.

The settings behind that switch are in section 2.1.

### 2.5 Controller database updates

Dish ships with a list of the controllers it recognises: their USB and
Bluetooth ids, how each one talks, and the known issues its Diagnostics card
shows. So that a newly supported controller does not wait for an app release,
the app checks for a newer list. About ten seconds after launch, and then every
six hours while it runs, it fetches a small manifest from
`https://dish.tinkernorth.com/controllers/v1/` that names the newest version.
Only when that version is newer than the one the app has does it fetch the
rest: the controller list and its text in each language. These files are data,
never code. The app checks each one's size and SHA-256 checksum against the
manifest before using it, and keeps the copy it has when anything does not
match. A newer list is stored in the folder named in section 2.1 and applies
to a controller the next time it is plugged in.

`dish.tinkernorth.com` is an Amazon S3 bucket behind Amazon CloudFront,
operated for TinkerNorth by Amazon Web Services. The requests carry no
identifier, no cookie and no query. Each one carries exactly these headers:
the host name; Qt's default `User-Agent`, `Mozilla/5.0`, not the update check's
`Dish/<version>` one; `Accept-Language: *`, the same for everyone and never
your system's languages; the compressions Qt can read (`Accept-Encoding`);
`Cache-Control` and `Pragma` asking for a fresh copy; `Connection`; and, on the
manifest only, `If-None-Match`, carrying the ETag the server gave the last
time, so an unchanged manifest is not sent again. What the host's access logs
can see is what any web server sees: the IP address the request came from, and
the time.
We use those logs as we use the rest of the website's: for capacity planning
and abuse detection only.

*Check for controller updates automatically* in Settings, on by default, turns
the check off. With it off, no request for the controller list leaves your
machine, and the app keeps using the copy it has. *Check now* runs one check
when you click it, and is disabled while the switch is off. Every build makes
this check, the Flatpak from Flathub included, where the update check in
section 2.4 starts off: Flathub delivers the app, not the controller list. It
is separate from the update check, and turning one off leaves the other alone.

### 2.6 Report a problem

The Diagnostics page (Settings, then *Diagnostics…*) has *Report a problem*,
*Ask a question* and *Save diagnostics*. The first two open the same page in
a different category: you pick what it is about (a controller not
recognised, no rumble or feedback, connection or latency, audio, something
else, or a question), write what happened and what you expected, and may name
the controller it concerns. The page lists in plain words what the file will
hold, *Preview file* shows you the file itself, and *Continue in your mail
app* writes `dish-diagnostics-dish-linux-<date>-<time>.json` under
`~/.local/share/TinkerNorth/Dish/diagnostics/` (in the Flatpak, under the
app's own `~/.var/app/com.tinkernorth.Dish/data/`) and runs your desktop's
`xdg-email` to open your own mail app with a new message to
`support@tinkernorth.com`. The subject is `Dish support: <the category, in
your language> · Dish for Linux <version> on <PC model>`; the body is your
own words first, then a summary of the Diagnostics page in your language; the
file is attached. When `xdg-email` is not installed or no mail app takes the
message, a `mailto:` link opens with the same subject and body and your file
manager shows the folder for you to attach the file. *Save diagnostics*
writes the same file without a report and opens that folder in your file
manager. Nothing is sent by the app: the mail leaves only when you click Send
in your mail app, and you can read or edit it before you do. If you send it,
TinkerNorth receives it at that address and uses it only to answer you and
fix what it shows.

**Your own words.** What you write and the category you pick go into the
mail and into the file as `report`, with the database record or product name
of the controller you named. Dish does not read, check or keep them anywhere
else; they exist only in the file and in the mail you send.

**The Satellite connection snapshot.** When the page opens, Dish asks each
Satellite it has a live session with, version 2.3.0 or later, for that
session's own account of itself: one request to
`GET /api/connections/<id>/diagnostics` on the PC you paired with, over the
same encrypted, pinned connection and pairing proof every session request
uses, given up after two seconds. Nothing goes anywhere else, and nothing is
asked of a Satellite older than 2.3.0, which has no such route; for that one,
and for any that does not answer, the file holds what Dish already had from
it (its version, protocol, backends and audio switches) and says why. The
snapshot holds that Satellite's version, platform and uptime, its virtual
controller backends and their driver versions, each virtual pad of this
session (the type asked for and the one made, its features, its touchpad mode,
and how many reports went in and out of it), the audio switches it grants, its
message counters, the state of its own controller list, and this connection's
lines of its log. Satellite removes from it every machine name, every other
client's name and address and every secret, and names your device "this
connection"; Dish then replaces that host's name and address with its alias
wherever either still appears. A Moonlight host (Sunshine, Apollo, Wolf) is
not asked anything: the file holds the version its last answer to Dish gave.

**What the file holds.** Your report, as above; the app's version and how it
was installed (Flatpak, AppImage or a package); this PC's model as its
firmware names it (`/sys/class/dmi/id/sys_vendor` and `product_name`, "Linux
PC" where the firmware names none; never the machine's host name), the
distribution, the kernel version and the processor architecture; its battery
level and whether it is charging (`/sys/class/power_supply`, a system battery
only), whether the platform's power profile is the low-power one
(`/sys/firmware/acpi/platform_profile`), and how hot it runs against its own
thermal trip points (`/sys/class/thermal`: nominal, fair, serious or
critical); the controller database's bundled and active versions and the time
and result of its last check; for each controller, the database record it
matched, that record's verification state, its transport (USB or Bluetooth)
and path (Direct or SDL), whether a Bluetooth link is classic or low energy
(see below), its USB vendor and product ids, its release number
(`bcdDevice`), the product and manufacturer strings it reports, its USB
interfaces (number, class, subclass, protocol) with each one's endpoints
(address, type, packet size, interval), the addresses of the endpoints Dish
reads and writes, a SHA-256 checksum of its USB descriptors, any
mismatch between the record and what Dish saw, how many input reports it sent
and at what rate, the last one (the buttons, sticks and triggers at that moment), for each kind of
output (rumble, light bar, player lights, trigger effects, mute lamp) how
many a host sent it, where they went and the last value with its time (an
inspector test's own output is not counted), whether its speaker and haptics
streams are open, the last report Dish wrote to it (the report id, whether it
went through and the kernel's error name when it did not), and your answers
to *Did it work?* after each test in the controller inspector, with the
test's name and when it ended; for each
paired Satellite or Moonlight host, an alias ("Host 1", "Host 2"), its kind
and version, the protocol version the session settled on, its link state and
which controller-audio directions it grants, Dish's own view of the session
(each controller slot, the last heartbeat's facts, round trip times, and how
many messages of each kind went each way) and the snapshot above; and this
run's event log of hosts and controllers appearing, changing state, binding
and leaving, with every host named only by its alias.

**Whether this PC has a USB host controller.** The file says whether Linux
lists at least one USB root hub under `/sys/bus/usb/devices` (one per host
controller), as `true` or `false`, and nothing about the devices on it.

**A controller you capture.** For a controller Dish reads directly, the report
page offers *Capture this controller*. Dish then keeps the next twenty input
reports the controller sends, with the time each arrived, while you leave it
untouched, and reads its USB descriptors from sysfs (`descriptors`, the raw
device and configuration descriptors), its manufacturer and product strings
and its HID report descriptor. Nothing is read from the controller's serial
number. The capture stays in memory for this run and goes into the file only
when your report names that controller; it never includes anything you press
afterwards.

**The Bluetooth link, a question on this PC.** For a Bluetooth controller the
kernel's own Bluetooth stack made, the link is classic, from sysfs alone. For
one that BlueZ hands to the kernel itself (every low energy pad, and from
BlueZ 5.73 classic ones too), Dish reads the controller's Bluetooth address
from sysfs and asks BlueZ on this PC, over the system D-Bus, which HID
service the paired device lists: classic HID, or HID over GATT. The question
never leaves your machine, the address is used for that one question and is
never kept or written, and when BlueZ cannot answer the file says the link is
not known.

**What it never holds.** Pairing keys, pairing secrets, certificate
fingerprints, IP addresses, Bluetooth addresses, Wi-Fi names, the names of
your hosts, PCs and devices, the machine's host name, this install's
`deviceId`, a controller's serial number, a stream of your controller input
(only the counts and the last report, and the twenty reports of a controller
you captured, taken at rest), and anything from a controller's microphone or
speaker.

---

## 3. Crash reports

This is the part most likely to be misread, so it is stated plainly.

**What is written locally, always.** The app installs handlers for the
fatal signals that carry a usable stack. When the process crashes it appends a
backtrace to `$XDG_STATE_HOME/dish/crash.log` (`~/.local/state/dish/crash.log`
by default) and to stderr, then re-raises on the default disposition so your
shell's exit status and any core-dump collector still see the real signal.

The log holds the signal name and a symbolised stack. It does not contain a
memory image. Note that the re-raise means your system's own core-dump
collector may still write a full core (`systemd-coredump`, `apport` and
`abrt` all do by default), and a core IS a memory image, so it can contain
whatever the app held at that moment, including the satellite address and, in
principle, key material. That file belongs to your system's collector, not to
Dish; treat it as sensitive before attaching it to a public issue.

Delete `~/.local/state/dish/` at any time; the app recreates it only if it
crashes again. In the Flatpak the same file lives under the app's own home,
`~/.var/app/com.tinkernorth.Dish/.local/state/dish/crash.log`.

**What is uploaded, and when.** Settings has a *Share crash reports* switch,
on by default. With it on, an official release build that crashes sends one
report to [Sentry](https://sentry.io), a crash-reporting service operated by
Functional Software, Inc. (San Francisco, USA), which processes it on our
behalf. The report is produced by the Sentry native SDK, running in-process
and armed only after reading your preference, and contains:

- the stack of the crashing thread and the list of loaded modules with their
  versions, so we can see where it failed. There is no minidump and no
  memory image on Linux;
- the app version and a build environment label, the kernel and distribution
  names and versions, the CPU architecture, and a random event id.

It does **not** contain your controller input, your pairings, keys or
settings, your user name, or a device identifier, and the app never reports
launches, sessions or usage: the crash is the only event. Sentry sees the
public IP address of your machine when it receives the report, as any web
service does; the app does not attach it and we do not use it. Sentry retains
crash data for 90 days, then deletes it. We use Sentry's US region, so the
data is stored in the United States. See Sentry's
[privacy policy](https://sentry.io/privacy/) for its role as a processor.

**Opting out.** Turn *Share crash reports* off. The switch disarms the SDK
immediately, so it stops the very next report, not just later ones, and the
choice persists in your config. Opting out never costs you the local log
above, and the redacted report card in Settings works either way.

**Where the SDK keeps its state.** With the switch on, the crash reporter
keeps its run state and any report it has not yet sent under
`~/.local/share/TinkerNorth/Dish/sentry/` (in the Flatpak,
`~/.var/app/com.tinkernorth.Dish/data/TinkerNorth/Dish/sentry/`). Deleting
that directory discards a pending report; the app recreates it only while the
switch is on.

**Builds that cannot upload.** Only official release builds from this
repository (the .deb, the .rpm, the AppImage and the Flatpak) carry the
Sentry address (DSN) that the SDK needs; the Flatpak's is written in its
manifest, because Flathub builds from the committed file and a DSN is not a
secret. A source build, a pull-request build or a fork carries none, does not
even include the SDK, and cannot upload, whatever the switch says.

**Symbols.** To make reports readable we upload the debug symbols of official
builds to Sentry at release time. They describe the program, not you.

---

## 4. System capabilities the app uses

Linux desktop apps do not declare a permission manifest the way Android apps
do, so this is the equivalent list of what the app touches.

| Capability | Why |
|---|---|
| Network sockets: UDP multicast, UDP broadcast listen, UDP unicast, TCP, TLS over TCP | Discovery, pairing, control plane, gamepad stream, for satellites and Moonlight hosts alike. All to your LAN. |
| HTTPS to `github.com` | The update check, and only the check. Section 2.4. Only while *Check for updates automatically* is on. |
| Game-controller device enumeration and IO | Reading controller input, and writing rumble, light, trigger and lamp output back to the controller. `/dev/hidraw*` access is used for reading and writing in USB-direct mode, which needs the udev rule the package installs. |
| A controller's own audio device, through SDL2's audio | Playing the sound and haptics your PC sends for that controller, and opening its microphone only as section 2.1 describes. Never the computer's own microphone. |
| `org.freedesktop.ScreenSaver.Inhibit` on the session bus | Preventing sleep while a controller is actively streaming, so input latency stays low. Released when streaming stops. |
| XDG desktop portals (appearance, settings) | Following your light/dark and reduced-motion preferences. Read only. |
| `/sys/class/bluetooth` and BlueZ on the system bus | Telling "no Bluetooth adapter" from "adapter switched off" in the setup wizard. Read only. |
| `~/.config/` read and write | Settings and pairing state, as listed in section 2.1. |
| `$XDG_STATE_HOME/dish/` file write | The crash log described in section 3. |
| `~/.local/share/TinkerNorth/Dish/sentry/` file write | The crash reporter's run state and any report not yet sent (section 3), only while *Share crash reports* is on. |
| Default browser launch | Opening the links listed in section 2.3, only when you click one. |

The app runs as a normal user. It never asks for root, installs no service and
no driver, does not read your files, and does not access the camera,
location, contacts, or clipboard. The only microphone it ever opens is a
controller's own (section 2.1). USB-direct needs read/write on the
hidraw node, which the packaged udev rule grants to your logged-in seat; if
that rule is absent the claim simply fails and the pad keeps streaming over
SDL.

---

## 5. Your choices

- **Forget a satellite.** Removing a satellite deletes its remembered row,
  its stored pairing key, and its certificate pin from your config, and
  unpairs on the server so any live session is closed there too.
- **Forget a Moonlight host.** Removing it deletes its row and its pinned
  certificate from your config. The host keeps its own list of paired
  clients; clear it in that host's settings.
- **Stop crash reports being sent.** Settings, *Share crash reports*, off.
  The local log under `~/.local/state/dish/` is still written; delete it
  whenever you like. Your system's own core-dump collector is configured
  separately, outside Dish.
- **Stop the update check.** Settings, Updates, *Check for updates
  automatically*. Off means no update-related request leaves your machine, at
  any time, for any reason.
- **Stop the controller database check.** Settings, Updates, *Check for
  controller updates automatically*. Off means no request for the controller
  list leaves your machine; the app keeps the list it has.
- **Delete the controller list and diagnostics files.**
  `~/.local/share/TinkerNorth/Dish/controllers/` and
  `~/.local/share/TinkerNorth/Dish/diagnostics/` can go at any time; the app
  falls back to the list it shipped with.
- **Uninstall.** However you installed it: your package manager, `flatpak
  uninstall`, or removing the files a `cmake --install` placed. Your settings,
  pairings and crash log are deliberately left behind so that reinstalling
  restores your setup; the *Wipe everything* step below removes those too.
- **Wipe everything.** Delete `~/.config/Dish/Dish.conf`,
  `~/.config/com.tinkernorth.Dish/dish.conf`, `~/.local/state/dish/` and
  `~/.local/share/TinkerNorth/Dish/` (for the Flatpak, the whole of
  `~/.var/app/com.tinkernorth.Dish/`). That removes every remembered server,
  pairing key, certificate pin, Moonlight identity, preference, update
  setting, and crash artifact, pending reports included. There is no
  server-side record to delete, because there is no TinkerNorth server.
- **Verify any of this.** The client is free software under
  [LGPL-3.0-or-later](LICENSE). Every claim above is checkable in this
  repository, and you can build the binary yourself. See
  [`CONTRIBUTING.md`](CONTRIBUTING.md).

---

## 6. Children's privacy

Dish is suitable for general audiences. The app collects nothing from anyone,
of any age, beyond the crash report described in section 3 and a support
report you choose to send (section 2.6), so there is no children's data for us
to hold. If you believe that is wrong in some way we have not anticipated,
contact `privacy@tinkernorth.com`.

---

## 7. International transfers

Crash reports, when enabled, are stored by Sentry in the United States
(section 3). The update check goes to GitHub, also in the United States. The
controller database check goes to `dish.tinkernorth.com`, served by Amazon
CloudFront from the edge location nearest you. A support report you choose
to send travels through your own mail provider to `support@tinkernorth.com`.
Nothing else leaves your machine to us.

---

## 8. Changes to this policy

We will update the *Effective date* at the top whenever this policy changes.
Material changes, such as any new analytics backend, will be made here
before the code ships and will be called out in that release's notes in
[`CHANGELOG.md`](CHANGELOG.md). Previous
versions remain in the git history of this file.

---

## 9. Contact

- Privacy questions: `privacy@tinkernorth.com`
- Security disclosures: see [`SECURITY.md`](SECURITY.md)
- General contact and bug reports: open an issue in this repository
