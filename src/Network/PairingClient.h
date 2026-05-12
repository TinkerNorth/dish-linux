// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QString>

#include <variant>

namespace dish::net {

// Blocking TCP pair handshake on :9878. Mirrors dish-mac/Network/PairingClient.swift
// and satellite_jni.cpp::pair. Single JSON request line, single JSON response.
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

    static models::PairResponse pair(const QString& ip, int port, const QString& deviceId,
                                     const QString& deviceName, const QString& pin);
};

} // namespace dish::net
