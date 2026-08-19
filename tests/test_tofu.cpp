// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The fingerprint is lowercase 64-hex SHA-256 over the cert DER bytes.

#include "core/net/Tofu.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using dish::net::sha256FingerprintHex;
using dish::net::tofuVerdict;
using dish::net::TofuVerdict;

namespace {
std::string fp(const std::vector<std::uint8_t>& bytes) {
    return sha256FingerprintHex(bytes.data(), bytes.size());
}
} // namespace

TEST_CASE("no prior pin trusts first use", "[tofu]") {
    CHECK(tofuVerdict(std::nullopt, "aabb") == TofuVerdict::TrustFirstUse);
}

TEST_CASE("equal fingerprint is a match", "[tofu]") {
    CHECK(tofuVerdict(std::optional<std::string>("aabb"), "aabb") == TofuVerdict::Match);
}

TEST_CASE("match is case-insensitive on hex", "[tofu]") {
    CHECK(tofuVerdict(std::optional<std::string>("AABBcc"), "aabbCC") == TofuVerdict::Match);
}

TEST_CASE("different fingerprint is a mismatch", "[tofu]") {
    CHECK(tofuVerdict(std::optional<std::string>("aabb"), "ccdd") == TofuVerdict::Mismatch);
}

TEST_CASE("an empty stored string can still mismatch", "[tofu]") {
    // Only nullopt is "never pinned"; an empty-string pin is present and differs.
    CHECK(tofuVerdict(std::optional<std::string>(""), "aabb") == TofuVerdict::Mismatch);
}

TEST_CASE("sha256 of the empty input is the known vector", "[tofu]") {
    CHECK(fp({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256 of abc is the known vector", "[tofu]") {
    CHECK(fp({'a', 'b', 'c'}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 output is lowercase 64 hex chars", "[tofu]") {
    const std::string out = fp({0x00, 0x7F, 0xFF});
    CHECK(out.size() == 64);
    std::string lowered = out;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    CHECK(out == lowered);
}
