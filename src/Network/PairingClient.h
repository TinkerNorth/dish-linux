// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"
#include "core/reducer/RestOutcome.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <variant>

namespace dish::net {

// Blocking pairing against the satellite's HTTPS server (:9443, self-signed,
// TOFU-pinned). The exchange carries the sharedKey exactly once, so it gets the
// same pin gate as the session REST path.
//
// Each call drives a nested QEventLoop around the async QNetworkAccessManager to
// keep the API synchronous, so it MUST be invoked from a worker thread and never
// from the UI thread.
class PairingClient {
  public:
    // Arms map 1:1 onto reducer::PairVerdict; the success arm carries the shared
    // key directly.
    struct Success {
        QString sharedKeyHex;
    };
    struct Pending {};         // Path B accepted — poll /api/pair/status
    struct AuthRequired {};    // reachable, no key — first-time pair, or it forgot us
    struct VersionMismatch {}; // 409 — protocol skew, terminal
    struct Unreachable {
        QString message;
    };
    using Outcome = std::variant<Success, Pending, AuthRequired, VersionMismatch, Unreachable>;

    static Outcome classify(const models::PairResponse& response);

    // Path A (operator `pin`) and Path B (client-shown `clientPin`, which answers
    // Pending and is then polled). Both fields always ride in the body, empty when
    // unused; the server tries a valid `pin` first.
    static models::PairResponse pair(const QString& ip, int port, const QString& deviceId,
                                     const QString& deviceName, const QString& pin,
                                     const QString& clientPin = QString());

    // Proves possession of the CURRENT key to get a fresh one. A failed proof
    // falls through to the PIN paths server-side, so it degrades to a fresh
    // attempt rather than an error.
    static models::PairResponse rotateKey(const QString& ip, int port, const QString& deviceId,
                                          const QString& deviceName, const QString& hmacProof);

    static models::PairResponse pairStatus(const QString& ip, int port, const QString& deviceId);

    // Called on the TLS `encrypted` edge with the peer cert DER; returning false
    // aborts before any payload transits. Pairing is the pin-on-first-use moment,
    // so the first pair pins and every later pair or rotation must match. Keyed by
    // host, sharing the pin store with HTTPClient. Unset means accept, which only
    // happens in tests and before a manager wires the store.
    using PinVerifier = std::function<bool(const QString& host, const QByteArray& certDer)>;
    static void setPinVerifier(PinVerifier verifier);

  private:
    static PinVerifier& pinVerifier();
};

} // namespace dish::net
