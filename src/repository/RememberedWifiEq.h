// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Field-wise value-equality for models::RememberedWifi, declared in
// dish::models so ADL finds it. Backs the RepositoryContract round-trip checks
// and ConnectionStore's "skip the write when the row is unchanged" guard.

#pragma once

#include "Models/Models.h"

namespace dish::models {

inline bool operator==(const RememberedWifi& a, const RememberedWifi& b) {
    return a.id == b.id && a.name == b.name && a.ip == b.ip && a.udpPort == b.udpPort &&
           a.pairPort == b.pairPort && a.httpPort == b.httpPort && a.machineId == b.machineId;
}

inline bool operator!=(const RememberedWifi& a, const RememberedWifi& b) { return !(a == b); }

} // namespace dish::models
