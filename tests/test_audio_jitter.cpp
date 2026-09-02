// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AudioJitterWindow (core/audio/AudioJitter.h): the 2-frame reorder window that
// decides, from wrapping u16 sequence numbers alone, which speaker frames to
// play, which to conceal, and which arrived too late to be worth anything.
//
// A deliberate mirror of satellite's tests/test_audio_jitter.cpp and
// dish-android's audio_jitter_test.cpp, case for case: the two ends of one
// stream have to agree on what counts as lost, what counts as late, and how
// long a hole is worth concealing, and a divergence there would show up as
// audible glitching rather than as a failure anywhere.
//
// Dependency-free by design, so this suite links no codec: the ordering rules
// are exactly the part that should be provable without one.

#include "core/audio/AudioJitter.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using dish::audio::AudioJitterWindow;
using Kind = AudioJitterWindow::Event::Kind;
using Accept = AudioJitterWindow::Accept;

// Packets are only ever compared by identity here, so each one is a block of a
// distinctive byte: it makes "the window handed back the packet I pushed" an
// assertion rather than a hope.
std::vector<std::uint8_t> packet(std::uint8_t tag, std::size_t bytes = 24) {
    return std::vector<std::uint8_t>(bytes, tag);
}

AudioJitterWindow::Result push(AudioJitterWindow& w, std::uint16_t seq,
                               const std::vector<std::uint8_t>& p) {
    return w.push(seq, p.data(), p.size());
}

// One-line shape of a result: "P12" for a packet, "G12" for a gap, "G12/F" for
// a gap that has an FEC carrier in hand. Comparing shapes catches ordering
// mistakes that per-field asserts miss.
std::string shape(const AudioJitterWindow::Result& r) {
    std::string s;
    for (int i = 0; i < r.count; i++) {
        const AudioJitterWindow::Event& e = r.events[i];
        if (!s.empty()) { s += " "; }
        s += (e.kind == Kind::Packet ? "P" : "G");
        s += std::to_string(e.seq);
        if (e.kind == Kind::Gap && e.fecCarrier != nullptr) { s += "/F"; }
    }
    return s;
}

// Every byte of the event's payload equals `tag`.
bool payloadIs(const AudioJitterWindow::Event& e, std::uint8_t tag) {
    if (e.data == nullptr || e.len == 0) { return false; }
    for (std::size_t i = 0; i < e.len; i++) {
        if (e.data[i] != tag) { return false; }
    }
    return true;
}

bool carrierIs(const AudioJitterWindow::Event& e, std::uint8_t tag) {
    if (e.fecCarrier == nullptr || e.fecCarrierLen == 0) { return false; }
    for (std::size_t i = 0; i < e.fecCarrierLen; i++) {
        if (e.fecCarrier[i] != tag) { return false; }
    }
    return true;
}

} // namespace

TEST_CASE("in-order stream passes straight through", "[audio][jitter]") {
    AudioJitterWindow w;
    CHECK_FALSE(w.primed());

    for (int i = 0; i < 8; i++) {
        const auto p = packet(static_cast<std::uint8_t>(0x40 + i));
        const auto r = push(w, static_cast<std::uint16_t>(100 + i), p);
        CHECK(r.accept == Accept::Ok);
        REQUIRE(r.count == 1);
        CHECK(r.events[0].kind == Kind::Packet);
        CHECK(static_cast<int>(r.events[0].seq) == 100 + i);
        CHECK(payloadIs(r.events[0], static_cast<std::uint8_t>(0x40 + i)));
        // The clean path never copies: the event aliases the caller's buffer.
        CHECK(r.events[0].data == p.data());
        CHECK(w.buffered() == 0);
    }
    CHECK(w.primed());
    CHECK(static_cast<int>(w.nextSeq()) == 108);
}

TEST_CASE("first packet defines the origin whatever its seq", "[audio][jitter]") {
    AudioJitterWindow w;
    const auto p = packet(0x11);
    // There is no such thing as a late or missing frame before the stream
    // started, so a mid-range opening seq is simply where it starts.
    const auto r = push(w, 40000, p);
    CHECK(r.accept == Accept::Ok);
    CHECK(shape(r) == "P40000");
    CHECK(static_cast<int>(w.nextSeq()) == 40001);
}

TEST_CASE("one swapped pair is reordered, not concealed", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0xA0));
    push(w, 11, packet(0xA1));

    // 13 before 12: held, nothing due yet. This is the whole reason the window
    // exists, so it must not emit a gap here.
    const auto p13 = packet(0xA3);
    const auto held = push(w, 13, p13);
    CHECK(held.accept == Accept::Ok);
    CHECK(held.count == 0);
    CHECK(w.buffered() == 1);

    // 12 arrives: it goes out, and 13 follows it immediately.
    const auto p12 = packet(0xA2);
    const auto healed = push(w, 12, p12);
    CHECK(healed.accept == Accept::Ok);
    REQUIRE(shape(healed) == "P12 P13");
    CHECK(payloadIs(healed.events[0], 0xA2));
    CHECK(payloadIs(healed.events[1], 0xA3));
    CHECK(w.buffered() == 0);
    CHECK(static_cast<int>(w.nextSeq()) == 14);
}

