// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/net/IpLiterals.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using dish::net::isIpv6Address;
using dish::net::isPrivateHostLiteral;

TEST_CASE("isPrivateHostLiteral accepts private IPv4 / IPv6 literals", "[iplit]") {
    CHECK(isPrivateHostLiteral("10.0.0.5"));       // 10/8
    CHECK(isPrivateHostLiteral("172.16.0.1"));     // bottom of 172.16/12
    CHECK(isPrivateHostLiteral("172.31.255.255")); // top of 172.16/12
    CHECK(isPrivateHostLiteral("192.168.1.1"));    // 192.168/16
    CHECK(isPrivateHostLiteral("169.254.1.1"));    // 169.254/16 link-local
    CHECK(isPrivateHostLiteral("127.0.0.1"));      // 127/8 loopback
    CHECK(isPrivateHostLiteral("::1"));            // IPv6 loopback
    CHECK(isPrivateHostLiteral("fe80::1"));        // fe80::/10 link-local
    CHECK(isPrivateHostLiteral("[fe80::1]"));      // bracketed authority form
    CHECK(isPrivateHostLiteral("fc00::1"));        // fc00::/7 unique-local
}

TEST_CASE("isPrivateHostLiteral rejects public literals and hostnames", "[iplit]") {
    CHECK_FALSE(isPrivateHostLiteral("8.8.8.8"));     // public
    CHECK_FALSE(isPrivateHostLiteral("172.32.0.1"));  // just past 172.16/12
    CHECK_FALSE(isPrivateHostLiteral("11.0.0.1"));    // 11.x is public (only 10/8 private)
    CHECK_FALSE(isPrivateHostLiteral("172.15.0.1"));  // just below 172.16/12
    CHECK_FALSE(isPrivateHostLiteral("example.com")); // hostname, not a literal
    CHECK_FALSE(isPrivateHostLiteral(""));            // empty
    CHECK_FALSE(isPrivateHostLiteral("999.1.1.1"));   // octet out of range
    CHECK_FALSE(isPrivateHostLiteral("1.2.3"));       // too few octets
}

TEST_CASE("isPrivateHostLiteral handles IPv6 edge forms", "[iplit]") {
    CHECK(isPrivateHostLiteral("fd12:3456:789a::1"));          // fd00::/8 (part of fc00::/7)
    CHECK_FALSE(isPrivateHostLiteral("2001:4860:4860::8888")); // public IPv6
    CHECK_FALSE(isPrivateHostLiteral("fe80::1%eth0"));         // zone id rejected
    CHECK_FALSE(isPrivateHostLiteral("::ffff:8.8.8.8"));  // v4-mapped, high bytes 0 -> not private
    CHECK_FALSE(isPrivateHostLiteral("::ffff:10.0.0.1")); // v4-mapped, high bytes 0 -> not private
}

TEST_CASE("isIpv6Address names every IPv6 form and nothing else", "[iplit]") {
    CHECK(isIpv6Address("::1"));
    CHECK(isIpv6Address("[::1]"));
    CHECK(isIpv6Address("fd12:3456:789a::1"));
    CHECK(isIpv6Address("2001:4860:4860::8888"));
    // A zone, or a literal the parser would not take, still names IPv6 and still cannot reach
    // an IPv4-only satellite.
    CHECK(isIpv6Address("fe80::1%eth0"));
    CHECK(isIpv6Address("fe80::1::2"));
    CHECK_FALSE(isIpv6Address("127.0.0.1"));
    CHECK_FALSE(isIpv6Address("192.168.1.20"));
    CHECK_FALSE(isIpv6Address("satellite.local"));
    CHECK_FALSE(isIpv6Address(""));
}
