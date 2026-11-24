# Manual smoke checklist

The automated suite covers every pure decision in the app, and `qmllint` plus
the literal scanner cover the QML. Neither can cover a window manager, a real
pad, or a human eye. This is the list a person runs before a release.

Build and launch: `scripts/build.sh release`, then run `build-release/dish`.

Mark an item pass only if you saw it happen. "The code looks right" is what the
unit tests are for.

## Window

The window manager draws the decorations, so there is no bespoke chrome to
test, but there are several window managers, and they disagree.

- [ ] **Decorations.** The WM's own title bar appears, with working minimize,
      maximize and close. Dish draws no second title bar.
- [ ] **Minimum size.** Drag the window as small as it will go. It stops at
      900x620 with nothing clipped.
- [ ] **Tiling WM.** Under a tiling WM (sway, i3, Hyprland) the window fills
      its tile, the rail and content both reflow, and nothing is clipped.
- [ ] **Wayland and X11.** Launch under both. The body is opaque in both, the
      app icon resolves from the .desktop file, and HiDPI scaling is applied
      once, not twice.
- [ ] **Fractional scaling.** At 125% and 150%, text stays crisp and no
      one-pixel divider disappears.

## Navigation

- [ ] **Rail collapse.** The rail-head toggle collapses the rail to the 48 px
      icon strip and expands it back to 236 px. Collapsed, the toggle is still
      reachable and is the only way back. The choice survives a restart.
- [ ] **Collapsed tooltips.** With the rail collapsed, hovering each entry
      shows its label after about half a second. All five entries (Home,
      Controllers, Connections, Support Dish, Settings) are distinguishable
      from their icon alone.
- [ ] **F6.** Moves focus between the rail and the content pane.
- [ ] **Ctrl+,** opens Settings.
- [ ] **Alt+Left** goes back exactly when the header back chevron is visible,
      and does nothing when it is not. In the setup wizard it steps one page
      back instead.
- [ ] **Header.** Every destination and every pushed detail page shows a title,
      and a sub-line with a status dot where it has one to show.

## Setup wizard

Six steps over three stages: Input, Destination, then Type, Session, Feel and
Review (the footer reads "Step 3 of 3" for the last four). Session exists only
for a Moonlight destination and is skipped for a satellite, so a satellite
binding walks five pages. Needs a real pad and a reachable satellite; the
Moonlight items also need a host running Sunshine, Apollo or Wolf.

- [ ] **Entry points.** The rail's `Set up` action, Home's `+ Add` card, and an
      unbound pad's `Bind…` all open the wizard, and all of them land on
      Home's stack so cancelling returns to Home.
- [ ] **Seeded pad.** Opening from an unbound pad's `Bind…` pre-answers the
      Input step and starts on Destination. Back to Input still works.
- [ ] **Banner fills in.** The pad, wire and host slots of the banner fill as
      answers land, and the stage markers advance.
- [ ] **Nothing writes early.** With a satellite destination, walk Input,
      Destination, Type and Feel and cancel. No binding was created, no type
      changed, no USB path switched. (A Moonlight destination is different: the
      Session step re-checks the host, reads its app list, and a picked app is
      recorded on the host at once.)
- [ ] **Discard confirm.** With answers entered, press Esc, click a rail entry,
      and close the window. Each raises the discard confirm, and Cancel keeps
      the draft intact.
- [ ] **Apply, Standard path.** Finish the wizard with the Standard path. The
      apply overlay shows its steps, the binding lands, the wizard pops to Home
      and the new row is there.
- [ ] **Apply, Direct path.** Finish with the Direct path on a raw-HID capable
      pad, with `packaging/udev/70-dish-hidraw.rules` installed. Past 4 seconds
      the slow hint appears, and Cancel is offered only while that step is
      active.
- [ ] **Missing udev rule.** Remove the rule, reload udev, replug the pad and
      pick Direct. The claim fails with the permission-denied copy, the pad
      keeps streaming over Standard, and the app never asks for root.
- [ ] **Direct fallback.** If the claim does not land, the run continues, the
      pad streams over Standard, and you get a **warning** toast, not an error.
- [ ] **Moonlight session step.** Pick a Moonlight host on Destination and
      continue through Type. The Session step reads "Checking <host>…" and then
      "Reading the app list from <host>…". On an unpaired host it offers
      `Pair now`. On a paired one it lists the host's apps to pick from, and
      without a pick it says "Without a pick, Dish starts whatever <host> lists
      first." Feel and Review follow it, and Back from Feel returns to Session.
