// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MicCaptureEngine (source/audio/MicCaptureEngine.h) against the fake gateway
// and a scriptable encoder. The load-bearing claims: exact 20 ms windows in,
// one seq per window OUT INCLUDING failed encodes, and — the privacy
// invariant — a slot reconciled away has its device CLOSED and its callback
// disabled, so not one packet leaves after the decision.

#include "source/audio/MicCaptureEngine.h"

#include "FakeAudioGateway.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using dish::source::audio::MicCaptureEngine;
using dish::source::audio::MicCaptureTarget;
using dish::test::FakeAudioGateway;

namespace proto = dish::proto;

namespace {

// Encodes every window to a fixed marker packet; window sizes and call counts
// are recorded, and `failEvery` makes the Nth encode fail to pin the
// seq-advances-anyway rule.
class FakeEncoder : public dish::audio::IAudioEncoder {
  public:
    struct State {
        std::vector<std::size_t> windows;
        int failEvery = 0; // 0 = never fail
    };
    explicit FakeEncoder(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::size_t encode(const std::int16_t* pcm, std::size_t frames, std::uint8_t* out,
                       std::size_t maxOut) override {
        if (pcm == nullptr || out == nullptr || maxOut < 3) { return 0; }
        state_->windows.push_back(frames);
        const auto n = static_cast<int>(state_->windows.size());
        if (state_->failEvery != 0 && n % state_->failEvery == 0) { return 0; }
        out[0] = 0xE0;
        out[1] = static_cast<std::uint8_t>(n);
        out[2] = pcm[0] != 0 ? 0x01 : 0x00;
        return 3;
    }

  private:
    std::shared_ptr<State> state_;
};

struct Sent {
    std::uint16_t seq;
    std::vector<std::uint8_t> packet;
};

struct Harness {
    FakeAudioGateway gateway;
    std::shared_ptr<FakeEncoder::State> encoderState = std::make_shared<FakeEncoder::State>();
    MicCaptureEngine engine{&gateway,
                            [this] { return std::make_unique<FakeEncoder>(encoderState); }};
    std::vector<Sent> sent;

    MicCaptureTarget target(const std::string& slotId, const std::string& device) {
        MicCaptureTarget t;
        t.slotId = slotId;
        t.captureDeviceName = device;
        t.send = [this](std::uint16_t seq, const std::uint8_t* opus, std::size_t len) {
            sent.push_back(Sent{seq, std::vector<std::uint8_t>(opus, opus + len)});
            return true;
        };
        return t;
    }

