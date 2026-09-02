// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SpeakerPlayoutEngine (source/audio/SpeakerPlayoutEngine.h) against the fake
// gateway and a scriptable decoder. The load-bearing claims: frames route by
// (connection, controller index); playback starts only once the two-frame
// cushion is queued; a gap takes the FEC path with the reorder window's
// carrier; a drained queue is re-cushioned with silence; and a voice
// reconciled away is closed.

#include "source/audio/SpeakerPlayoutEngine.h"

#include "FakeAudioGateway.h"
#include "core/audio/AudioEnginePolicy.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using dish::source::audio::SpeakerPlayoutEngine;
using dish::source::audio::SpeakerVoiceTarget;
using dish::test::FakeAudioGateway;

namespace proto = dish::proto;

namespace {

constexpr std::size_t kFrame = static_cast<std::size_t>(proto::kAudioFrameSamples);
constexpr std::size_t kWindowSamples = kFrame * 2; // stereo interleaved

// Fills every decoded window with a marker so the queue contents say which
// path produced them: packet decodes carry the packet's first byte, FEC
// recoveries carry 0x7E00 + carrier length, concealment 0x7C00.
class FakeDecoder : public dish::audio::IAudioDecoder {
  public:
    struct State {
        int decodes = 0;
        int fecs = 0;
        int conceals = 0;
    };
    explicit FakeDecoder(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::size_t decode(const std::uint8_t* opus, std::size_t opusLen, std::int16_t* pcm,
                       std::size_t maxFrames) override {
        if (opus == nullptr || opusLen == 0 || maxFrames < kFrame) { return 0; }
        state_->decodes++;
        fill(pcm, static_cast<std::int16_t>(opus[0]));
        return kFrame;
    }
    std::size_t conceal(std::int16_t* pcm, std::size_t maxFrames) override {
        if (maxFrames < kFrame) { return 0; }
        state_->conceals++;
        fill(pcm, 0x7C00);
        return kFrame;
    }
    std::size_t decodeFec(const std::uint8_t* opus, std::size_t opusLen, std::int16_t* pcm,
                          std::size_t maxFrames) override {
        if (opus == nullptr || opusLen == 0) { return conceal(pcm, maxFrames); }
        if (maxFrames < kFrame) { return 0; }
        state_->fecs++;
        fill(pcm, static_cast<std::int16_t>(0x7E00 + static_cast<int>(opusLen)));
        return kFrame;
    }

  private:
    static void fill(std::int16_t* pcm, std::int16_t value) {
        for (std::size_t i = 0; i < kWindowSamples; i++) { pcm[i] = value; }
    }
    std::shared_ptr<State> state_;
};

struct Harness {
    FakeAudioGateway gateway;
    std::shared_ptr<FakeDecoder::State> decoderState = std::make_shared<FakeDecoder::State>();
    SpeakerPlayoutEngine engine{&gateway,
                                [this] { return std::make_unique<FakeDecoder>(decoderState); }};

    static SpeakerVoiceTarget voice(const std::string& conn, int idx, const std::string& slot,
                                    const std::string& device) {
        SpeakerVoiceTarget t;
        t.connectionId = conn;
        t.controllerIndex = idx;
        t.slotId = slot;
        t.playbackDeviceName = device;
        return t;
    }

    void frame(const std::string& conn, int idx, std::uint16_t seq, std::uint8_t marker) {
        const std::uint8_t opus[4] = {marker, 1, 2, 3};
        engine.deliver(conn, idx, seq, opus, sizeof(opus));
    }
};

} // namespace

TEST_CASE("frames queue on the voice's own device and start at the cushion", "[audio][playout]") {
    Harness h;
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "Speakers (Wireless Controller)")});
    CHECK(h.engine.playingFor("slot"));
    auto* playback = h.gateway.playbackFor("Speakers (Wireless Controller)");
    REQUIRE(playback != nullptr);

    // First frame: queued, but the device stays paused — starting an empty
    // device plays one window and clicks on the underrun.
    h.frame("sat", 0, 0, 0x11);
    CHECK(playback->queued.size() == kWindowSamples);
    CHECK(playback->queued.front() == 0x11);
    CHECK_FALSE(playback->resumed);

    // Second frame reaches the two-window cushion: now it starts.
    h.frame("sat", 0, 1, 0x22);
    CHECK(playback->queued.size() == 2 * kWindowSamples);
    CHECK(playback->resumed);
    CHECK(h.decoderState->decodes == 2);
}

TEST_CASE("a frame for a voice nobody holds is dropped", "[audio][playout]") {
    Harness h;
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk")});
    h.frame("sat", 1, 0, 0x11);   // wrong index
    h.frame("other", 0, 0, 0x11); // wrong connection
    CHECK(h.decoderState->decodes == 0);
    CHECK(h.gateway.playbackFor("spk")->queued.empty());
}

