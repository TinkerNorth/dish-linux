// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Drives the 5-phase Moonlight PIN pairing over MoonlightHttp: generates the
// PIN and the handshake's random material, walks core/moonlight's
// PairingSession through the phase responses, and finishes with the HTTPS
// pairchallenge that proves the secure channel works end to end. One attempt
// at a time; a second start() cancels the first.
//
// The PIN is shown by THIS client and typed into the host's UI (for example
// Sunshine's web PIN page) — the reverse of the satellite flow's direction.

#pragma once

#include "core/moonlight/MoonlightPairing.h"
#include "source/moonlight/MoonlightHttp.h"

#include <QObject>
#include <QString>

#include <memory>

namespace dish::source::moon {

class MoonlightPairingFlow : public QObject {
    Q_OBJECT
  public:
    explicit MoonlightPairingFlow(MoonlightHttp* http, QObject* parent = nullptr);

    // Begins pairing against `address:httpPort` (phases 1-4, plain HTTP) and
    // `httpsPort` (phase 5). Emits pinReady() immediately with the PIN the
    // user must type into the host, then finished() once the host answers.
    void start(const QString& hostUuid, const QString& address, int httpPort, int httpsPort,
               const QString& clientCertPem, const QString& clientKeyPem,
               const QString& deviceName);

    void cancel();

    bool active() const { return active_; }
    QString pin() const { return pin_; }
    QString hostUuid() const { return hostUuid_; }

  signals:
    // The 4-digit PIN to show. Fired from start().
    void pinReady(const QString& pin);

    // `reasonToken` on failure: "unreachable" | "wrongPin" | "declined" |
    // "crypto". On success `serverCertPem` is the pairing anchor to persist.
    void finished(bool ok, const QString& reasonToken, const QString& serverCertPem);

  private:
    void phase1();
    void phase2();
    void phase3(const QString& serverChallengeResp);
    void phase4();
    void phase5();
    void fail(const QString& reasonToken);
    // True while this reply still belongs to the current attempt.
    bool current(quint64 attempt) const { return active_ && attempt == attempt_; }

    MoonlightHttp* http_;
    std::unique_ptr<moonpair::PairingSession> session_;

    bool active_ = false;
    quint64 attempt_ = 0; // stale-reply guard across cancel/restart
    QString hostUuid_;
    QString address_;
    int httpPort_ = 0;
    int httpsPort_ = 0;
    QString deviceName_;
    QString pin_;
};

} // namespace dish::source::moon
