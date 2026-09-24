// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"
#include "core/reducer/RestOutcome.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <utility>
#include <variant>

namespace dish::net {

// Blocking pairing against the satellite's HTTPS server (:9443, self-signed,
// TOFU-pinned). The exchange carries the sharedKey exactly once, so it gets the
// same pin gate as the session REST path.
//
// Each call drives a nested QEventLoop around the async QNetworkAccessManager to
// keep the API synchronous, so it MUST be invoked from a worker thread and never
// from the UI thread.
//
// An instance carries the TOFU gate it runs, and is cheap to copy: a caller hands
// each worker its own copy rather than every worker reading one process-wide
// verifier. That process-wide slot was a singleton with a lifetime hazard in it -
// the verifier closes over a pin store by reference, and nothing tied the slot's
// contents to that store's lifetime.
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
    struct IdentityChanged {}; // TOFU pin mismatch — terminal, forget and pair again
    struct Unreachable {
        QString message;
    };
    using Outcome =
        std::variant<Success, Pending, AuthRequired, VersionMismatch, Unreachable, IdentityChanged>;

    // A round-trip plus the one fact the JSON body cannot carry: the TOFU gate
    // saw a CHANGED cert and aborted before any PIN or proof transited.
    struct Reply {
        models::PairResponse response;
        bool pinMismatch = false;
    };

    static Outcome classify(const models::PairResponse& response, bool pinMismatch = false);

    // Called on the TLS `encrypted` edge with the peer cert DER; returning false
    // aborts before any payload transits. Pairing is the pin-on-first-use moment,
    // so the first pair pins and every later pair must match. Keyed by host,
    // sharing the pin store with HTTPClient. An empty verifier accepts, which only
    // a test should ever construct. `pinMismatch` is set only for a CHANGED cert,
    // so an identity change is not read as a dead link.
    using PinVerifier =
        std::function<bool(const QString& host, const QByteArray& certDer, bool& pinMismatch)>;

    explicit PairingClient(PinVerifier verify) : verify_(std::move(verify)) {}

    // Path A (operator `pin`) and Path B (client-shown `clientPin`, which answers
    // Pending and is then polled). Both fields always ride in the body, empty when
    // unused; the server tries a valid `pin` first.
    Reply pair(const QString& ip, int port, const QString& deviceId, const QString& deviceName,
               const QString& pin, const QString& clientPin = QString()) const;

    Reply pairStatus(const QString& ip, int port, const QString& deviceId) const;

  private:
    PinVerifier verify_;
};

} // namespace dish::net
