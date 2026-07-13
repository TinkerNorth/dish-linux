// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QList>
#include <QSettings>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace dish::net {

// Persistent registry of remembered connections, per-satellite shared keys and
// TOFU cert pins. Backed by QSettings (XDG: ~/.config/Dish/Dish.conf), the
// Linux analogue of SharedPreferences / UserDefaults.
//
// Identity: rows are keyed on DiscoveredServer::id() — "mid:<machineId>" when
// the satellite advertises one, else the legacy "wifi:<ip>:<udpPort>". Rows
// persisted before protocol-1 (no machineId) still load and are upgraded in
// place the first time the box is seen with a stable id (remember() collapses
// the legacy ghost and carries its pairing key forward).
//
// Cert pins are keyed by HOST (IP string) because the TLS layer only knows the
// URL host at verify time; remember() migrates the pin when a machineId-matched
// satellite moves address. Pin accessors are mutex-guarded — PairingClient
// verifies pins from a QtConcurrent worker thread while the Qt main thread may
// be writing (QSettings itself is not thread-safe across threads).
class ConnectionStore {
  public:
    explicit ConnectionStore(std::unique_ptr<QSettings> settings = nullptr);

    QString getOrCreateDeviceId();

    QList<models::RememberedWifi> remembered() const;
    void remember(const models::DiscoveredServer& server);
    void forget(const QString& id);

    // Re-point remembered rows from a fresh discovery scan: a satellite whose
    // machineId matches a remembered row (or that upgrades a legacy ip:port
    // row) gets its endpoint refreshed IN PLACE, so auto-reconnect targets the
    // current address after a DHCP move — no manual re-add. Never adds a new
    // satellite. Mirrors dish-windows ConnectionStore::refreshFromDiscovery.
    void refreshFromDiscovery(const QList<models::DiscoveredServer>& discovered);

    std::optional<QString> sharedKey(const QString& id) const;
    void setSharedKey(const QString& keyHex, const QString& id);
    // Drop only the pairing key (terminal 401 / close-notify(unpaired)): the
    // row survives so the UI can park it on "Needs pairing".
    void forgetKey(const QString& id);

    // ── TOFU cert pins (SHA-256 fingerprint hex, keyed by host/IP) ──────────
    std::optional<QString> certPin(const QString& host) const;
    void setCertPin(const QString& host, const QString& fingerprintHex);
    void forgetCertPin(const QString& host);

  private:
    void persist(const QList<models::RememberedWifi>& list);
    // remember() helpers — see dish-windows ConnectionStore for the origin of
    // each rule.
    bool refreshKnownBox(const models::DiscoveredServer& server);
    void collapseLegacyGhosts(const models::DiscoveredServer& server, const QString& id);
    void migratePinOnAddressChange(const std::optional<QString>& oldIp, const QString& newIp);

    std::unique_ptr<QSettings> settings_;
    // Guards the pin accessors only (see class comment). The remembered-list /
    // key accessors stay main-thread-only like before.
    mutable std::mutex pinMtx_;
};

} // namespace dish::net
