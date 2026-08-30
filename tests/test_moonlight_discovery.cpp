// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The one-shot mDNS sweep for `_nvstream._tcp.local.`: what it makes of a
// reply, and that it is over when it says it is.
//
// THE WINDOW IS THE CONTRACT. Opening the hosts screen kicks a sweep, and the
// screen shows a spinner for as long as one is running. The sweep is a blocking
// receive loop on a background thread with no other way out, so a deadline that
// stopped being enforced would leave that spinner up forever with nothing to
// take it down, on a screen the user cannot otherwise use. It is asserted from
// both sides: a window asked for is a window waited out, and a window of
// nothing is over at once.
//
// The parse cases feed hand-built packets straight to the parser, so they need
// no socket and no host on the network.

#include "source/moonlight/MoonlightDiscovery.h"

#include <catch2/catch_test_macros.hpp>

#include <QElapsedTimer>
#include <QHostAddress>
#include <QUdpSocket>

#include <cstdint>
#include <string>
#include <vector>

using namespace dish::source::moon;

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

// `dotted` carries no trailing dot, e.g. "Den._nvstream._tcp.local".
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
    put16(v, 0x8001); // class IN + cache-flush; the parser ignores the class
    put32(v, 120);
    put16(v, static_cast<std::uint16_t>(rdata.size()));
    v.insert(v.end(), rdata.begin(), rdata.end());
}

std::vector<std::uint8_t> header(int answers) {
    std::vector<std::uint8_t> v;
    put16(v, 0);      // id
    put16(v, 0x8400); // QR + AA
    put16(v, 0);      // qdcount
    put16(v, static_cast<std::uint16_t>(answers));
    put16(v, 0); // nscount
    put16(v, 0); // arcount
    return v;
}

std::vector<std::uint8_t> srvRdata(std::uint16_t port, const std::string& target) {
    std::vector<std::uint8_t> r;
    put16(r, 0); // priority
    put16(r, 0); // weight
    put16(r, port);
    writeName(r, target);
    return r;
}

std::vector<std::uint8_t> ptrRdata(const std::string& target) {
    std::vector<std::uint8_t> r;
    writeName(r, target);
    return r;
}

std::vector<std::uint8_t> aRdata(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return {a, b, c, d};
}

// The whole answer set a GameStream host sends: the service instance, the SRV
// naming the machine, and that machine's address.
std::vector<std::uint8_t> hostReply(const std::string& instance, const std::string& target) {
    std::vector<std::uint8_t> v = header(3);
    appendRr(v, "_nvstream._tcp.local", 12 /*PTR*/, ptrRdata(instance + "._nvstream._tcp.local"));
    appendRr(v, instance + "._nvstream._tcp.local", 33 /*SRV*/, srvRdata(47984, target));
    appendRr(v, target, 1 /*A*/, aRdata(192, 168, 68, 98));
    return v;
}

// A UDP socket that binds proves the sandbox allows the sweep to run at all;
// without one the window assertions would be measuring a refusal.
bool socketsWork() {
    QUdpSocket probe;
    return probe.bind(QHostAddress::LocalHost, 0);
}

} // namespace

TEST_CASE("a reply naming a host resolves to its address", "[moonlight][discovery]") {
    const auto packet = hostReply("Den", "den.local");
    const auto host = detail::parseMoonlightResponse(packet.data(), packet.size());

    REQUIRE(host.has_value());
    CHECK(host->name == QStringLiteral("Den"));
    CHECK(host->address == QStringLiteral("192.168.68.98"));
    // The SRV advertises the HTTPS port; the plaintext one every probe and
    // every pairing phase uses is the GameStream default and is not announced.
    CHECK(host->httpPort == 47989);
    CHECK(host->isValid());
}

TEST_CASE("a reply with no address at all is not a host", "[moonlight][discovery]") {
    // Everything but the A record: a name with nowhere to send anything is not
    // a row, and inventing one would put an unreachable host in the list.
    std::vector<std::uint8_t> v = header(2);
    appendRr(v, "_nvstream._tcp.local", 12, ptrRdata("Den._nvstream._tcp.local"));
    appendRr(v, "Den._nvstream._tcp.local", 33, srvRdata(47984, "den.local"));

    CHECK_FALSE(detail::parseMoonlightResponse(v.data(), v.size()).has_value());
    CHECK_FALSE(detail::parseMoonlightResponse(nullptr, 0).has_value());
    const std::uint8_t truncated[4] = {0, 0, 0x84, 0x00};
    CHECK_FALSE(detail::parseMoonlightResponse(truncated, sizeof(truncated)).has_value());
}

TEST_CASE("the sweep is over when its window is", "[moonlight][discovery]") {
    if (!socketsWork()) { SKIP("no UDP socket to sweep with"); }

    // A window of nothing ends immediately, so a caller can ask for one.
    QElapsedTimer clock;
    clock.start();
    const auto none = MoonlightDiscovery::discover(0);
    const qint64 instant = clock.elapsed();
    CHECK(none.isEmpty());
    CHECK(instant < 2000);

    // And a window that was asked for is waited out rather than cut short:
    // hosts answer at their own pace and a sweep that returned early would
    // report an empty network it never listened to.
    // Nothing is asserted about what it finds: whether a GameStream host
    // answers depends on the network the suite happens to be running on.
    clock.restart();
    static_cast<void>(MoonlightDiscovery::discover(700));
    const qint64 waited = clock.elapsed();
    CHECK(waited >= 600);
    // BOUNDED, and by the number it was given. The screen keeps a spinner up
    // for exactly as long as this call runs.
    CHECK(waited < 6000);
}