TEST_CASE("lost frame is declared with its FEC carrier", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0xB0));
    push(w, 11, packet(0xB1));

    // 12 is lost. 13 alone proves nothing (it could be a reorder).
    const auto p13 = packet(0xB3);
    CHECK(push(w, 13, p13).count == 0);

    // 14 is 2 ahead of the frame we want, which is the window's whole
    // definition of "lost". The gap names 12 and carries 13, because Opus hides
    // a redundant copy of 12 inside 13 and that is what makes recovery
    // possible; then 13 and 14 follow in order.
    const auto p14 = packet(0xB4);
    const auto r = push(w, 14, p14);
    CHECK(r.accept == Accept::Ok);
    REQUIRE(shape(r) == "G12/F P13 P14");
    CHECK(carrierIs(r.events[0], 0xB3));
    CHECK(r.events[0].data == nullptr); // a gap has no packet of its own
    CHECK(payloadIs(r.events[1], 0xB3));
    CHECK(payloadIs(r.events[2], 0xB4));
}

TEST_CASE("back-to-back losses conceal blind then recover by FEC", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0xC0));
    push(w, 11, packet(0xC1));

    // 12 and 13 both lost. When 14 lands it is 2 ahead of 12, so 12 is
    // declared; nothing carries it (13 never arrived), so the gap is blind.
    const auto first = push(w, 14, packet(0xC4));
    REQUIRE(shape(first) == "G12");
    CHECK(first.events[0].fecCarrier == nullptr);

    // 15 lands: now 13 is 2 behind, and 14 IS in hand to carry it.
    const auto second = push(w, 15, packet(0xC5));
    REQUIRE(shape(second) == "G13/F P14 P15");
    CHECK(carrierIs(second.events[0], 0xC4));
}

TEST_CASE("frame arriving after its slot played is dropped", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0xD0));
    push(w, 11, packet(0xD1));
    push(w, 13, packet(0xD3));
    const auto flushed = push(w, 14, packet(0xD4));
    CHECK(shape(flushed) == "G12/F P13 P14");

    // 12 finally shows up, long after it was concealed. Playing it now would be
    // an audible jump backwards.
    const auto late = push(w, 12, packet(0xD2));
    CHECK(late.accept == Accept::Late);
    CHECK(late.count == 0);

    // So would replaying a frame already emitted.
    const auto replay = push(w, 13, packet(0xD3));
    CHECK(replay.accept == Accept::Late);
    CHECK(replay.count == 0);

    // The stream carries on untouched.
    CHECK(shape(push(w, 15, packet(0xD5))) == "P15");
}

TEST_CASE("duplicate of a held frame is dropped", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0xE0));
    CHECK(push(w, 12, packet(0xE2)).count == 0); // held, waiting on 11

    const auto dup = push(w, 12, packet(0xE2));
    CHECK(dup.accept == Accept::Duplicate);
    CHECK(dup.count == 0);
    CHECK(w.buffered() == 1);

    // The real 11 still heals the hole.
    CHECK(shape(push(w, 11, packet(0xE1))) == "P11 P12");
}

TEST_CASE("empty, null and oversize packets are refused", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0xF0));

    const auto p = packet(0xF1);
    CHECK(w.push(11, nullptr, p.size()).accept == Accept::Rejected);
    CHECK(w.push(11, p.data(), 0).accept == Accept::Rejected);

    const auto huge =
        packet(0xF2, static_cast<std::size_t>(dish::audio::AUDIO_JITTER_MAX_PACKET_BYTES) + 1);
    CHECK(w.push(11, huge.data(), huge.size()).accept == Accept::Rejected);

    // Exactly at the ceiling is fine; the bound is on what the window can be
    // made to allocate, not on what is plausible.
    const auto atCap =
        packet(0xF3, static_cast<std::size_t>(dish::audio::AUDIO_JITTER_MAX_PACKET_BYTES));
    const auto ok = w.push(11, atCap.data(), atCap.size());
    CHECK(ok.accept == Accept::Ok);
    CHECK(shape(ok) == "P11");

    // None of the refusals moved the stream on.
    CHECK(static_cast<int>(w.nextSeq()) == 12);
}

TEST_CASE("zero is the frame after 0xFFFF, not 65535 losses", "[audio][jitter]") {
    AudioJitterWindow w;
    const std::uint16_t start = 0xFFFD;
    for (int i = 0; i < 6; i++) {
        const auto seq = static_cast<std::uint16_t>(start + i);
        const auto r = push(w, seq, packet(static_cast<std::uint8_t>(0x50 + i)));
        CHECK(r.accept == Accept::Ok);
        REQUIRE(r.count == 1);
        CHECK(static_cast<int>(r.events[0].seq) == static_cast<int>(seq));
    }
    CHECK(static_cast<int>(w.nextSeq()) == 3);
}

