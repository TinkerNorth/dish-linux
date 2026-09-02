// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A scriptable AudioDeviceGateway for the engine suites: capture callbacks are
// driven by the test, playback queues are byte-counted maps, and every open /
// close is recorded so the privacy claim ("closed means closed") is an
// assertion rather than a hope.

#pragma once

#include "source/audio/AudioDeviceGateway.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace dish::test {

class FakeAudioGateway : public dish::source::audio::AudioDeviceGateway {
  public:
    std::vector<std::string> captureNames;
    std::vector<std::string> playbackNames;
    // Names that refuse to open.
    std::vector<std::string> refuse;

    struct Capture {
        std::string name;
        std::function<void(const std::int16_t*, std::size_t)> onSamples;
        bool open = true;
    };
    struct Playback {
        std::string name;
        std::vector<std::int16_t> queued; // everything ever queued, in order
        std::size_t queuedBytes = 0;      // the "unplayed" figure the engine reads
        bool open = true;
        bool resumed = false;
    };

    std::map<int, Capture> captures;
    std::map<int, Playback> playbacks;
    int closes = 0;

    std::vector<std::string> captureDeviceNames() override { return captureNames; }
    std::vector<std::string> playbackDeviceNames() override { return playbackNames; }

    int openCapture(const std::string& deviceName,
                    std::function<void(const std::int16_t*, std::size_t)> onSamples) override {
        for (const auto& r : refuse) {
            if (r == deviceName) { return dish::source::audio::kNoAudioDevice; }
        }
        const int handle = nextHandle_++;
        captures[handle] = Capture{deviceName, std::move(onSamples), true};
        return handle;
    }

    void closeCapture(int handle) override {
        const auto it = captures.find(handle);
        if (it == captures.end() || !it->second.open) { return; }
        it->second.open = false;
        it->second.onSamples = {};
        closes++;
    }

    int openPlayback(const std::string& deviceName) override {
        for (const auto& r : refuse) {
            if (r == deviceName) { return dish::source::audio::kNoAudioDevice; }
        }
        const int handle = nextHandle_++;
        playbacks[handle] = Playback{deviceName, {}, 0, true, false};
        return handle;
    }

    bool queuePlayback(int handle, const std::int16_t* samples, std::size_t sampleCount) override {
        const auto it = playbacks.find(handle);
        if (it == playbacks.end() || !it->second.open) { return false; }
        it->second.queued.insert(it->second.queued.end(), samples, samples + sampleCount);
        it->second.queuedBytes += sampleCount * sizeof(std::int16_t);
        return true;
    }

    std::size_t queuedPlaybackBytes(int handle) override {
        const auto it = playbacks.find(handle);
        return it != playbacks.end() ? it->second.queuedBytes : 0;
    }

    void resumePlayback(int handle) override {
        const auto it = playbacks.find(handle);
        if (it != playbacks.end()) { it->second.resumed = true; }
    }

    void closePlayback(int handle) override {
        const auto it = playbacks.find(handle);
        if (it == playbacks.end() || !it->second.open) { return; }
        it->second.open = false;
        closes++;
    }

    // Test conveniences.
    Capture* captureFor(const std::string& deviceName) {
        for (auto& [handle, c] : captures) {
            if (c.open && c.name == deviceName) { return &c; }
        }
        return nullptr;
    }
    Playback* playbackFor(const std::string& deviceName) {
        for (auto& [handle, p] : playbacks) {
            if (p.open && p.name == deviceName) { return &p; }
        }
        return nullptr;
    }
    int openCaptureCount() const {
        int n = 0;
        for (const auto& [handle, c] : captures) {
            if (c.open) { n++; }
        }
        return n;
    }
    int openPlaybackCount() const {
        int n = 0;
        for (const auto& [handle, p] : playbacks) {
            if (p.open) { n++; }
        }
        return n;
    }
    // Simulate the device consuming everything queued so far.
    void drain(int handle) { playbacks.at(handle).queuedBytes = 0; }

  private:
    int nextHandle_ = 1;
};

} // namespace dish::test
