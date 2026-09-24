// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The IPv6 literal PARSE, exercised through the one function that exports it.
//
// isPrivateHostLiteral answers false for two different reasons: the literal did
// not parse, and it parsed to a public address. Every case below is chosen so
// the two are distinguishable: each rejected input would classify as PRIVATE if
// it parsed at all, so a false answer can only mean the parse refused it.

#include "core/net/IpLiterals.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using dish::net::isPrivateHostLiteral;

TEST_CASE("ipv6 parse: the eight-group form and its compressed spellings agree", "[iplit][ipv6]") {
    // All four spell fe80::1, which is link-local and therefore private.
    CHECK(isPrivateHostLiteral("fe80::1"));
    CHECK(isPrivateHostLiteral("fe80:0:0:0:0:0:0:1"));
    CHECK(isPrivateHostLiteral("fe80:0000:0000:0000:0000:0000:0000:0001"));
    CHECK(isPrivateHostLiteral("fe80::0:1"));
}

TEST_CASE("ipv6 parse: compression stands for at least one group", "[iplit][ipv6]") {
    // Eight groups written out with a "::" between them leaves the run standing
    // for nothing, which is not a legal literal. Spelled without the "::" the
    // same address is link-local, so a false here is the parse refusing it.
    CHECK(isPrivateHostLiteral("fe80:0:0:0:0:0:0:1"));
    CHECK_FALSE(isPrivateHostLiteral("fe80:0:0:0:0:0:0::1"));
}

TEST_CASE("ipv6 parse: a literal carries at most one compression", "[iplit][ipv6]") {
    CHECK_FALSE(isPrivateHostLiteral("fe80::1::2"));
    CHECK_FALSE(isPrivateHostLiteral("fe80::::1"));
}

TEST_CASE("ipv6 parse: a zone index is rejected rather than stripped", "[iplit][ipv6]") {
    // fe80::1 is private; the same address with a zone must not be, or a zone
    // would be a way to smuggle a literal past a stricter caller.
    //
    // Two things refuse these: the explicit '%' guard, and the hex-digit rule a
    // zone always lands inside. Mutating the guard alone does not turn these
    // green, which is why the case is written against the behaviour rather than
    // against the guard.
    CHECK(isPrivateHostLiteral("fe80::1"));
    CHECK_FALSE(isPrivateHostLiteral("fe80::1%eth0"));
    CHECK_FALSE(isPrivateHostLiteral("fe80::1%25eth0"));
}

TEST_CASE("ipv6 parse: a hextet is one to four hex digits", "[iplit][ipv6]") {
    CHECK(isPrivateHostLiteral("fe80::f"));    // one digit
    CHECK(isPrivateHostLiteral("fe80::ffff")); // four
    CHECK_FALSE(isPrivateHostLiteral("fe80::fffff"));
    CHECK_FALSE(isPrivateHostLiteral("fe80::g"));
    CHECK_FALSE(isPrivateHostLiteral("fe80::-1"));
    CHECK_FALSE(isPrivateHostLiteral("fe80: :1"));
}

TEST_CASE("ipv6 parse: hex case does not matter", "[iplit][ipv6]") {
    CHECK(isPrivateHostLiteral("FE80::ABCD"));
    CHECK(isPrivateHostLiteral("Fe80::AbCd"));
    CHECK(isPrivateHostLiteral("fd00::AAAA")); // unique-local, upper-case payload
}

TEST_CASE("ipv6 parse: an uncompressed literal must be exactly eight groups", "[iplit][ipv6]") {
    CHECK(isPrivateHostLiteral("fd00:0:0:0:0:0:0:1"));
    CHECK_FALSE(isPrivateHostLiteral("fd00:0:0:0:0:0:1"));     // seven
    CHECK_FALSE(isPrivateHostLiteral("fd00:0:0:0:0:0:0:0:1")); // nine
}

TEST_CASE("ipv6 parse: the embedded IPv4 form occupies the last two groups", "[iplit][ipv6]") {
    // ::ffff:a.b.c.d is the IPv4-mapped form. It is NOT private on the v6
    // rules, so these prove the parse reads the dotted quad rather than that
    // the classifier likes it.
    CHECK_FALSE(isPrivateHostLiteral("::ffff:10.0.0.1"));
    // fd00::/8 is unique-local whatever rides in the low bits, so a literal
    // that parses lands private and one that does not cannot.
    CHECK(isPrivateHostLiteral("fd00::10.0.0.1"));
    CHECK_FALSE(isPrivateHostLiteral("fd00::10.0.0"));
    CHECK_FALSE(isPrivateHostLiteral("fd00::10.0.0.256"));
    CHECK_FALSE(isPrivateHostLiteral("fd00::10.0.0.1.2"));
}

TEST_CASE("ipv6 parse: the loopback and the unspecified address are not the same",
          "[iplit][ipv6]") {
    CHECK(isPrivateHostLiteral("::1"));
    CHECK(isPrivateHostLiteral("0:0:0:0:0:0:0:1"));
    // "::" is the unspecified address, which is not loopback and not private.
    CHECK_FALSE(isPrivateHostLiteral("::"));
}

TEST_CASE("ipv6 parse: the URL-authority brackets are stripped before the parse", "[iplit][ipv6]") {
    CHECK(isPrivateHostLiteral("[fe80::1]"));
    CHECK(isPrivateHostLiteral("[::1]"));
    // A bracket on one side only is not the authority form and must not parse.
    CHECK_FALSE(isPrivateHostLiteral("[fe80::1"));
    CHECK_FALSE(isPrivateHostLiteral("fe80::1]"));
    // Brackets do not launder a zone index either.
    CHECK_FALSE(isPrivateHostLiteral("[fe80::1%eth0]"));
}

TEST_CASE("ipv6 parse: an empty or punctuation-only string is not a literal", "[iplit][ipv6]") {
    CHECK_FALSE(isPrivateHostLiteral(""));
    CHECK_FALSE(isPrivateHostLiteral(":"));
    CHECK_FALSE(isPrivateHostLiteral(":::"));
    CHECK_FALSE(isPrivateHostLiteral("[]"));
}

TEST_CASE("ipv6 parse: the classifier's three private ranges, at their edges", "[iplit][ipv6]") {
    // fc00::/7 is fc.. and fd..; fe80::/10 is the top two bits of the second
    // byte, so fe80 through febf are link-local and fec0 is not.
    CHECK(isPrivateHostLiteral("fc00::1"));
    CHECK(isPrivateHostLiteral("fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
    CHECK(isPrivateHostLiteral("fe80::1"));
    CHECK(isPrivateHostLiteral("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
    CHECK_FALSE(isPrivateHostLiteral("fbff::1"));
    CHECK_FALSE(isPrivateHostLiteral("fec0::1"));
    CHECK_FALSE(isPrivateHostLiteral("fe00::1"));
    CHECK_FALSE(isPrivateHostLiteral("2001:4860:4860::8888"));
}
