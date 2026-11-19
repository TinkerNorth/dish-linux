// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The opens the audio stack refused, kept per slot and lane at the endpoint
// each was refused at. The descriptor's audio caps read it (a cap is a promise
// the host streams on, so an endpoint this machine cannot open is not
// promised) and the engines read it too, so a refused endpoint is not asked
// again on every reconcile under a claim that says it plays. A refusal stands
// only against the very endpoint it was earned at: a route that moved to
// another device or another width is a new attempt, and a device that departed
// enumerates afresh when it returns, so its refusals leave with it. Each
// engine owns one, so a fresh engine starts with every attempt open.
//
// The sibling of dish-android's PadAudioRefusalStore and dish-mac's
// RefusedEndpoint, with the microphone as a lane of its own because this
// client's capture engine opens by name too.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dish::audio {

// One direction of one pad's audio function, as the descriptor claims it.
enum class AudioLane : std::uint8_t { Mic, Speaker, Haptic };

// Where an open was tried: the device by the name it opens by, at the channel
// count asked for.
struct AudioEndpoint {
    std::string deviceName;
    int channels = 0;

    bool operator==(const AudioEndpoint& o) const {
        return deviceName == o.deviceName && channels == o.channels;
    }
    bool operator!=(const AudioEndpoint& o) const { return !(*this == o); }
};

// The slots whose standing refusal appeared, moved or lapsed in one pass, each
// named once: what a caller re-folds the descriptor for.
using RefusedSlots = std::set<std::string>;

class AudioRefusalLedger {
  public:
    // True when the ledger moved: a lane refused for the first time, or refused
    // at another endpoint than the one on record.
    bool record(const std::string& slotId, AudioLane lane, const AudioEndpoint& endpoint) {
        const Key key{slotId, lane};
        const auto it = refusals_.find(key);
        const bool alreadyOnRecord = it != refusals_.end() && it->second == endpoint;
        if (alreadyOnRecord) { return false; }
        refusals_[key] = endpoint;
        return true;
    }

    // Spends the lane's refusal whatever endpoint it was at; true when one stood.
    bool lift(const std::string& slotId, AudioLane lane) {
        return refusals_.erase(Key{slotId, lane}) > 0;
    }

    // Whether the lane is refused at `endpoint`: on record, and at that very
    // endpoint.
    bool stands(const std::string& slotId, AudioLane lane, const AudioEndpoint& endpoint) const {
        const auto it = refusals_.find(Key{slotId, lane});
        return it != refusals_.end() && it->second == endpoint;
    }

    // A device that is no longer present takes its refusals with it; the slots
    // that lost one are named so their descriptors can be re-folded.
    RefusedSlots pruneAbsent(const std::vector<std::string>& presentDevices) {
        RefusedSlots lapsed;
        for (auto it = refusals_.begin(); it != refusals_.end();) {
            const bool present = isPresent(it->second.deviceName, presentDevices);
            if (present) {
                ++it;
                continue;
            }
            lapsed.insert(it->first.first);
            it = refusals_.erase(it);
        }
        return lapsed;
    }

    bool empty() const { return refusals_.empty(); }

  private:
    using Key = std::pair<std::string, AudioLane>;

    static bool isPresent(const std::string& deviceName,
                          const std::vector<std::string>& presentDevices) {
        for (const auto& present : presentDevices) {
            if (present == deviceName) { return true; }
        }
        return false;
    }

    std::map<Key, AudioEndpoint> refusals_;
};

} // namespace dish::audio
