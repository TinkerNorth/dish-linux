// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QString>

namespace dish::net {

// Blocking TCP pair handshake on :9878. Mirrors dish-mac/Network/PairingClient.swift
// and satellite_jni.cpp::pair. Single JSON request line, single JSON response.
class PairingClient {
public:
    static models::PairResponse pair(const QString& ip, int port, const QString& deviceId,
                                     const QString& deviceName, const QString& pin);
};

}  // namespace dish::net
