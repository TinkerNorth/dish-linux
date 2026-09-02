// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Matches a Direct-claimed pad to its own audio endpoints, conservatively. The
// Windows sibling of dish-android's PadAudioMatcher, stance for stance.
//
// A DualSense (or DualShock 4 v2) is a composite USB device: the HID interface
// this client claims plus a USB Audio Class function the OS keeps. That audio
// function's endpoints surface as ordinary audio devices, which SDL enumerates
// by NAME — and nothing in that name is a vendor:product. The one fact both
// sides genuinely share is the pad's iProduct string ("Wireless Controller"),
// which the hidraw enumeration reads on the HID side and the audio stack
// embeds in the device name on the audio side. The exact wrapping differs by
// platform — Windows/WASAPI decorates it ("Speakers (Wireless Controller)"),
// pipewire/alsa node descriptions carry the iProduct itself or append a
// profile suffix ("Wireless Controller Analog Stereo") — which is why the
// match is CONTAINMENT, not equality: an endpoint belongs to a pad when the
// endpoint's name contains the pad's product string.
//
// Because it is only a name, every ambiguity resolves to "no route", never to
// a guess:
//
//  - The pad's family must carry a USB audio function at all
//    (usbparse::parserHasUsbAudio). A name alone would let an unrelated USB
//    audio dongle lend its endpoints to a pad that has none.
//  - The product string must identify exactly one claimed pad. Two DualSenses
//    (or a DualSense next to a DualShock 4, which shares the string) are
//    indistinguishable here, and routing a slot to the wrong pad's speaker is
//    worse than not routing it.
//  - An endpoint whose name contains more than one pad's string belongs to
//    nobody, and a pad whose string matches more than one endpoint in a
//    direction gets no route in that direction, for the same reason.
//
// A pad that resolves to nothing simply advertises neither cap, which is the
// honest answer for a pad whose audio function this machine cannot confidently
// name. Qt-free so dish-linux can reuse it as-is (pipewire/alsa names embed
// the product string the same way).

#pragma once

#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace dish::audio {

// One Direct-claimed pad, flattened out of the claim table so the rule stays
// pure. `productName` is the HID product string the gateway enumerated.
struct AudioPadCandidate {
    int vendorId = 0;
    int productId = 0;
    std::string productName;
    // Whether the FAMILY carries a USB Audio Class function (DualSense, DS4).
    // The gate that keeps a lookalike name from lending endpoints to a pad
    // that has none.
    bool hasAudioFunction = false;
};

// Which of a pad's own endpoints this machine can name, per direction, and the
// exact SDL device names to open them by. A flag is set ONLY alongside its
// name: a claim the resolver cannot point at is a claim it must not make.
struct PadAudioRoute {
    bool microphone = false;
    bool speaker = false;
    std::string captureDeviceName;
    std::string playbackDeviceName;

    bool operator==(const PadAudioRoute& o) const {
        return microphone == o.microphone && speaker == o.speaker &&
               captureDeviceName == o.captureDeviceName &&
               playbackDeviceName == o.playbackDeviceName;
    }
    bool operator!=(const PadAudioRoute& o) const { return !(*this == o); }
};

// Same vendor:product packing the USB path keys everything on.
inline int padAudioKey(int vendorId, int productId) {
    return (vendorId << 16) | (productId & 0xFFFF);
}

namespace detail {

inline std::string trimmed(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) { b++; }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) { e--; }
    return s.substr(b, e - b);
}

inline char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII-case-insensitive containment. Friendly names are localized around the
// product string but the string itself comes from the same descriptor on both
// sides; folding case costs nothing and absorbs a stack that rewrites it.
inline bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || needle.size() > haystack.size()) { return false; }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); i++) {
        std::size_t j = 0;
        while (j < needle.size() && lowerAscii(haystack[i + j]) == lowerAscii(needle[j])) { j++; }
        if (j == needle.size()) { return true; }
    }
    return false;
}

// For one direction: pad name -> the single endpoint that is unambiguously
// that pad's, or nothing. `padNames` are already trimmed and unique.
inline std::map<std::string, std::string>
matchDirection(const std::vector<std::string>& padNames,
               const std::vector<std::string>& endpointNames) {
    // An endpoint that names more than one pad belongs to nobody: "Wireless
    // Controller" inside a name would also match a hypothetical pad whose
    // whole name is a substring of another's.
    std::map<std::string, std::vector<std::string>> byPad;
    for (const auto& endpoint : endpointNames) {
        const std::string name = trimmed(endpoint);
        if (name.empty()) { continue; }
        const std::string* owner = nullptr;
        bool ambiguous = false;
        for (const auto& pad : padNames) {
            if (!containsInsensitive(name, pad)) { continue; }
            if (owner != nullptr) {
                ambiguous = true;
                break;
            }
            owner = &pad;
        }
        if (owner == nullptr || ambiguous) { continue; }
        // The ORIGINAL name, not the trimmed one: it is the open key.
        byPad[*owner].push_back(endpoint);
    }
    std::map<std::string, std::string> out;
    for (auto& [pad, endpoints] : byPad) {
        // Two endpoints answering to one pad in one direction (two identical
        // dongles enumerate identical friendly names) cannot be told apart.
        if (endpoints.size() == 1) { out[pad] = endpoints.front(); }
    }
    return out;
}

} // namespace detail

// The whole rule. Returns routes keyed by padAudioKey; a pad absent from the
// result has no route (advertise nothing). Directions resolve independently:
// a missing capture endpoint does not cost the pad its speaker.
inline std::map<int, PadAudioRoute>
resolvePadAudioRoutes(const std::vector<AudioPadCandidate>& pads,
                      const std::vector<std::string>& captureNames,
                      const std::vector<std::string>& playbackNames) {
    // Group candidates by trimmed product string; a name shared by two claimed
    // pads disqualifies both.
    std::map<std::string, std::vector<const AudioPadCandidate*>> byName;
    for (const auto& pad : pads) {
        if (!pad.hasAudioFunction) { continue; }
        const std::string name = detail::trimmed(pad.productName);
        if (name.empty()) { continue; }
        byName[name].push_back(&pad);
    }
    std::vector<std::string> padNames;
    padNames.reserve(byName.size());
    for (const auto& [name, candidates] : byName) {
        if (candidates.size() == 1) { padNames.push_back(name); }
    }

    const auto sources = detail::matchDirection(padNames, captureNames);
    const auto sinks = detail::matchDirection(padNames, playbackNames);

    std::map<int, PadAudioRoute> out;
    for (const auto& name : padNames) {
        const AudioPadCandidate* pad = byName[name].front();
        PadAudioRoute route;
        if (const auto it = sources.find(name); it != sources.end()) {
            route.microphone = true;
            route.captureDeviceName = it->second;
        }
        if (const auto it = sinks.find(name); it != sinks.end()) {
            route.speaker = true;
            route.playbackDeviceName = it->second;
        }
        if (!route.microphone && !route.speaker) { continue; }
        out[padAudioKey(pad->vendorId, pad->productId)] = route;
    }
    return out;
}

} // namespace dish::audio
