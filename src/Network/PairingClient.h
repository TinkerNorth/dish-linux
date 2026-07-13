// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <variant>

namespace dish::net {

// Blocking pair handshake. The satellite exposes pairing as POST /api/pair on
// its HTTPS client server (:9443). Mirrors dish-windows/Network/PairingClient
// and satellite_jni.cpp::pair. Single JSON request, single JSON response; the
// body carries protocolVersion so a future incompatible server can 409.
//
// TLS trust is TOFU cert-pinning, not CA validation: the optional verifier is
// handed the peer cert's DER bytes once the handshake completes and may abort
// the exchange by returning false (see HTTPClient::PinVerifier — same seam,
// same composition against ConnectionStore's pin registry; pair() runs on a
// QtConcurrent worker, which is why the store's pin accessors are the one
// mutex-guarded surface it has).
class PairingClient {
  public:
    // Classification of a PairResponse — mirrors PairingClient.Outcome on
    // dish-mac and the unreachable-vs-auth split introduced for dish-android
    // PR #43. The manager fans the variant out to either an error toast, a
    // PIN dialog, or the openSession path. Tagged union (variant) keeps the
    // arms exhaustive and the success arm carries the shared key directly.
    struct Success {
        QString sharedKeyHex;
    };
    struct AuthRequired {};
    struct Unreachable {
        QString message;
    };
    using Outcome = std::variant<Success, AuthRequired, Unreachable>;

    // Pure classifier — driven only by fields on the response so it's
    // trivially unit-testable.
    static Outcome classify(const models::PairResponse& response);

    using PinVerifier = std::function<bool(const QString& host, const QByteArray& certDer)>;

    static models::PairResponse pair(const QString& ip, int port, const QString& deviceId,
                                     const QString& deviceName, const QString& pin,
                                     const PinVerifier& verifier = {});
};

} // namespace dish::net
