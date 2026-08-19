// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/connection/MdnsDiscovery.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace dish::net;
using dish::models::DiscoveredServer;
using dish::models::DiscoverySource;

namespace {

void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

// `dotted` carries no trailing dot, e.g. "sat-1._satellite._udp.local".
void writeName(std::vector<std::uint8_t>& v, const std::string& dotted) {
    std::size_t i = 0;
    while (i < dotted.size()) {
        std::size_t j = dotted.find('.', i);
        if (j == std::string::npos) { j = dotted.size(); }
        v.push_back(static_cast<std::uint8_t>(j - i));
        for (std::size_t k = i; k < j; ++k) { v.push_back(static_cast<std::uint8_t>(dotted[k])); }
        i = j + 1;
    }
    v.push_back(0);
}

void appendRr(std::vector<std::uint8_t>& v, const std::string& name, std::uint16_t type,
              const std::vector<std::uint8_t>& rdata) {
    writeName(v, name);
    put16(v, type);
    put16(v, 0x8001); // class IN + cache-flush — the parser ignores the class
    put32(v, 120);    // ttl
    put16(v, static_cast<std::uint16_t>(rdata.size()));
    v.insert(v.end(), rdata.begin(), rdata.end());
}

std::vector<std::uint8_t> srvRdata(std::uint16_t port, const std::string& target) {
    std::vector<std::uint8_t> r;
    put16(r, 0); // priority
    put16(r, 0); // weight
    put16(r, port);
    writeName(r, target);
    return r;
}

std::vector<std::uint8_t> txtRdata(const std::vector<std::string>& entries) {
    std::vector<std::uint8_t> r;
    if (entries.empty()) {
        r.push_back(0);
        return r;
    }
    for (const auto& e : entries) {
        r.push_back(static_cast<std::uint8_t>(e.size()));
        for (char ch : e) { r.push_back(static_cast<std::uint8_t>(ch)); }
    }
    return r;
}

std::vector<std::uint8_t> ptrRdata(const std::string& target) {
    std::vector<std::uint8_t> r;
    writeName(r, target);
    return r;
}

std::string kv(const std::string& key, const std::string& value) { return key + "=" + value; }

std::vector<std::uint8_t> responseHeader(int answerCount) {
    std::vector<std::uint8_t> v;
    put16(v, 0);      // id
    put16(v, 0x8400); // flags: QR + AA
    put16(v, 0);      // qdcount
    put16(v, static_cast<std::uint16_t>(answerCount));
    put16(v, 0); // nscount
    put16(v, 0); // arcount
    return v;
}

} // namespace

TEST_CASE("readName decodes a plain length-prefixed name", "[mdns]") {
    std::vector<std::uint8_t> v;
    writeName(v, "sat.local");
    std::string out;
    REQUIRE(detail::readName(v.data(), v.size(), 0, out));
    CHECK(out == "sat.local");
}

TEST_CASE("readName follows a backward compression pointer", "[mdns]") {
    // 0: "foo.bar." (9 bytes); 9: "baz" label + pointer to offset 4 ("bar").
    std::vector<std::uint8_t> v = {3, 'f', 'o', 'o', 3,   'b',  'a', 'r',
                                   0, 3,   'b', 'a', 'z', 0xC0, 0x04};
    std::string out;
    REQUIRE(detail::readName(v.data(), v.size(), 9, out));
    CHECK(out == "baz.bar");
}

TEST_CASE("readName rejects a self / forward compression pointer", "[mdns]") {
    std::vector<std::uint8_t> v = {0xC0, 0x00}; // pointer to itself
    std::string out;
    CHECK_FALSE(detail::readName(v.data(), v.size(), 0, out));
}

TEST_CASE("readName rejects a label running past the packet", "[mdns]") {
    std::vector<std::uint8_t> v = {0x0A, 'a', 'b'}; // label claims 10, has 2
    std::string out;
    CHECK_FALSE(detail::readName(v.data(), v.size(), 0, out));
}

TEST_CASE("skipName reports the bytes consumed at the offset", "[mdns]") {
    std::vector<std::uint8_t> v;
    writeName(v, "a.bc"); // 1+1 + 1+2 + 1 terminator = 6
    CHECK(detail::skipName(v.data(), v.size(), 0) == v.size());
}

TEST_CASE("skipName counts a compression pointer as two bytes", "[mdns]") {
    std::vector<std::uint8_t> v = {3, 'f', 'o', 'o', 3,   'b',  'a', 'r',
                                   0, 3,   'b', 'a', 'z', 0xC0, 0x04};
    // Name at offset 9: "baz" label (4 bytes) + pointer (2 bytes) = 6 consumed.
    CHECK(detail::skipName(v.data(), v.size(), 9) == 6);
}

