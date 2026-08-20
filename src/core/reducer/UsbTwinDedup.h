// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Twin-dedup arbitration. A HID pad such as a DualSense is visible to both
// SDL and the raw-HID gateway; if USB-direct claims it while SDL also
// reads it, the satellite receives the same input twice. This decides which
// routed device ids the bridge must suppress so a pad streams over exactly one
// path. When a claim fails or the device detaches the synthetic disappears, the
// set recomputes empty and SDL resumes with no replug.
//
// Pairing is count-based per (vid, pid), which is what makes two identical pads
// correct: claiming one of two DualSenses hides exactly one routed twin. Xbox
// pads never appear as synthetics, since xpad publishes no hidraw node, so they
// always stay on SDL.
//
// The claim can only ever hold a USB device (the gateway refuses Bluetooth HID
// paths), so a Bluetooth routed instance is never the claimed device's twin:
// hiding one never cures a double stream, it can only silence a wireless pad.
// USB twins are therefore hidden first, and a Bluetooth instance only as the
// model's last remaining twin — the single-pad dual-presence case, where SDL's
// serial dedup can keep the BT-flagged instance while the claim reads the
// pad's USB link.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dish::reducer {

// A USB-direct claim that is actively streaming. A vid or pid of 0 is
// unidentified and matches nothing, so it never hides a real pad.
struct SyntheticTwin {
    int vendorId = 0;
    int productId = 0;
};

// `id` is the bridge's stable "sdl:<iid>" id. `disconnecting` marks a device in
// grace-period teardown; among same-transport twins those are hidden first so a
// live twin keeps streaming. `bluetooth` is the attach-time transport class.
struct RoutedDevice {
    std::string id;
    int vendorId = 0;
    int productId = 0;
    bool disconnecting = false;
    bool bluetooth = false;
};

inline std::int64_t twinModelKey(int vendorId, int productId) {
    return (static_cast<std::int64_t>(vendorId) << 32) |
           (static_cast<std::int64_t>(productId) & 0xFFFFFFFF);
}

inline std::set<std::string> suppressedRoutedIds(const std::vector<SyntheticTwin>& synthetics,
                                                 const std::vector<RoutedDevice>& routed) {
    std::map<std::int64_t, int> syntheticModelCounts;
    for (const auto& s : synthetics) {
        if (s.vendorId == 0 || s.productId == 0) { continue; }
        ++syntheticModelCounts[twinModelKey(s.vendorId, s.productId)];
    }
    std::set<std::string> hidden;
    if (syntheticModelCounts.empty()) { return hidden; }

    // Input order is preserved so the stable-sort tie-break stays deterministic.
    std::map<std::int64_t, std::vector<RoutedDevice>> byModel;
    for (const auto& r : routed) {
        const std::int64_t key = twinModelKey(r.vendorId, r.productId);
        if (syntheticModelCounts.find(key) == syntheticModelCounts.end()) { continue; }
        byModel[key].push_back(r);
    }
    for (auto& [key, group] : byModel) {
        // Hide priority: USB before Bluetooth (transport outranks disconnecting
        // — only a USB twin can be duplicating the claim's stream), then
        // disconnecting before live within a transport. Stable, so input order
        // keeps the tie-break deterministic.
        const auto rank = [](const RoutedDevice& d) {
            return (d.bluetooth ? 2 : 0) + (d.disconnecting ? 0 : 1);
        };
        std::stable_sort(
            group.begin(), group.end(),
            [&rank](const RoutedDevice& a, const RoutedDevice& b) { return rank(a) < rank(b); });
        const int take = syntheticModelCounts[key];
        for (int i = 0; i < take && i < static_cast<int>(group.size()); ++i) {
            hidden.insert(group[static_cast<std::size_t>(i)].id);
        }
    }
    return hidden;
}

} // namespace dish::reducer
