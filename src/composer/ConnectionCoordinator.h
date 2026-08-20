// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The imperative command surface over the connection subsystem: it feeds the
// ConnectionsComposer's upstream Observables from the Qt signal world and
// exposes the user commands. The binding table and hot-path sender factory stay
// on ConnectionHub, because the input thread reads them.

#pragma once

#include "Network/ConnectionHub.h"
#include "Network/WifiConnectionManager.h"
#include "architecture/Observable.h"
#include "composer/ConnectionsComposer.h"

#include <QObject>
#include <QString>

#include <memory>
#include <string>
#include <vector>

namespace dish::composer {

class ConnectionCoordinator : public QObject {
    Q_OBJECT
  public:
    ConnectionCoordinator(net::WifiConnectionManager* wifi, net::ConnectionHub* hub,
                          QObject* parent = nullptr);
    ~ConnectionCoordinator() override;

    // Re-exposed by reference, never mirrored — the list is owned by the
    // composer, and a second copy here would be a two-writer race.
    const arch::Observable<std::vector<ConnectionRow>>& connections() const {
        return composer_->state();
    }

    std::optional<ConnectionRow> summary(const std::string& id) const;

    void bind(const QString& slotId, const QString& connectionId);
    void unbind(const QString& slotId);

    // Unbinds this connection's slots first, then drops the row + key + pin.
    void forgetConnection(const QString& connectionId);

    // Connect every remembered satellite that is not already live. Failures are
    // silent: the row's own chip is the cue.
    void autoReconnectAll();

    // Reconnect a remembered satellite without a rescan and without re-pairing
    // (the key persists). Prefers a currently-discovered endpoint, whose IP is
    // guaranteed current; otherwise kicks a discovery pass to relearn a moved
    // box AND tries the last-known address right now.
    void reconnectConnection(const QString& connectionId);

    // Graceful teardown that KEEPS the remembered row and pairing key — contrast
    // forgetConnection, which drops them.
    void disconnectConnection(const QString& connectionId);

  signals:
    // A Qt bridge for consumers that can't hold an Observable subscription. The
    // authoritative value is always connections().value().
    void connectionsChanged();

  private:
    void refreshSessions();
    void refreshRemembered();
    void refreshDiscovered();
    void refreshBindings();

    net::WifiConnectionManager* wifi_;
    net::ConnectionHub* hub_;

    // Owned here; the composer only reads them.
    arch::Observable<std::vector<SessionSnapshot>> sessions_;
    arch::Observable<std::vector<RememberedSnapshot>> remembered_;
    arch::Observable<std::vector<std::string>> discoveredIds_;
    arch::Observable<std::vector<Binding>> bindings_;
    arch::Observable<std::vector<std::string>> staleIds_;

    std::unique_ptr<ConnectionsComposer> composer_;
    arch::Observable<std::vector<ConnectionRow>>::Subscription composerSub_;
};

} // namespace dish::composer