    void pump(const std::string& device, std::size_t samples) {
        auto* capture = gateway.captureFor(device);
        REQUIRE(capture != nullptr);
        const std::vector<std::int16_t> pcm(samples, 7);
        capture->onSamples(pcm.data(), pcm.size());
    }
};

constexpr std::size_t kFrame = static_cast<std::size_t>(proto::kAudioFrameSamples);

} // namespace

TEST_CASE("a reconciled target opens its device and streams windowed packets",
          "[audio][micengine]") {
    Harness h;
    h.engine.reconcile({h.target("slot", "Headset Microphone (Wireless Controller)")});
    CHECK(h.engine.capturingFor("slot"));
    CHECK(h.gateway.openCaptureCount() == 1);

    // One and a half windows: exactly one packet, the tail carried.
    h.pump("Headset Microphone (Wireless Controller)", kFrame + kFrame / 2);
    REQUIRE(h.sent.size() == 1U);
    CHECK(h.sent[0].seq == 0);
    CHECK(h.sent[0].packet.size() == 3U);
    REQUIRE(h.encoderState->windows.size() == 1U);
    CHECK(h.encoderState->windows[0] == kFrame); // never a partial window

    // The carried half completes on the next chunk; seq advances.
    h.pump("Headset Microphone (Wireless Controller)", kFrame / 2);
    REQUIRE(h.sent.size() == 2U);
    CHECK(h.sent[1].seq == 1);
}

TEST_CASE("a failed encode spends its seq without sending", "[audio][micengine]") {
    // That 20 ms really happened: skipping the seq would play the stream short
    // at the receiver and drift it against the pad's clock; the gap is what
    // the satellite's concealment is for.
    Harness h;
    h.encoderState->failEvery = 2; // windows 2, 4, ... fail
    h.engine.reconcile({h.target("slot", "mic")});
    h.pump("mic", kFrame * 4);
    REQUIRE(h.sent.size() == 2U);
    CHECK(h.sent[0].seq == 0);
    CHECK(h.sent[1].seq == 2); // seq 1 and 3 were spent by the failed encodes
    CHECK(h.encoderState->windows.size() == 4U);
}

TEST_CASE("reconciling a slot away closes the device and silences it", "[audio][micengine]") {
    // THE PRIVACY INVARIANT, at the engine's edge. Mute, a toggle, a dead
    // session and a lost route all arrive here the same way: the target is
    // gone from the list, and gone must mean a closed device AND zero sends —
    // even for a callback the platform had already handed samples to.
    Harness h;
    h.engine.reconcile({h.target("slot", "mic")});
    auto* capture = h.gateway.captureFor("mic");
    REQUIRE(capture != nullptr);
    h.pump("mic", kFrame);
    REQUIRE(h.sent.size() == 1U);

    // Keep the callback the fake captured, as a real audio thread would.
    const auto lateCallback = capture->onSamples;
    h.engine.reconcile({});
    CHECK_FALSE(h.engine.capturingFor("slot"));
    CHECK(h.gateway.openCaptureCount() == 0);
    CHECK(h.gateway.closes == 1);

    // A window that raced the close is dropped by the enabled flag, not sent.
    const std::vector<std::int16_t> pcm(kFrame, 7);
    lateCallback(pcm.data(), pcm.size());
    CHECK(h.sent.size() == 1U);
    CHECK(h.encoderState->windows.size() == 1U); // it never even reached the encoder
}

TEST_CASE("a device change reopens; an unchanged target is left running", "[audio][micengine]") {
    Harness h;
    h.engine.reconcile({h.target("slot", "mic-a")});
    h.pump("mic-a", kFrame / 2); // half a window in flight
    h.engine.reconcile({h.target("slot", "mic-a")});
    CHECK(h.gateway.closes == 0); // unchanged: not restarted, tail preserved
    h.pump("mic-a", kFrame / 2);
    CHECK(h.sent.size() == 1U); // the two halves made one window

    h.engine.reconcile({h.target("slot", "mic-b")});
    CHECK(h.gateway.closes == 1);
    CHECK(h.gateway.captureFor("mic-a") == nullptr);
    CHECK(h.gateway.captureFor("mic-b") != nullptr);
}

TEST_CASE("a refused device leaves no session and the next reconcile retries",
          "[audio][micengine]") {
    Harness h;
    h.gateway.refuse = {"mic"};
    h.engine.reconcile({h.target("slot", "mic")});
    CHECK_FALSE(h.engine.capturingFor("slot"));
    CHECK(h.gateway.openCaptureCount() == 0);

    h.gateway.refuse = {};
    h.engine.reconcile({h.target("slot", "mic")});
    CHECK(h.engine.capturingFor("slot"));
}

TEST_CASE("two targets capture from their own devices independently", "[audio][micengine]") {
    Harness h;
    h.engine.reconcile({h.target("a", "mic-a"), h.target("b", "mic-b")});
    CHECK(h.gateway.openCaptureCount() == 2);
    h.pump("mic-a", kFrame);
    REQUIRE(h.sent.size() == 1U);
    h.engine.reconcile({h.target("a", "mic-a")});
    CHECK(h.gateway.openCaptureCount() == 1);
    CHECK(h.engine.capturingFor("a"));
    CHECK_FALSE(h.engine.capturingFor("b"));
}

TEST_CASE("destruction closes every capture", "[audio][micengine]") {
    FakeAudioGateway gateway;
    {
        MicCaptureEngine engine{
            &gateway,
            [] { return std::make_unique<FakeEncoder>(std::make_shared<FakeEncoder::State>()); }};
        MicCaptureTarget t;
        t.slotId = "slot";
        t.captureDeviceName = "mic";
        t.send = [](std::uint16_t, const std::uint8_t*, std::size_t) { return true; };
        engine.reconcile({t});
        CHECK(gateway.openCaptureCount() == 1);
    }
    CHECK(gateway.openCaptureCount() == 0);
}
