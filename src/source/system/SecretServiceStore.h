// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SecretServiceStore — satellite pairing keys in the desktop keyring, over the
// freedesktop Secret Service D-Bus API (org.freedesktop.secrets).
//
// gnome-keyring and KWallet both implement it, which covers GNOME, KDE and every
// desktop that runs one of the two agents. There is no new dependency: this is
// QtDBus, which Dish already links for the screensaver inhibit and the BlueZ
// probe.
//
// A pairing key is the satellite's root secret — it authenticates every REST
// call AND is the HKDF input for the session key that encrypts input packets, so
// it deserves better than a plaintext line in a config file that a stray backup
// or a dotfile sync can carry off.
//
// `available()` is false wherever no Secret Service answers: a headless box, a
// bare window manager, some Flatpak sandboxes. The caller falls back to the
// 0600 config file rather than failing to connect — see SatelliteSharedKeyRepository.
//
// Every call is SYNCHRONOUS with a short timeout. The keyring lives on the
// session bus one hop away, the repository API above it is synchronous, and the
// alternative — an async key read on the connect path — would restructure the
// session FSM for a call that takes single-digit milliseconds.

#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace dish::source {

class SecretServiceStore {
  public:
    SecretServiceStore();

    // Whether a Secret Service answered on the session bus AND a session opened.
    // Probed once at construction; a keyring that dies later surfaces as failed
    // reads and writes, which the caller already treats as "fall back".
    bool available() const { return available_; }

    // The stored hex key for a satellite id, or nullopt when absent, locked with
    // no way to unlock, or the service is gone.
    std::optional<QString> read(const QString& id) const;

    // Creates or replaces the item. False on any failure, so the caller can fall
    // back rather than silently dropping a key it will need to reconnect.
    bool write(const QString& id, const QString& secretHex);

    // Removes the item. True when it is gone afterwards, including when it was
    // never there.
    bool erase(const QString& id);

    // Satellite ids this store holds keys for.
    std::vector<QString> ids() const;

  private:
    bool available_ = false;
    QString sessionPath_;
};

} // namespace dish::source