TEST_CASE("a reorder heals and a loss takes the FEC path with the carrier", "[audio][playout]") {
    Harness h;
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk")});
    auto* playback = h.gateway.playbackFor("spk");

    h.frame("sat", 0, 0, 0x10);
    h.frame("sat", 0, 1, 0x11);
    // 2 lost; 3 held (proves nothing yet), 4 declares the loss.
    h.frame("sat", 0, 3, 0x13);
    CHECK(h.decoderState->decodes == 2); // 3 is held, not played
    h.frame("sat", 0, 4, 0x14);
    // Gap for 2 (FEC off packet 3, the 4-byte carrier), then 3 and 4 in order.
    CHECK(h.decoderState->fecs == 1);
    CHECK(h.decoderState->decodes == 4);
    REQUIRE(playback->queued.size() == 5 * kWindowSamples);
    CHECK(playback->queued[2 * kWindowSamples] == static_cast<std::int16_t>(0x7E04));
    CHECK(playback->queued[3 * kWindowSamples] == 0x13);
    CHECK(playback->queued[4 * kWindowSamples] == 0x14);
}

TEST_CASE("a duplicate or late frame queues nothing", "[audio][playout]") {
    Harness h;
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk")});
    h.frame("sat", 0, 5, 0x15);
    h.frame("sat", 0, 5, 0x15); // duplicate: its slot already played
    h.frame("sat", 0, 4, 0x14); // late: behind the stream
    CHECK(h.decoderState->decodes == 1);
    CHECK(h.gateway.playbackFor("spk")->queued.size() == kWindowSamples);
}

TEST_CASE("a drained queue is re-cushioned with silence before the resumed audio",
          "[audio][playout]") {
    // The satellite suppresses digitally silent windows without advancing seq,
    // so a stream goes quiet for seconds and the queue empties. The resumed
    // sound must ride behind a fresh 40 ms cushion of SILENCE — not wait for a
    // second frame that may be another suppression away.
    Harness h;
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk")});
    h.frame("sat", 0, 0, 0x10);
    h.frame("sat", 0, 1, 0x11);
    auto* playback = h.gateway.playbackFor("spk");
    REQUIRE(playback->resumed);

    // The device played everything out during the quiet stretch.
    for (auto& [handle, p] : h.gateway.playbacks) { h.gateway.drain(handle); }
    playback->queued.clear();

    // seq CONTINUES from 2: suppression is not loss, so no gap events fire.
    h.frame("sat", 0, 2, 0x12);
    CHECK(h.decoderState->fecs == 0);
    CHECK(h.decoderState->conceals == 0);
    const std::size_t cushion =
        static_cast<std::size_t>(dish::audio::kPlayoutStartThresholdFrames) * kWindowSamples;
    REQUIRE(playback->queued.size() == cushion + kWindowSamples);
    for (std::size_t i = 0; i < cushion; i++) {
        if (playback->queued[i] != 0) { FAIL("cushion sample " << i << " is not silence"); }
    }
    CHECK(playback->queued[cushion] == 0x12);
}

TEST_CASE("reconciling a voice away closes its device", "[audio][playout]") {
    Harness h;
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk")});
    CHECK(h.gateway.openPlaybackCount() == 1);
    h.engine.reconcile({});
    CHECK(h.gateway.openPlaybackCount() == 0);
    CHECK_FALSE(h.engine.playingFor("slot"));
    // Frames after the plan moved are dropped, not decoded.
    h.frame("sat", 0, 0, 0x11);
    CHECK(h.decoderState->decodes == 0);
}

TEST_CASE("a device change reopens the voice; an unchanged one is kept", "[audio][playout]") {
    Harness h;
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk-a")});
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk-a")});
    CHECK(h.gateway.closes == 0);
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk-b")});
    CHECK(h.gateway.closes == 1);
    CHECK(h.gateway.playbackFor("spk-a") == nullptr);
    CHECK(h.gateway.playbackFor("spk-b") != nullptr);
}

TEST_CASE("a refused playback device leaves no voice and retries next reconcile",
          "[audio][playout]") {
    Harness h;
    h.gateway.refuse = {"spk"};
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk")});
    CHECK_FALSE(h.engine.playingFor("slot"));
    h.gateway.refuse = {};
    h.engine.reconcile({Harness::voice("sat", 0, "slot", "spk")});
    CHECK(h.engine.playingFor("slot"));
}

TEST_CASE("two voices on one connection route by controller index", "[audio][playout]") {
    Harness h;
    h.engine.reconcile(
        {Harness::voice("sat", 0, "slot-a", "spk-a"), Harness::voice("sat", 1, "slot-b", "spk-b")});
    h.frame("sat", 1, 0, 0x21);
    CHECK(h.gateway.playbackFor("spk-a")->queued.empty());
    CHECK(h.gateway.playbackFor("spk-b")->queued.size() == kWindowSamples);
}