TEST_CASE("parseResponse decodes a full satellite response", "[mdns]") {
    auto pkt = responseHeader(4);
    appendRr(pkt, "_satellite._udp.local", 12, ptrRdata("sat-1._satellite._udp.local"));
    appendRr(pkt, "sat-1._satellite._udp.local", 33, srvRdata(9876, "sat-1.local"));
    appendRr(pkt, "sat-1._satellite._udp.local", 16,
             txtRdata({"udp=9876", "pair=9878", "http=9877"}));
    appendRr(pkt, "sat-1.local", 1, {192, 168, 1, 50});

    const auto out = detail::parseResponse(pkt.data(), pkt.size());
    REQUIRE(out.has_value());
    CHECK(out->ip == QStringLiteral("192.168.1.50"));
    CHECK(out->udpPort == 9876);
    CHECK(out->pairPort == 9878);
    CHECK(out->httpPort == 9877);
    CHECK(out->name == QStringLiteral("sat-1"));
    CHECK(out->source == DiscoverySource::Mdns);
    CHECK(out->machineId.isEmpty()); // no mid TXT in this packet
}

TEST_CASE("parseResponse plumbs the machineId from the mid TXT key", "[mdns]") {
    auto pkt = responseHeader(3);
    appendRr(pkt, "sat._satellite._udp.local", 33, srvRdata(9876, "sat.local"));
    appendRr(pkt, "sat._satellite._udp.local", 16, txtRdata({kv("mid", "boxabc"), "udp=9876"}));
    appendRr(pkt, "sat.local", 1, {10, 0, 0, 5});

    const auto out = detail::parseResponse(pkt.data(), pkt.size());
    REQUIRE(out.has_value());
    CHECK(out->machineId == QStringLiteral("boxabc"));
}

TEST_CASE("parseResponse reads ports from TXT when SRV is absent", "[mdns]") {
    auto pkt = responseHeader(2);
    appendRr(pkt, "sat._satellite._udp.local", 16,
             txtRdata({"udp=40000", "pair=40001", "http=40002"}));
    appendRr(pkt, "sat.local", 1, {10, 0, 0, 7});

    const auto out = detail::parseResponse(pkt.data(), pkt.size());
    REQUIRE(out.has_value());
    CHECK(out->udpPort == 40000);
    CHECK(out->pairPort == 40001);
    CHECK(out->httpPort == 40002);
}

TEST_CASE("parseResponse takes the UDP port from SRV when TXT is absent", "[mdns]") {
    auto pkt = responseHeader(2);
    appendRr(pkt, "sat._satellite._udp.local", 33, srvRdata(50505, "sat.local"));
    appendRr(pkt, "sat.local", 1, {10, 0, 0, 8});

    const auto out = detail::parseResponse(pkt.data(), pkt.size());
    REQUIRE(out.has_value());
    CHECK(out->udpPort == 50505);
    CHECK(out->pairPort == 9443); // client API default (HTTPS)
    CHECK(out->httpPort == 9443); // client API default (HTTPS)
}

TEST_CASE("parseResponse rejects a packet with no A record", "[mdns]") {
    auto pkt = responseHeader(1);
    appendRr(pkt, "sat._satellite._udp.local", 33, srvRdata(9876, "sat.local"));
    CHECK_FALSE(detail::parseResponse(pkt.data(), pkt.size()).has_value());
}

TEST_CASE("parseResponse rejects a packet with neither SRV nor TXT", "[mdns]") {
    auto pkt = responseHeader(1);
    appendRr(pkt, "sat.local", 1, {10, 0, 0, 9});
    CHECK_FALSE(detail::parseResponse(pkt.data(), pkt.size()).has_value());
}

TEST_CASE("parseResponse rejects a packet shorter than a DNS header", "[mdns]") {
    std::vector<std::uint8_t> pkt(8, 0);
    CHECK_FALSE(detail::parseResponse(pkt.data(), pkt.size()).has_value());
}

TEST_CASE("parseResponse rejects an rdlen that overruns the packet", "[mdns]") {
    auto pkt = responseHeader(1);
    writeName(pkt, "sat.local");
    put16(pkt, 1);      // type A
    put16(pkt, 0x8001); // class
    put32(pkt, 120);    // ttl
    put16(pkt, 0xFFFF); // rdlen — far past the packet end
    pkt.insert(pkt.end(), {10, 0, 0, 9});
    CHECK_FALSE(detail::parseResponse(pkt.data(), pkt.size()).has_value());
}