- [ ] **Failure keeps the draft.** Bind against an unreachable host. The wizard
      stays put, the draft survives, the reason arrives as a toast, and the
      primary button is live again.

## Diagnostics

Settings, then the `Diagnostics…` row under the Diagnostics header, opens the
page. Needs a pad, and a satellite for the Hosts items.

- [ ] **Hosts.** With no satellite the Hosts section reads "No satellites yet.
      Pair one on the Connections page and its session shows here." With one
      paired, its card shows Address, Protocol, Epoch, Controllers, Virtual
      controllers, Round trip, Missed heartbeats and Mouse control, and a live
      host also shows the "Controller audio" row with Mic, Speaker and haptic
      audio chips. The figures move while a pad streams.
- [ ] **Controller list.** The Controllers section lists every connected pad
      with its connection, path and input rate, a Bluetooth pad as Bluetooth and
      a claimed one as USB direct, plus its battery when it reports one. A pad
      the controller database knows carries a Verified, Layout guessed or
      Flagged chip. With no pad it reads "No controllers connected."
- [ ] **Inspect input.** `Inspect input` on a controller card opens the
      "Controller inspector". Move a stick and press a button: the Live input
      section follows, and "Pressed:" names the buttons. Until the pad sends its first
      report the page says "Waiting for this controller's first report."
- [ ] **Stick tests.** In the inspector, `Drift test (hands off the sticks)`
      counts down and ends with "Resting drift: L .. · R .. Suggested dead
      zone: ..". `Range test (sweep full circles)` ends with "Reach" and
      "Circularity error" figures. Each asks "Did it work?" with Yes and No.
- [ ] **Bench.** Under "Feedback and audio tests", each row is a test the pad
      can carry on its current path, such as Rumble (Weak, Strong, Both) or
      Light bar. Start one: the pad responds, the page says "Testing: <name>…",
      and afterwards "<name>: did it work?" appears with Yes and No. Start a
      second test while one runs and the first ends. A pad with nothing to test
      reads "This controller has nothing to test on the path it is on now."
- [ ] **Bench microphone.** On a pad with a microphone, `Show its level` shows
      "Level n% · peak n%" while you speak, and `Stop` ends it early.
- [ ] **Controller database.** `Check now` runs one check and the Last check
      line updates. With "Check for controller updates automatically" off in
      Settings the button is disabled and the page says "Automatic controller
      checks are off in Settings, so Dish sends no request for the list."
- [ ] **Save diagnostics.** `Save diagnostics` shows the toast "Diagnostics
      saved. The file manager shows its folder." and opens the folder
      `~/.local/share/TinkerNorth/Dish/diagnostics`, which holds a new
      `dish-diagnostics-dish-linux-<date>-<time>.json`.
- [ ] **Events.** The Events section lists what changed (a pad plugged in, a
      link going up), newest first. `Copy events` raises the toast "Events
      copied."

## Report a problem

Reached from Diagnostics, `Report a problem` or `Ask a question` under "Send to
support". Do not click Send in your mail app during the check.

- [ ] **Two entry points.** `Report a problem` opens a page headed "Report a
      problem". `Ask a question` opens "Ask a question", and its category is
      "A question".
- [ ] **Form.** The page offers a Category (Controller not recognised, No rumble
      or feedback, Connection or latency, Audio, Something else), a text box
      "What happened, and what did you expect?", and a Controller list whose
      first row is "No particular controller".
- [ ] **File contents shown.** "What the file holds" lists what goes in, and
      says it never holds pairing keys, certificate fingerprints, IP addresses,
      Wi-Fi names or the names of your PCs and devices.
