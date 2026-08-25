// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The known-answer packets are from a real Moonlight session (captured in
// Wolf's testControl.cpp, MIT); they pin the whole construction — framing,
// key handling, the IV built from the sequence number and the GCM tag — in
// both directions. The remaining cases cover seq evolution, tampering and
// framing edge cases.

#include "core/moonlight/MoonlightControlCipher.h"

#include "Util/Hex.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using dish::mooncrypto::ControlCipher;
namespace util = dish::util;

namespace {

std::array<std::uint8_t, 16> sessionKey() {
    const auto bytes = util::fromHex("EDF04A215C4FBEA20934120C8480D855");
    REQUIRE(bytes.has_value());
    std::array<std::uint8_t, 16> key{};
    std::copy(bytes->begin(), bytes->end(), key.begin());
    return key;
}

std::vector<std::uint8_t> bytesOf(const std::string& hex) {
    const auto decoded = util::fromHex(hex);
    REQUIRE(decoded.has_value());
    return *decoded;
}

struct Fixture {
    std::uint32_t seq;
    std::string plaintextHex;
    std::string packetHex;
};

// {seq, decrypted plaintext, full encrypted packet} from the captured session.
const Fixture kFixtures[] = {
    {0, "020302000000", "01001a0000000000bf0eb6da10e47c702ec8644eb87d9cf7b6fac9ff75ca"},
    {1, "0703010000", "010019000100000021dbb8dc0590af3a2b20bce5a347de31d366e5b9c5"},
    {2, "000208000400000000000000",
     "0100200002000000220722fbaded58a03f2e8898f0f1dcb7c93f6235590618e4186ad990"},
    {6, "060212000000000e05000000033400c00000059f0329",
     "01002a00060000005a4d999fb2542f85bdd39d99f77eb825254569d2c04e21241b5cec01bd3f93129718ecc1"
     "f153"},
};

} // namespace

TEST_CASE("seal reproduces captured session packets byte-for-byte", "[moonlight][controlcipher]") {
    ControlCipher cipher;
    REQUIRE(cipher.setKey(sessionKey()));

    for (const auto& fx : kFixtures) {
        const auto plaintext = bytesOf(fx.plaintextHex);
        std::array<std::uint8_t, 256> out{};
        const std::size_t len = cipher.seal(fx.seq, plaintext.data(), plaintext.size(), out.data());
        REQUIRE(len == plaintext.size() + ControlCipher::kOverhead);
        CHECK(util::toHex(out.data(), len) == fx.packetHex);
    }
}

TEST_CASE("open recovers captured session plaintexts", "[moonlight][controlcipher]") {
    ControlCipher cipher;
    REQUIRE(cipher.setKey(sessionKey()));

    for (const auto& fx : kFixtures) {
        const auto packet = bytesOf(fx.packetHex);
        std::array<std::uint8_t, 256> out{};
        const auto len = cipher.open(packet.data(), packet.size(), out.data(), out.size());
        REQUIRE(len.has_value());
        CHECK(util::toHex(out.data(), *len) == fx.plaintextHex);
    }
}

TEST_CASE("seal/open round-trips across an evolving sequence", "[moonlight][controlcipher]") {
    ControlCipher sender;
    ControlCipher receiver;
    REQUIRE(sender.setKey(sessionKey()));
    REQUIRE(receiver.setKey(sessionKey()));

    const auto plaintext = bytesOf("060222000000001e0c000000");
    std::array<std::uint8_t, 256> packet{};
    std::array<std::uint8_t, 256> recovered{};

    // Crosses the u8 IV truncation boundary at 256 deliberately: both ends
    // must keep agreeing when seq mod 256 wraps.
    for (std::uint32_t seq : {0U, 1U, 2U, 255U, 256U, 257U, 511U, 70000U}) {
        const std::size_t len = sender.seal(seq, plaintext.data(), plaintext.size(), packet.data());
        REQUIRE(len > 0);
        const auto ptLen = receiver.open(packet.data(), len, recovered.data(), recovered.size());
        REQUIRE(ptLen.has_value());
        CHECK(util::toHex(recovered.data(), *ptLen) == util::toHex(plaintext));
    }
}

TEST_CASE("a tampered packet is rejected", "[moonlight][controlcipher]") {
    ControlCipher cipher;
    REQUIRE(cipher.setKey(sessionKey()));
    std::array<std::uint8_t, 256> out{};

    SECTION("flipped ciphertext byte") {
        auto packet = bytesOf(kFixtures[2].packetHex);
        packet[packet.size() - 1] ^= 0x01;
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), out.size()));
    }
    SECTION("flipped tag byte") {
        auto packet = bytesOf(kFixtures[2].packetHex);
        packet[8] ^= 0x80; // inside the 16-byte tag
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), out.size()));
    }
    SECTION("altered seq changes the IV and fails authentication") {
        auto packet = bytesOf(kFixtures[2].packetHex);
        packet[4] ^= 0x01;
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), out.size()));
    }
    SECTION("wrong key") {
        ControlCipher wrong;
        std::array<std::uint8_t, 16> other{};
        other.fill(0x42);
        REQUIRE(wrong.setKey(other));
        const auto packet = bytesOf(kFixtures[2].packetHex);
        CHECK_FALSE(wrong.open(packet.data(), packet.size(), out.data(), out.size()));
    }
}

TEST_CASE("misframed packets are rejected before crypto", "[moonlight][controlcipher]") {
    ControlCipher cipher;
    REQUIRE(cipher.setKey(sessionKey()));
    std::array<std::uint8_t, 256> out{};

    SECTION("too short for the framing") {
        const auto packet = bytesOf("01001a000000");
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), out.size()));
    }
    SECTION("wrong outer type") {
        auto packet = bytesOf(kFixtures[0].packetHex);
        packet[0] = 0x02;
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), out.size()));
    }
    SECTION("declared length overruns the buffer") {
        auto packet = bytesOf(kFixtures[0].packetHex);
        packet[2] = 0xFF; // len low byte
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), out.size()));
    }
    SECTION("declared length below the seq+tag minimum") {
        auto packet = bytesOf(kFixtures[0].packetHex);
        packet[2] = 0x08;
        packet[3] = 0x00;
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), out.size()));
    }
    SECTION("output buffer too small") {
        const auto packet = bytesOf(kFixtures[3].packetHex);
        CHECK_FALSE(cipher.open(packet.data(), packet.size(), out.data(), 4));
    }
}

TEST_CASE("seal refuses to run without a key", "[moonlight][controlcipher]") {
    ControlCipher cipher;
    const auto plaintext = bytesOf("0002080004000000");
    std::array<std::uint8_t, 64> out{};
    CHECK(cipher.seal(1, plaintext.data(), plaintext.size(), out.data()) == 0);
    CHECK_FALSE(cipher.hasKey());
}
