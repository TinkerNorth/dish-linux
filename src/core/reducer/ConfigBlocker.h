// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which full-screen interruption the binding-config flow shows. Emits kind tokens
// and the raw host label, never localized text; the view maps tokens to strings.

#pragma once

#include "core/reducer/SatelliteLinkState.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dish::reducer {

// The two states that actually stream.
inline bool isLiveLink(UiLinkState s) {
    return s == UiLinkState::Connected || s == UiLinkState::Unstable;
}

enum class ConfigBlockerKind { None, InputLost, HostLost, HostUnsteady };

struct ConfigBlocker {
    ConfigBlockerKind kind = ConfigBlockerKind::None;
    std::string hostLabel;     // HostLost only
    bool reconnecting = false; // HostLost only; the view shows a spinner instead
                               // of a reconnect action while true

    bool operator==(const ConfigBlocker& o) const {
        return kind == o.kind && hostLabel == o.hostLabel && reconnecting == o.reconnecting;
    }
    bool operator!=(const ConfigBlocker& o) const { return !(*this == o); }
};

// Plain values so the caller can project from whatever summary type it holds.
struct BlockerHostRow {
    std::string id;
    std::string label;
    UiLinkState live = UiLinkState::Saved;
};

// Strict precedence: nothing shows until `loaded`, so there is no flash of
// blocker; a missing controller blocks everything; no host selected means no host
// blocker, because the picker itself is the UI. An unsteady dismissal is per host
// id and never suppresses a real loss. `knownHostLabels` is the remembered
// id-to-label map a forgotten host's blocker falls back to.
inline ConfigBlocker configBlockerFor(bool loaded, bool controllerPresent,
                                      const std::optional<std::string>& hostId,
                                      const std::vector<BlockerHostRow>& connections,
                                      const std::map<std::string, std::string>& knownHostLabels,
                                      const std::set<std::string>& dismissedUnsteadyHostIds) {
    if (!loaded) { return {}; }
    if (!controllerPresent) { return ConfigBlocker{ConfigBlockerKind::InputLost, {}, false}; }
    if (!hostId.has_value()) { return {}; }

    const BlockerHostRow* summary = nullptr;
    for (const auto& row : connections) {
        if (row.id == *hostId) {
            summary = &row;
            break;
        }
    }

    if (summary == nullptr || !isLiveLink(summary->live)) {
        ConfigBlocker lost;
        lost.kind = ConfigBlockerKind::HostLost;
        if (summary != nullptr) {
            lost.hostLabel = summary->label;
        } else {
            const auto it = knownHostLabels.find(*hostId);
            if (it != knownHostLabels.end()) { lost.hostLabel = it->second; }
        }
        lost.reconnecting = summary != nullptr && summary->live == UiLinkState::Connecting;
        return lost;
    }
    if (summary->live == UiLinkState::Unstable &&
        dismissedUnsteadyHostIds.find(*hostId) == dismissedUnsteadyHostIds.end()) {
        return ConfigBlocker{ConfigBlockerKind::HostUnsteady, {}, false};
    }
    return {};
}

} // namespace dish::reducer