- [ ] **Waiting for satellites.** On opening, "Asking each Satellite for its
      side of the session…" shows, and `Preview file` and `Continue in your mail
      app` stay disabled until it finishes.
- [ ] **Preview.** `Preview file` shows the file as text, and the button then
      reads `Hide file`.
- [ ] **Mail hand-off.** `Continue in your mail app` opens your mail app
      addressed to support@tinkernorth.com, and the file manager shows the
      file's folder. The page says "Your mail app has the message. Nothing is
      sent until you click Send." or, where the file did not attach, "Your mail
      app opened without the file: attach the one the file manager shows."
- [ ] **Capture this controller.** With a pad claimed over Direct picked,
      `Capture this controller` says "Recording its reports. Leave the
      controller untouched…" and then "Captured: the file now holds this
      controller's descriptors and the reports it sent while it lay still."

## Updates

The pill is the shell's, in the header beside the streaming pill. It shows only
while an update is on offer, so this needs a build older than the newest
release (or a test manifest). Settings has the controls.

- [ ] **Pill appears.** With a newer release out, an arrow-leaving-a-tray pill
      appears at the right of the header on every destination. Hovering it shows
      the tooltip "Update available: Dish <version>".
- [ ] **Popover.** Clicking the pill opens a popover headed "Update available",
      with "Dish <version>" under it, the line "Dish is installed by your
      package manager, so it updates itself there. The release page has the
      packages and the notes.", `Open release page` and `Skip this version`.
      Clicking the pill again closes it.
- [ ] **Open release page.** `Open release page` closes the popover and opens
      the release notes in the browser.
- [ ] **Skip.** `Skip this version` closes the popover and the pill goes away.
- [ ] **Unsupported build.** On a build below the minimum supported version, the
      pill carries a warning dot, the popover adds "This version is no longer
      supported.", and `Skip this version` is not offered.
- [ ] **Settings.** In Settings, `Check for updates` runs a check and the last
      check time updates; "Check for updates automatically" turns it off, and
      `Check for updates` is then disabled. Offline, the page says "You're
      offline. Dish will retry automatically."

## Crash report

Needs a run that crashed last time (the card does not exist otherwise).

- [ ] **Card after a crash.** After a crash, relaunch and open Settings. A card
      reads "Dish closed unexpectedly last time." and shows the redacted report
      text.
- [ ] **Actions.** `Copy` puts the text on the clipboard. `Report on GitHub`
      opens a prefilled issue in the browser, and nothing is uploaded by Dish.
      `Dismiss` removes the card, and it stays gone on the next launch.
- [ ] **Share crash reports.** The "Share crash reports" switch is on by default.
      Turn it off, restart, and it is still off.

## Moonlight hosts

Connections, then the `Moonlight hosts` row ("Stream to a PC running Sunshine,
Apollo or Wolf instead of a satellite.") opens the page. Needs a Sunshine,
Apollo or Wolf host on the same network, and a pad for the binding items.

- [ ] **Scan.** On opening, the page says "Looking for Moonlight hosts" and
      then lists each host that answers under "Found". `Scan` rescans, and reads
      "Scanning…" while it runs. With no host, "No Moonlight hosts found" offers
      `Get Sunshine` and `Add by address…`.
- [ ] **Add a host.** `Add by address…` opens "Add a host by address". Enter the
      host's IP address or hostname (placeholder `192.168.1.20`), optionally a
      name, and press `Add`. The host appears in the list.
- [ ] **Pair with a PIN.** On a host marked "Not paired", `Pair…` opens "Pair
      with <host>" and shows a PIN: "Type <PIN> into the Moonlight or Sunshine
      page on <host>." Enter it in the host's UI: the sheet moves past "Waiting
      for the host to accept the PIN…" and the row reads "Paired". `New code`
      gives another PIN, and `Cancel` abandons the pairing. A wrong or
      unanswered PIN shows the reason text instead.
- [ ] **Trust is not liveness.** A paired host shows "Paired" or "Remembered",
      never a pulsing live dot, and the page says "Pairing is one-time trust, not
      a connection. Dish re-checks it when you use a controller."
- [ ] **Pick an app and bind a pad.** Run the setup wizard with this host as
      the destination. On the Session step, pick an app from the list, then `Continue ›`
      through Feel and finish at Review with `Bind`. On the Moonlight hosts page
      the host's row shows "In use by" with the controller count, and the pad is
      bound on Home. (`Start a session` appears on the Session step only after
      the host ended a session.)
- [ ] **Session and quit.** While a session runs, the host row shows the app
      under "Session". The `⋯` menu offers `Quit session`, which ends it, and
      `Forget`.
- [ ] **Forget.** `Forget` asks "Forget <host>?", says Dish deletes its pairing
      and you will need the PIN again, and lists the bindings that will be
      dropped. Confirming removes the row.

## Controller audio

Needs a pad with a microphone or speaker (a DualSense over USB direct or
Bluetooth) and a host that carries audio.

- [ ] **Switches.** The "Configure binding" page for a bound pad (opened from
      its row on Home or Controllers), and the wizard's Feel step, show "Microphone" ("The pad's mic carries your voice to the
      host. Off sends nothing.") and "Controller sound" ("Audio from the host
      plays on the pad's speaker or headset."). Each only appears where the pad
      and host can carry it. Microphone starts off, Controller sound starts on.
- [ ] **Settings persist.** Turn Microphone on and apply. Reopen the binding
      page and see whether the switch is still on. The switches are stored per
      pad in the code, so a second pad should be unaffected.
- [ ] **Live mute on the binding page.** With the microphone armed, a button
      reads `Mic live`. Clicking it changes it to `Mic muted` and mutes the pad.
- [ ] **App-wide mic chip.** With an armed microphone, a "Mic live" chip
      appears in the header on every destination. Clicking it reads "Mic
      muted" and mutes every armed pad, and clicking again unmutes them. With no
      armed microphone the chip is not shown.
- [ ] **Pad's own mute button.** Press the DualSense mute button: the chip
      changes with it.

## Live input

Needs a pad and a satellite. Nothing here can be faked in CI.

- [ ] **Telemetry ticks.** On Controllers, the per-slot Hz and the footer
      events/s and sends/s move while you move the sticks, and settle when you
      stop.
- [ ] **Latency.** A connected row shows a latency figure with its sample
      count, and it updates about once a second. It never reads `~0.0 ms`.
- [ ] **Bluetooth pad.** A pad connected over Bluetooth shows the Bluetooth
      transport chip, the bluetooth glyph family, and offers no USB path
      control.
- [ ] **Battery.** A pad that reports battery shows the chip; one that does not
      shows no chip rather than a wrong number.
- [ ] **Rumble and lightbar.** A game on the satellite drives rumble and the
      light bar on a capable pad.
- [ ] **Configure controls.** On a generic USB pad, capture and assign
      each output, flip the Y inverts, and confirm the change takes effect on
      the next report with no re-plug. Reset to defaults restores the stock
      layout.
- [ ] **Capture does not self-assign.** Arm capture and leave the pad at rest.
      Nothing is captured from idle stick jitter.

## Appearance

- [ ] **Toggle repaints live.** Settings, Appearance: click Light, then Dark,
      then System. The whole window repaints immediately each time. No surface
      is left on the old palette.
- [ ] **System resolves correctly.** With mode System, flip the desktop between
      dark and light (GNOME: Settings, Appearance; KDE: the global theme). The
      app follows live through the XDG appearance portal. On a desktop with no
      portal it stays dark, which is the documented fallback.
- [ ] **Light palette is readable.** In Light, walk every page. No text or
      glyph washes out; brand glyphs stay legible on white cards.
- [ ] **Reduced motion.** Turn animations off (GNOME:
      `gsettings set org.gnome.desktop.interface enable-animations false`;
      KDE: set the animation speed to Instant), then re-focus the app window.
      Indeterminate bars become a static filled track, the rail collapse stops
      animating, and no glyph animates.

## Transients and guards

- [ ] **Overflow menus open with a visible width.** On Connections, click the
      `⋯` on a host card: the Forget menu appears, sized to its text. On Home,
      open a slot row's context menu the same way. A Menu takes its width from
      its background, so a restyled background with no `implicitWidth` opens the
      menu at zero width: it takes focus and draws nothing, which looks exactly
      like a dead button. Check this after any change to a Menu background.
- [ ] **One toast host.** Trigger a failure (unplug the network mid-connect).
      Exactly one toast appears, bottom-centre, and it does not block the
      controls under it.
- [ ] **Streaming pill.** With a pad streaming, the header shows the streaming
      pill on every destination, and it clears when streaming stops.
- [ ] **Close while streaming.** With *Keep running in the background* OFF,
      close the window while a pad is streaming. The quit confirm appears;
      Cancel keeps the app running and streaming.
- [ ] **First run.** With a fresh profile (remove
      `~/.config/com.tinkernorth.Dish/dish.conf`) the onboarding flow opens full-screen
      over the shell. Both Skip and finishing the flow mark it done, and it
      does not reappear on the next launch.

## Running in the background

None of this is reachable from CI: the suite runs offscreen with no session bus
and no panel.

- [ ] **Tray item appears.** Launch on a desktop with a StatusNotifier host
      (KDE, XFCE, Cinnamon, sway/Waybar, or GNOME with the AppIndicator
      extension). The item shows with the Dish icon, and its tooltip names the
      streaming controller count while a pad streams.
- [ ] **Close hides, and says so once.** With the preference on, close the
      window. The window disappears, the pad keeps streaming, and a desktop
      notification says Dish is still running. Re-open and close again: no
      second notification, ever.
- [ ] **Show comes back.** From the tray menu, Show Dish restores the window,
      raises it above other windows and focuses it. On GNOME check the menu
      item specifically: the extension does not deliver a single left click.
- [ ] **Quit really quits.** Quit from the tray menu ends the process (`pgrep
      dish` is empty) and the item disappears from the panel.
- [ ] **No tray, no trap.** On bare GNOME with no AppIndicator extension, the
      Settings row is disabled and explains why, and closing the window quits
      rather than hiding it.
- [ ] **Panel restart.** With the window hidden, restart the shell (KDE:
      `systemctl --user restart plasma-plasmashell`). The item comes back by
      itself and Show still works.

## Keep awake

`systemd-inhibit --list` is the oracle: Dish's `idle`/`block` lock is the
machine hold, and `qdbus org.freedesktop.ScreenSaver /ScreenSaver` reports the
display one. Neither is visible to CI.

- [ ] **Default reach is the machine, not the screen.** With a pad streaming on
      a fresh config, `systemd-inhibit --list` shows a Dish `idle`/`block` lock
      and the screen still blanks on its normal schedule. The pill reads
      "Streaming · computer kept awake".
- [ ] **Configure lands on the setting.** Click **Configure** on the pill from
      any destination. Settings opens at the rail root, not as a pushed detail
      with a back chevron.
- [ ] **Never means never.** Set *Keep the computer awake* to Never. The lock
      disappears immediately, the pill drops to "Streaming", and the quit
      confirm still appears when closing with a pad streaming.
- [ ] **The idle window lets go and takes back.** Set While playing with a
      1-minute window. Leave the pad untouched: within ~1 minute the lock
      disappears and the pill drops to "Streaming". Move a stick: the lock and
      the suffix come back within a second.
- [ ] **A resting pad does not hold it.** With a USB-direct DualSense or DS4
      claimed (which gets no deadzone), leave it on the desk under While
      playing. The lock must still expire: a drifting stick must not read as
      activity.
- [ ] **While connected ignores stillness.** Set While connected and leave the
      pad untouched well past the window. The lock stays.
- [ ] **The display opt-in widens, never creates.** Turn *Keep the display
      awake too* on: the screen stops blanking and the pill reads "display kept
      awake". Turn the mode to Never with the switch still on: no lock at all.
- [ ] **The lid opt-in takes a second lock.** On a laptop, with a pad
      streaming, turn *Keep streaming with the lid closed* on: `systemd-inhibit
      --list` shows a Dish `handle-lid-switch`/`block` lock beside the `idle`
      one, and the pill reads "kept awake, even with the lid closed". Close the
      lid (wait out `HoldoffTimeoutSec`, 30 s after boot or resume): the
      machine stays up and the pad keeps streaming. Turn the switch off, or set
      the mode to Never: the `handle-lid-switch` lock disappears at once. The
      switch is hidden while the mode is Never.

## Suspend and resume

- [ ] **Suspend closes the sessions.** With a pad streaming, `systemctl
      suspend`. On resume the connection rows go Connecting and return to
      Connected within a couple of seconds, not the ~10 s heartbeat timeout,
      and the pad streams again with no manual reconnect.
- [ ] **Resume on another network.** Suspend on one LAN, resume on another,
      then return. The rescan relearns the satellite rather than retrying a
      stale address.
- [ ] **No stuck delay lock.** After a suspend/resume cycle,
      `systemd-inhibit --list` shows exactly one Dish `sleep`/`delay` lock, not
      several, and none after the app quits.

## Accessibility

- [ ] **Keyboard only.** Unplug the mouse. Reach every destination, open the
      wizard, complete a binding, and open and dismiss a dialog.
- [ ] **Focus is visible.** Every focused control draws the 1 px accent border
      and the 2 px ring outside it. No control takes focus without showing it.
- [ ] **Screen reader.** With Orca on, each rail entry, header control and
      status pill announces something meaningful.
