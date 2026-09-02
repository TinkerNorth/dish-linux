// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SdlAudioGateway — the SDL2 (pipewire/pulse/alsa, whichever backend SDL
// picks) implementation of AudioDeviceGateway, and the OWNER of
// SDL_INIT_AUDIO's lifecycle.
//
// Deliberately not SDLGamepadBridge: SDL subsystems are ref-counted
// independently, and the bridge quits GAMECONTROLLER|JOYSTICK on stop() — a
// gamepad re-init (settings change, teardown ordering) must not take a live
// microphone or speaker stream down with it. The bridge still PUMPS the audio
// hotplug events, because it owns the one SDL event loop, and forwards them as
// a signal; this class never touches the event queue.
//
// The audio subsystem is initialized eagerly in the constructor so hotplug
// events flow from app start, and a failed init (headless CI, no audio stack)
// degrades to empty enumerations and refused opens — everything downstream
// then reads "no routes" and stays off, which is the conservative posture the
// whole feature takes.

#pragma once

#include "source/audio/AudioDeviceGateway.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace dish::source::audio {

class SdlAudioGateway : public AudioDeviceGateway {
  public:
    SdlAudioGateway();
    ~SdlAudioGateway() override;

    SdlAudioGateway(const SdlAudioGateway&) = delete;
    SdlAudioGateway& operator=(const SdlAudioGateway&) = delete;
    SdlAudioGateway(SdlAudioGateway&&) = delete;
    SdlAudioGateway& operator=(SdlAudioGateway&&) = delete;

    std::vector<std::string> captureDeviceNames() override;
    std::vector<std::string> playbackDeviceNames() override;
    int openCapture(const std::string& deviceName,
                    std::function<void(const std::int16_t*, std::size_t)> onSamples) override;
    void closeCapture(int handle) override;
    int openPlayback(const std::string& deviceName) override;
    bool queuePlayback(int handle, const std::int16_t* samples, std::size_t sampleCount) override;
    std::size_t queuedPlaybackBytes(int handle) override;
    void resumePlayback(int handle) override;
    void closePlayback(int handle) override;

  private:
    // The capture callback's home, heap-held so SDL's userdata pointer stays
    // valid for the device's whole life whatever the map does.
    struct CaptureHolder {
        std::function<void(const std::int16_t*, std::size_t)> onSamples;
    };

    // SDL's C callback shape (SDLCALL is plain cdecl here); a static member so
    // it can name the private holder type.
    static void captureCallback(void* userdata, std::uint8_t* stream, int len);

    bool audioReady_ = false;

    // handle -> SDL device id (+ holder for captures). Guarded by mtx_; the
    // capture callback itself never touches the maps (its holder is reached
    // through SDL's userdata), so the audio thread and these stay apart.
    std::mutex mtx_;
    int nextHandle_ = 1;
    std::map<int, std::uint32_t> captureDevices_;
    std::map<int, std::unique_ptr<CaptureHolder>> captureHolders_;
    std::map<int, std::uint32_t> playbackDevices_;
};

} // namespace dish::source::audio
