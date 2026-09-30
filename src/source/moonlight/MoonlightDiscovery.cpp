// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightDiscovery.h"

#include "source/connection/MdnsDiscovery.h" // net::detail::skipName / readName
#include "source/connection/MdnsScan.h"

#include <arpa/inet.h>

#include <QSet>

#include <cstring>
#include <string>
#include <unordered_map>

namespace dish::source::moon {
namespace {

constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeSrv = 33;
constexpr std::uint16_t kClassInQu = 0x8001;

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::vector<std::uint8_t> buildQuery() {
    std::vector<std::uint8_t> q;
    const std::uint8_t header[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    q.insert(q.end(), header, header + 12);
    for (const char* label : {"_nvstream", "_tcp", "local"}) {
        const auto len = static_cast<std::uint8_t>(std::strlen(label));
        q.push_back(len);
        q.insert(q.end(), label, label + len);
    }
    q.push_back(0);
    q.push_back(static_cast<std::uint8_t>(kTypePtr >> 8));
    q.push_back(static_cast<std::uint8_t>(kTypePtr & 0xFF));
    q.push_back(static_cast<std::uint8_t>(kClassInQu >> 8));
    q.push_back(static_cast<std::uint8_t>(kClassInQu & 0xFF));
    return q;
}

} // namespace

namespace detail {

std::optional<DiscoveredMoonlightHost> parseMoonlightResponse(const std::uint8_t* p,
                                                              std::size_t len) {
    if (len < 12) { return std::nullopt; }
    const std::uint16_t qd = read16(p + 4);
    const std::uint16_t an = read16(p + 6);
    std::size_t pos = 12;

    for (std::uint16_t i = 0; i < qd; ++i) {
        const std::size_t consumed = net::detail::skipName(p, len, pos);
        if (consumed == 0) { return std::nullopt; }
        pos += consumed + 4;
        if (pos > len) { return std::nullopt; }
    }

    std::string instance;
    std::string srvTarget;
    // A GameStream reply packs the SRV (with the host's target name) and that
    // target's A record; resolve one against the other by name.
    std::unordered_map<std::string, std::string> aRecords;

    for (std::uint16_t i = 0; i < an; ++i) {
        std::string owner;
        net::detail::readName(p, len, pos, owner);
        const std::size_t nameLen = net::detail::skipName(p, len, pos);
        if (nameLen == 0) { return std::nullopt; }
        pos += nameLen;
        if (pos + 10 > len) { return std::nullopt; }
        const std::uint16_t type = read16(p + pos);
        const std::uint16_t rdlen = read16(p + pos + 8);
        const std::size_t rdata = pos + 10;
        if (rdata + rdlen > len) { return std::nullopt; }

        if (type == kTypeA && rdlen == 4) {
            char buf[INET_ADDRSTRLEN] = {};
            in_addr a{};
            std::memcpy(&a, p + rdata, 4);
            if (::inet_ntop(AF_INET, &a, buf, sizeof(buf)) != nullptr) {
                aRecords.emplace(owner, buf);
            }
        } else if (type == kTypeSrv && rdlen >= 7) {
            // priority(2) weight(2) port(2), then the target name. The port is
            // the HTTPS one, which the host mapping below does not use.
            std::string target;
            if (net::detail::readName(p, len, rdata + 6, target)) { srvTarget = target; }
        } else if (type == kTypePtr && instance.empty()) {
            std::string n;
            if (net::detail::readName(p, len, rdata, n)) { instance = n.substr(0, n.find('.')); }
        }
        pos = rdata + rdlen;
    }

    std::string address;
    if (!srvTarget.empty()) {
        const auto it = aRecords.find(srvTarget);
        if (it != aRecords.end()) { address = it->second; }
    }
    if (address.empty() && !aRecords.empty()) { address = aRecords.begin()->second; }
    if (address.empty()) { return std::nullopt; }

    DiscoveredMoonlightHost host;
    host.name =
        instance.empty() ? QString::fromStdString(address) : QString::fromStdString(instance);
    host.address = QString::fromStdString(address);
    // The SRV record advertises the HTTPS port; the plain HTTP port is the
    // GameStream default. Hosts do not advertise it, so keep the default.
    host.httpPort = 47989;
    return host;
}

} // namespace detail

QList<DiscoveredMoonlightHost> MoonlightDiscovery::discover(int timeoutMs) {
    QList<DiscoveredMoonlightHost> result;
    QSet<QString> seen;
    // Keyed on the address alone: a Moonlight host serves one session, whatever ports it names.
    net::mdnsScan(buildQuery(), timeoutMs, [&](const std::uint8_t* p, std::size_t n) {
        const auto host = detail::parseMoonlightResponse(p, n);
        if (!host) { return false; }
        if (seen.contains(host->address)) { return false; }
        seen.insert(host->address);
        result.append(*host);
        return true;
    });
    return result;
}

} // namespace dish::source::moon