TEST_CASE("reorder and gap detection keep working across the wrap", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 0xFFFD, packet(0x60));
    push(w, 0xFFFE, packet(0x61));

    // 0x0000 before 0xFFFF: a one-frame reorder that happens to straddle the
    // wrap. Signed u16 arithmetic is what keeps this from reading as a jump.
    CHECK(push(w, 0x0000, packet(0x63)).count == 0);
    CHECK(shape(push(w, 0xFFFF, packet(0x62))) == "P65535 P0");

    // And a loss straddling it: 0x0001 lost, 0x0002 held, 0x0003 declares it.
    CHECK(push(w, 0x0002, packet(0x65)).count == 0);
    const auto gapped = push(w, 0x0003, packet(0x66));
    REQUIRE(shape(gapped) == "G1/F P2 P3");
    CHECK(carrierIs(gapped.events[0], 0x65));
}

TEST_CASE("long dropout is concealed only to the cap, then resyncs", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0x70));

    // Half a second of nothing, then the stream resumes. Concealing all of it
    // would inject synthetic audio AND hold the stream that far behind live.
    const auto resume = push(w, 36, packet(0x71));
    int gaps = 0;
    int packets = 0;
    for (int i = 0; i < resume.count; i++) {
        if (resume.events[i].kind == Kind::Gap) {
            gaps++;
        } else {
            packets++;
        }
    }
    CHECK(gaps == dish::audio::AUDIO_JITTER_MAX_CONCEAL_FRAMES);
    CHECK(packets == 1);
    CHECK(shape(resume) == "G11 G12 P36");
    // Resynchronised onto the new audio, not still grinding through the hole.
    CHECK(static_cast<int>(w.nextSeq()) == 37);
    CHECK(w.buffered() == 0);

    // A frame from inside the skipped range is history now.
    CHECK(push(w, 20, packet(0x72)).accept == Accept::Late);
}

TEST_CASE("concealment budget refills once real audio gets through", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 10, packet(0x80));
    // Burn the budget on one dropout...
    CHECK(shape(push(w, 30, packet(0x81))) == "G11 G12 P30");
    // ...then a clean frame, then another dropout: the second one gets its own
    // full allowance rather than inheriting a spent counter.
    CHECK(shape(push(w, 31, packet(0x82))) == "P31");
    CHECK(shape(push(w, 60, packet(0x83))) == "G32 G33 P60");
}

TEST_CASE("at most one frame is ever left waiting", "[audio][jitter]") {
    AudioJitterWindow w;
    // A deliberately hostile order: reorders, repeats, jumps forward and back.
    const std::uint16_t order[] = {100, 102, 101, 105, 103,   104, 104, 110, 108,
                                   111, 112, 109, 113, 65535, 116, 115, 117};
    for (std::uint16_t seq : order) {
        push(w, seq, packet(0x90));
        // The slots_ array is sized by this invariant; if it ever stops
        // holding, the window would be writing past its storage.
        CHECK(w.buffered() < dish::audio::AUDIO_JITTER_WINDOW_FRAMES);
    }
}

TEST_CASE("no arrival order overruns the result array", "[audio][jitter]") {
    AudioJitterWindow w;
    // Worst shape the window can produce: something held, then a jump big
    // enough to burn the concealment budget and resync in one push.
    push(w, 10, packet(0xA0));
    push(w, 11, packet(0xA1));
    CHECK(push(w, 13, packet(0xA3)).count == 0); // 12 held back
    const auto burst = push(w, 900, packet(0xA9));
    CHECK(burst.count <= dish::audio::AUDIO_JITTER_MAX_EVENTS_PER_PUSH);
    CHECK(burst.count > 0);
}

TEST_CASE("reset forgets the old pad's stream entirely", "[audio][jitter]") {
    AudioJitterWindow w;
    push(w, 500, packet(0xB0));
    push(w, 502, packet(0xB2)); // left waiting
    CHECK(w.buffered() == 1);

    w.reset();
    CHECK_FALSE(w.primed());
    CHECK(w.buffered() == 0);

    // A seq that would have been ancient history before the reset is now simply
    // where the new stream begins: a re-PUT restarts the far end's numbering and
    // must not have its first second thrown away as "late".
    const auto r = push(w, 3, packet(0xB3));
    CHECK(r.accept == Accept::Ok);
    CHECK(shape(r) == "P3");
}

TEST_CASE("window constants match the contract", "[audio][jitter]") {
    // Pinned here because they are a cross-repo agreement, not a tuning knob:
    // satellite's window is the same two frames with the same conceal cap, and
    // a change on one side alone would desynchronise the two ends of a stream.
    CHECK(dish::audio::AUDIO_JITTER_WINDOW_FRAMES == 2);
    CHECK(dish::audio::AUDIO_JITTER_MAX_CONCEAL_FRAMES == 2);
    CHECK(dish::audio::AUDIO_JITTER_MAX_PACKET_BYTES == 1500);
    CHECK(dish::audio::AUDIO_JITTER_MAX_EVENTS_PER_PUSH == 6);
}
