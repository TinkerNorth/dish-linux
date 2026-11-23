# Dish for Linux

Dish for Linux runs on your Linux computer and sends the controllers plugged into it, or paired with it over Bluetooth, to your gaming PC as wireless controllers.

Dish is made of two parts. The Dish apps run on the device that holds your controller, or that is the controller: Dish for Android, Dish for iOS, Dish for macOS, Dish for Windows and Dish for Linux. Satellite is the small server on the PC you play on, and it makes each Dish controller look to your games like a controller plugged into that PC. You need one of each. Everything is free and open source, the website is [dish.tinkernorth.com](https://dish.tinkernorth.com), and I maintain all of it on my own.

## Download

Dish for Linux 2.8.1 is the current release. Every link below always points at the newest one. Every build is x86_64.

Debian 13: add the APT repository once, and `apt upgrade` keeps Dish current with the rest of your system.

```bash
curl -fsSL https://tinkernorth.github.io/dish-linux/gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/dish-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/dish-archive-keyring.gpg] \
  https://tinkernorth.github.io/dish-linux/debian stable main" \
  | sudo tee /etc/apt/sources.list.d/dish.list
sudo apt update && sudo apt install dish
```

The .deb is built on Debian 13 against Debian's own Qt and asks for that exact Qt build, so on Ubuntu and other derivatives, take the AppImage below.

Fedora: add the DNF repository once, and `dnf upgrade` keeps it current.

```bash
sudo curl -fsSL -o /etc/yum.repos.d/dish.repo \
  https://tinkernorth.github.io/dish-linux/rpm/dish.repo
sudo dnf install dish
```

The 2.8.1 .rpm was built on Fedora 42 and needs Fedora's Qt 6.10, so it does not install on Fedora 44, which ships Qt 6.11. The next release is built on Fedora 44; until then, on Fedora 44, use the AppImage.

If you'd rather not add a repository, [dish-amd64.deb](https://github.com/TinkerNorth/dish-linux/releases/latest/download/dish-amd64.deb) and [dish-x86_64.rpm](https://github.com/TinkerNorth/dish-linux/releases/latest/download/dish-x86_64.rpm) install the same package once (`sudo apt install ./dish-amd64.deb` or `sudo dnf install ./dish-x86_64.rpm`), and you install later versions by hand. Both packages install the udev rule described under Requirements.

Any other x86_64 Linux, Ubuntu and the Steam Deck included: download **[Dish-x86_64.AppImage](https://github.com/TinkerNorth/dish-linux/releases/latest/download/Dish-x86_64.AppImage)**. It carries its own Qt and runs on glibc 2.35 or newer.

```bash
chmod +x Dish-x86_64.AppImage
./Dish-x86_64.AppImage
```

Running it in place needs FUSE; where FUSE is missing, `./Dish-x86_64.AppImage --appimage-extract-and-run` starts it without. It carries update information, so AppImageUpdate or Gear Lever can bring it to the next release. An AppImage can't install the udev rule, so outside SteamOS you install it once yourself; the rule travels inside the AppImage:

```bash
./Dish-x86_64.AppImage --appimage-extract usr/share/dish/70-dish-hidraw.rules
sudo install -m 644 squashfs-root/usr/share/dish/70-dish-hidraw.rules \
     /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

If you'd rather have the sandbox, [Dish-x86_64.flatpak](https://github.com/TinkerNorth/dish-linux/releases/latest/download/Dish-x86_64.flatpak) is a Flatpak bundle on the KDE runtime from Flathub. Dish is not on Flathub itself, so the bundle doesn't update, and its update check starts switched off. Like the AppImage it can't install the udev rule; when a controller needs it, Dish names the copy inside the bundle and the commands to install it.

Arch Linux: the 2.8.1 package in Dish's pacman repository is broken. It installs an AppImage that cannot start. The fix is on its way with the next release; until then, use the AppImage above.

Dish never updates itself. It checks for a new release and shows a notice that links to it, and your package manager or the AppImage's updater does the rest. All files and older versions are on the [releases page](https://github.com/TinkerNorth/dish-linux/releases), and the repositories have their own page at [tinkernorth.github.io/dish-linux](https://tinkernorth.github.io/dish-linux/).

### Satellite and the Dish apps

| App | Where to get it |
|---|---|
| Satellite (Windows, Linux) | [GitHub releases](https://github.com/TinkerNorth/satellite/releases/latest) |
| Dish for Android | [Google Play](https://play.google.com/store/apps/details?id=com.tinkernorth.dish), or the APK from [GitHub releases](https://github.com/TinkerNorth/dish-android/releases/latest) |
| Dish for iOS | Not yet released |
| Dish for macOS | [GitHub releases](https://github.com/TinkerNorth/dish-mac/releases/latest) |
| Dish for Windows | [GitHub releases](https://github.com/TinkerNorth/dish-windows/releases/latest) |
| Dish for Linux | [GitHub releases](https://github.com/TinkerNorth/dish-linux/releases/latest) |

## Getting started

1. Install Satellite on the PC you play on and start it. Its dashboard shows a 4-digit PIN under Device Pairing, with a new one every five minutes.
2. Install Dish for Linux on the computer your controller is connected to, plug the controller in or pair it over Bluetooth, and start Dish. Both computers have to be on the same local network.
3. Pair them. Open Connections: the PC appears under Found once Satellite answers (Scan looks again). Press Pair… and type the PIN from the dashboard. If Dish shows you a PIN instead, approve the request on the dashboard. Dish remembers the PC, so you only pair once.
4. Play. On Home, Set up a controller walks you through the controller, the PC and what it should be to your games; Bind… on a controller's card does the same in one page. Emulate as picks the type the game sees, from the types Satellite offers, and Apply makes it live.

## What it does

Dish reads each controller on your computer and sends it to the PC you bound it to, where Satellite turns it into a virtual controller. Input travels over UDP on your own network, encrypted with a key the two computers agree on when they pair. Several controllers can go to several PCs at once.

Every controller the system recognises works over USB and Bluetooth on the Standard path, through SDL. Over USB, Dish can also take a DualSense, DualShock 4, Switch Pro Controller, 8BitDo pad in HID mode, PDP Switch pad or Steam Controller on the Direct path, reading it without SDL's rate cap; the Controllers page shows which path each one is on. Bluetooth is never Direct, and Xbox pads stay on Standard because the kernel's own driver holds them. Dish carries the list of controllers the whole Dish family recognises and keeps it current, so a newer list can teach it a controller without a new release.

Motion, touchpad and battery go to the PC. Rumble, the light bar, player lights, adaptive triggers and the mute lamp come back from the game, and a DualSense or DualShock 4 can carry the PC's sound and your voice through its own speaker and microphone. What reaches the game also depends on the type you emulate and on the PC; Satellite's README has that table.

I don't own every controller. The 2015 and 2026 Steam Controllers on the Direct path, every controller on a Steam Controller receiver, two of the same controller on Direct at once, and DualSense HD haptics are built but not yet confirmed on real hardware.

Each controller's inspector tests what its path carries: rumble, the light bar, player lights, adaptive triggers, the mic lamp, the speaker, the microphone and the sticks, all from Dish alone, then asks whether it worked. Between that and the PC's own view, you can tell whether a problem is the controller, Dish or the PC.

Dish doesn't need Satellite to play on a Sunshine, Apollo or Wolf host. Under Connections, Moonlight hosts finds them on your network or takes an address, and Dish shows a PIN to type into the host's page. Dish is then the controller for that host; it holds the session open but never shows the stream.

Closing the window keeps controllers streaming behind a tray icon. Settings can keep the computer awake while you play, and even with the lid closed. Dish closes its sessions before the computer suspends and reopens them on resume (Moonlight sessions don't come back by themselves yet). It follows your desktop's light or dark theme and speaks English, Bosnian, German, Spanish, French and Brazilian Portuguese.

## Requirements

Linux on x86_64. There are no ARM builds yet. The .deb needs Debian 13, the 2.8.1 .rpm needs a Fedora with Qt 6.10, and the AppImage needs glibc 2.35 or newer (Ubuntu 22.04 and later). Ubuntu 24.04 ships Qt 6.4, below the Qt 6.7 Dish needs, so it has no package of its own and uses the AppImage.

Permissions: Dish never asks for root. The Direct path, and a Bluetooth DualSense or DualShock 4's light bar, motion, touchpad, adaptive triggers and player lights, need read and write access to the controller's hidraw node, which Dish's udev rule (`70-dish-hidraw.rules`) gives to the user at the seat and, for sessions without logind, to the `input` group. The .deb and .rpm install it and reload udev; the AppImage and the Flatpak can't, and Dish tells you when a controller needs it. Without the rule every controller still streams on the Standard path, and a Bluetooth Sony pad keeps only rumble. SteamOS needs nothing installed, because its own rules already open those nodes.

The tray icon needs a StatusNotifier host. KDE, Xfce, Cinnamon, MATE, LXQt, sway and Waybar have one; stock GNOME needs the AppIndicator extension, and without it closing the window quits Dish.

Network: both computers on the same local network. Dish finds Satellite over mDNS (UDP 5353) and Satellite's older UDP beacon on port 9879, pairs and talks to it over HTTPS on TCP 9443, and streams on UDP 9876. A Moonlight host is paired over HTTP on port 47989 and called over TLS on 47984. A firewall on the Linux computer may need to let Dish send and receive on your local network.

## Trust and privacy

Every release has a `SHA256SUMS` file signed with cosign by the release workflow. To check a download on Linux, put `SHA256SUMS` beside it and run:

```bash
sha256sum --ignore-missing -c SHA256SUMS
```

On a Mac, `shasum -a 256 --ignore-missing -c SHA256SUMS` does the same. [SECURITY.md](SECURITY.md) has the signature and build provenance checks, and how to report a vulnerability.

The APT and DNF repositories sign their metadata with the repository key whose fingerprint is in [packaging/repo/README.md](packaging/repo/README.md), and apt and dnf check every download against it. The files on the release page carry no signature of their own: no distribution signs them, and the .rpm has no package signature. The checksum and its cosign signature are how you can tell a downloaded file came from this repository's release.

Dish for Linux works on your own network and has no account, no analytics and no ads. Three things can leave your computer, and each has a switch in Settings: a check for a new version against GitHub (Check for updates automatically, which starts off in the Flatpak), a check for a newer controller database at dish.tinkernorth.com (Check for controller updates automatically), and, if Dish crashes, one crash report to Sentry (Share crash reports). Crash reports are on by default. Controller input never leaves your network. The diagnostics file from Report a problem is something you send yourself; Dish never sends it. [PRIVACY.md](PRIVACY.md) says exactly what is stored and sent.

Dish for Linux is licensed under the GNU Lesser General Public License v3.0 or later ([LICENSE](LICENSE), with [COPYING.GPL3](COPYING.GPL3)). The third-party components and their licences are in [THIRD_PARTY.md](THIRD_PARTY.md).

## For developers

Dish for Linux is C++20 with a Qt Quick interface, built with CMake. The presets in `CMakePresets.json` (`debug`, `release` and `package`) carry the same flags CI uses, warnings as errors included, and the scripts drive them:

```bash
scripts/install-deps.sh          # once: the toolchain CI installs (apt)
scripts/build.sh debug test      # build, then run the tests
scripts/ci-local.sh              # every CI gate, in CI's order
```

`scripts/package-in-container.sh` builds, installs and starts the .deb and .rpm in CI's containers, and `scripts/build-appimage.sh` builds the AppImage.

Where things are written down:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): the layers, the state primitives, the threads and the input hot path.
- [docs/contract.md](docs/contract.md): where each part of the Satellite protocol lives in this tree.
- [docs/MOONLIGHT_HOSTS.md](docs/MOONLIGHT_HOSTS.md): what Sunshine and Wolf actually do, for the Moonlight path.
- [docs/PACKAGING.md](docs/PACKAGING.md): every package and channel, the udev rule, the update check and the desktop integration.
- [docs/FLATHUB.md](docs/FLATHUB.md): the Flathub submission procedure, if it happens.
- [docs/QML_CONTRACT.md](docs/QML_CONTRACT.md) and [docs/QML_UI_KIT.md](docs/QML_UI_KIT.md): the interface's binding surface and its component kit.
- [docs/QML_MANUAL_SMOKE_CHECKLIST.md](docs/QML_MANUAL_SMOKE_CHECKLIST.md): what is checked by hand before a release.
- [CHANGELOG.md](CHANGELOG.md): what changed in each release.

[CONTRIBUTING.md](CONTRIBUTING.md) covers setting up, where code goes, the interface, translations, the code style, the tests, what CI runs and how a release is cut. Please read it before opening a pull request.

## Getting help

If something doesn't work, open an issue at [github.com/TinkerNorth/dish-linux/issues](https://github.com/TinkerNorth/dish-linux/issues/new/choose). The most useful thing you can attach is the diagnostics file: in Settings, open Diagnostics…, use Report a problem, say what happened, and press Continue in your mail app. The file leaves out pairing keys, addresses and the names of your PCs and devices; read it before you attach it, it's plain JSON. The same button starts a mail to support@tinkernorth.com if you'd rather write to me than open an issue. Thunderbird attaches the file for you; with other mail apps, attach it from the folder Dish opens.

Please also say which Dish for Linux version you run and how you installed it, your distribution, which Satellite version or Moonlight host you use, and which controller. If the problem is in Satellite rather than here, its own repository is the better place. For a security problem, don't open a public issue; [SECURITY.md](SECURITY.md) says how to report it privately.
