# Manual smoke checklist

The automated suite covers every pure decision in the app, and `qmllint` plus
the literal scanner cover the QML. Neither can cover a window manager, a real
pad, or a human eye. This is the list a person runs before a release.

Build and launch: `scripts/build.sh release`, then run `build-release/dish`.

Mark an item pass only if you saw it happen. "The code looks right" is what the
unit tests are for.

## Window

The window manager draws the decorations, so there is no bespoke chrome to
test — but there are several window managers, and they disagree.

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

Five pages over three stages: Input, Destination, then Type, Feel and Review.
Needs a real pad and a reachable satellite.

- [ ] **Entry points.** The rail's `Set up` action, Home's `+ Add` card, and an
      unbound pad's `Bind…` all open the wizard, and all of them land on
      Home's stack so cancelling returns to Home.
- [ ] **Seeded pad.** Opening from an unbound pad's `Bind…` pre-answers page 1
      and starts on page 2. Back to page 1 still works.
- [ ] **Banner fills in.** The pad, wire and host slots of the banner fill as
      answers land, and the stage markers advance.
- [ ] **Nothing writes early.** Walk pages 1 to 4 and cancel. No binding was
      created, no type changed, no USB path switched.
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
- [ ] **Failure keeps the draft.** Bind against an unreachable host. The wizard
      stays put, the draft survives, the reason arrives as a toast, and the
      primary button is live again.

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

- [ ] **One toast host.** Trigger a failure (unplug the network mid-connect).
      Exactly one toast appears, bottom-centre, and it does not block the
      controls under it.
- [ ] **Streaming pill.** With a pad streaming, the header shows the streaming
      pill on every destination, and it clears when streaming stops.
- [ ] **Close while streaming.** With *Keep running in the background* OFF,
      close the window while a pad is streaming. The quit confirm appears;
      Cancel keeps the app running and streaming.
- [ ] **First run.** With a fresh profile (remove
      `~/.config/TinkerNorth/Dish.conf`) the onboarding flow opens full-screen
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
      item specifically — the extension does not deliver a single left click.
- [ ] **Quit really quits.** Quit from the tray menu ends the process (`pgrep
      dish` is empty) and the item disappears from the panel.
- [ ] **No tray, no trap.** On bare GNOME with no AppIndicator extension, the
      Settings row is disabled and explains why, and closing the window quits
      rather than hiding it.
- [ ] **Panel restart.** With the window hidden, restart the shell (KDE:
      `systemctl --user restart plasma-plasmashell`). The item comes back by
      itself and Show still works.

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
