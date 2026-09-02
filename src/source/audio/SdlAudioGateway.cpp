// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/audio/SdlAudioGateway.h"

#include "core/model/Protocol.h"

#include <QtGlobal>

#include <SDL.h>

#include <cstring>

namespace dish::source::audio {

namespace {

// One SDL period, in per-channel sample frames. The wire's own window, so on
// the capture side the common case is one callback per wire frame and the
// windower's fast path applies; the playout queue smooths whatever the device
// actually granted.
constexpr int kPeriodSamples = dish::proto::kAudioFrameSamples;

} // namespace

// SDL's capture callback, on the device's audio thread. The format was pinned
// with allowed_changes = 0, so the stream is whole int16 mono samples.
void SdlAudioGateway::captureCallback(void* userdata, std::uint8_t* stream, int len) {
    auto* holder = static_cast<CaptureHolder*>(userdata);
    if (holder == nullptr || stream == nullptr || len <= 0) { return; }
    holder->onSamples(reinterpret_cast<const std::int16_t*>(stream),
                      static_cast<std::size_t>(len) / sizeof(std::int16_t));
}

SdlAudioGateway::SdlAudioGateway() {
    // Ref-counted per subsystem, so this coexists with the bridge's
    // GAMECONTROLLER|JOYSTICK init on its own thread. Failure is tolerated:
    // every entry point below degrades to "nothing there".
    audioReady_ = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
    if (!audioReady_) {
        qWarning("SdlAudioGateway: SDL audio unavailable (%s); controller audio stays off",
                 SDL_GetError());
    }
}

SdlAudioGateway::~SdlAudioGateway() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& [handle, dev] : captureDevices_) { SDL_CloseAudioDevice(dev); }
        for (const auto& [handle, dev] : playbackDevices_) { SDL_CloseAudioDevice(dev); }
        captureDevices_.clear();
        captureHolders_.clear();
        playbackDevices_.clear();
    }
    if (audioReady_) { SDL_QuitSubSystem(SDL_INIT_AUDIO); }
}

std::vector<std::string> SdlAudioGateway::captureDeviceNames() {
    if (!audioReady_) { return {}; }
    std::vector<std::string> out;
    const int n = SDL_GetNumAudioDevices(SDL_TRUE);
    for (int i = 0; i < n; i++) {
        const char* name = SDL_GetAudioDeviceName(i, SDL_TRUE);
        if (name != nullptr) { out.emplace_back(name); }
    }
    return out;
}

std::vector<std::string> SdlAudioGateway::playbackDeviceNames() {
    if (!audioReady_) { return {}; }
    std::vector<std::string> out;
    const int n = SDL_GetNumAudioDevices(SDL_FALSE);
    for (int i = 0; i < n; i++) {
        const char* name = SDL_GetAudioDeviceName(i, SDL_FALSE);
        if (name != nullptr) { out.emplace_back(name); }
    }
    return out;
}

int SdlAudioGateway::openCapture(const std::string& deviceName,
                                 std::function<void(const std::int16_t*, std::size_t)> onSamples) {
    if (!audioReady_ || deviceName.empty() || !onSamples) { return kNoAudioDevice; }
    auto holder = std::make_unique<CaptureHolder>();
    holder->onSamples = std::move(onSamples);

    SDL_AudioSpec want{};
    want.freq = dish::proto::kAudioSampleRateHz;
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(dish::proto::kAudioMicChannels);
    want.samples = static_cast<Uint16>(kPeriodSamples);
    want.callback = &SdlAudioGateway::captureCallback;
    want.userdata = holder.get();
    SDL_AudioSpec have{};
    // allowed_changes = 0: SDL converts whatever the endpoint really runs to
    // the wire format, so the callback only ever sees 48 kHz mono S16 and no
    // resampler lives in this repo.
    const SDL_AudioDeviceID dev =
        SDL_OpenAudioDevice(deviceName.c_str(), SDL_TRUE, &want, &have, 0);
    if (dev == 0) {
        qWarning("SdlAudioGateway: capture open failed for '%s': %s", deviceName.c_str(),
                 SDL_GetError());
        return kNoAudioDevice;
    }
    int handle = kNoAudioDevice;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        handle = nextHandle_++;
        captureDevices_[handle] = dev;
        captureHolders_[handle] = std::move(holder);
    }
    SDL_PauseAudioDevice(dev, 0);
    return handle;
}

void SdlAudioGateway::closeCapture(int handle) {
    std::uint32_t dev = 0;
    std::unique_ptr<CaptureHolder> holder;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = captureDevices_.find(handle);
        if (it == captureDevices_.end()) { return; }
        dev = it->second;
        captureDevices_.erase(it);
        const auto hit = captureHolders_.find(handle);
        if (hit != captureHolders_.end()) {
            holder = std::move(hit->second);
            captureHolders_.erase(hit);
        }
    }
    // SDL_CloseAudioDevice waits out a callback in flight, which is the join
    // the interface promises: after this returns, no more samples. The holder
    // is destroyed after, so the callback never sees a dead function.
    SDL_CloseAudioDevice(dev);
}

int SdlAudioGateway::openPlayback(const std::string& deviceName) {
    if (!audioReady_ || deviceName.empty()) { return kNoAudioDevice; }
    SDL_AudioSpec want{};
    want.freq = dish::proto::kAudioSampleRateHz;
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(dish::proto::kAudioSpeakerChannels);
    want.samples = static_cast<Uint16>(kPeriodSamples);
    want.callback = nullptr; // queued mode: the engine pushes whole frames
    SDL_AudioSpec have{};
    const SDL_AudioDeviceID dev =
        SDL_OpenAudioDevice(deviceName.c_str(), SDL_FALSE, &want, &have, 0);
    if (dev == 0) {
        qWarning("SdlAudioGateway: playback open failed for '%s': %s", deviceName.c_str(),
                 SDL_GetError());
        return kNoAudioDevice;
    }
    // Opened paused (SDL's default), per the interface: the engine resumes once
    // the start cushion is queued.
    int handle = kNoAudioDevice;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        handle = nextHandle_++;
        playbackDevices_[handle] = dev;
    }
    return handle;
}

bool SdlAudioGateway::queuePlayback(int handle, const std::int16_t* samples,
                                    std::size_t sampleCount) {
    if (samples == nullptr || sampleCount == 0) { return false; }
    std::uint32_t dev = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = playbackDevices_.find(handle);
        if (it == playbackDevices_.end()) { return false; }
        dev = it->second;
    }
    return SDL_QueueAudio(dev, samples, static_cast<Uint32>(sampleCount * sizeof(std::int16_t))) ==
           0;
}

std::size_t SdlAudioGateway::queuedPlaybackBytes(int handle) {
    std::uint32_t dev = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = playbackDevices_.find(handle);
        if (it == playbackDevices_.end()) { return 0; }
        dev = it->second;
    }
    return SDL_GetQueuedAudioSize(dev);
}

void SdlAudioGateway::resumePlayback(int handle) {
    std::uint32_t dev = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = playbackDevices_.find(handle);
        if (it == playbackDevices_.end()) { return; }
        dev = it->second;
    }
    SDL_PauseAudioDevice(dev, 0);
}

void SdlAudioGateway::closePlayback(int handle) {
    std::uint32_t dev = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = playbackDevices_.find(handle);
        if (it == playbackDevices_.end()) { return; }
        dev = it->second;
        playbackDevices_.erase(it);
    }
    SDL_CloseAudioDevice(dev);
}

} // namespace dish::source::audio
