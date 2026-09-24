// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "MdnsDiscovery.h"

#include "MdnsScan.h"

#include <arpa/inet.h>

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace dish::net {

namespace {

constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeTxt = 16;
constexpr std::uint16_t kTypeSrv = 33;

// IN class with the QU (unicast-response) bit set — RFC 6762 §5.4. The
// responder then answers straight to our source port, so a one-shot client
// never has to join the multicast group to receive.
constexpr std::uint16_t kClassInQu = 0x8001;

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// The PTR query for `_satellite._udp.local.`: 12-byte header + the question
// (length-prefixed labels + type + class).
std::vector<std::uint8_t> buildQuery() {
    std::vector<std::uint8_t> q;
    const std::uint8_t header[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}; // qdcount = 1
    q.insert(q.end(), header, header + 12);
    for (const char* label : {"_satellite", "_udp", "local"}) {
        const auto len = static_cast<std::uint8_t>(std::strlen(label));
        q.push_back(len);
        q.insert(q.end(), label, label + len);
    }
    q.push_back(0); // root label
    q.push_back(static_cast<std::uint8_t>(kTypePtr >> 8));
    q.push_back(static_cast<std::uint8_t>(kTypePtr & 0xFF));
    q.push_back(static_cast<std::uint8_t>(kClassInQu >> 8));
    q.push_back(static_cast<std::uint8_t>(kClassInQu & 0xFF));
    return q;
}

} // namespace

namespace detail {

std::size_t skipName(const std::uint8_t* p, std::size_t len, std::size_t off) {
    std::size_t consumed = 0;
    bool jumped = false;
    std::size_t guard = 0;
    while (off < len) {
        if (++guard > len) { return 0; } // malformed: loop guard
        const std::uint8_t b = p[off];
        if (b == 0) {
            if (!jumped) { consumed += 1; }
            return consumed;
        }
        if ((b & 0xC0) == 0xC0) { // compression pointer
            if (off + 1 >= len) { return 0; }
            if (!jumped) { consumed += 2; }
            const std::size_t target = (static_cast<std::size_t>(b & 0x3F) << 8) | p[off + 1];
            if (target >= off) { return 0; } // only backward jumps
            off = target;
            jumped = true;
            continue;
        }
        const std::size_t label = b + 1;
        if (off + label > len) { return 0; }
        if (!jumped) { consumed += label; }
        off += label;
    }
    return 0;
}

bool readName(const std::uint8_t* p, std::size_t len, std::size_t off, std::string& out) {
    out.clear();
    std::size_t guard = 0;
    while (off < len) {
        if (++guard > len) { return false; }
        const std::uint8_t b = p[off];
        if (b == 0) { return true; }
        if ((b & 0xC0) == 0xC0) {
            if (off + 1 >= len) { return false; }
            const std::size_t target = (static_cast<std::size_t>(b & 0x3F) << 8) | p[off + 1];
            if (target >= off) { return false; }
            off = target;
            continue;
        }
        if (off + 1 + b > len) { return false; }
        if (!out.empty()) { out += '.'; }
        out.append(reinterpret_cast<const char*>(p + off + 1), b);
        off += b + 1;
    }
    return false;
}

// The responder packs SRV + A + TXT into a single packet, so a per-packet parse
// is sufficient — no cross-packet record assembly needed.
// What the answer section contributed. srvPort 0 means no SRV was seen; the mapping layer applies
// its own precedence over that.
struct MdnsAnswers {
    std::string ip;
    std::string instance;
    int srvPort = 0;
    QHash<QString, QByteArray> txt;
    bool haveSrv = false;
    bool haveTxt = false;
};

// False when the section is malformed, which makes the whole response unusable rather than
// partially read.
bool skipQuestions(const std::uint8_t* p, std::size_t len, std::uint16_t count, std::size_t& pos) {
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t consumed = skipName(p, len, pos);
        if (consumed == 0) { return false; }
        pos += consumed + 4; // + type + class
        if (pos > len) { return false; }
    }
    return true;
}

void readTxtRecord(const std::uint8_t* p, std::size_t rdata, std::uint16_t rdlen,
                   MdnsAnswers& out) {
    std::size_t t = rdata;
    const std::size_t end = rdata + rdlen;
    while (t < end) {
        const std::uint8_t slen = p[t];
        if (t + 1 + slen > end) { break; }
        const std::string entry(reinterpret_cast<const char*>(p + t + 1), slen);
        const auto eq = entry.find('=');
        if (eq != std::string::npos) {
            out.txt.insert(QString::fromStdString(entry.substr(0, eq)),
                           QByteArray::fromStdString(entry.substr(eq + 1)));
        }
        t += 1 + slen;
        out.haveTxt = true;
    }
}

void readARecord(const std::uint8_t* p, std::size_t rdata, MdnsAnswers& out) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a{};
    std::memcpy(&a, p + rdata, 4);
    if (::inet_ntop(AF_INET, &a, buf, sizeof(buf)) != nullptr) { out.ip = buf; }
}

// One record of whichever kind. Records this discovery does not use are stepped over.
void readAnswerRecord(const std::uint8_t* p, std::size_t len, std::uint16_t type, std::size_t rdata,
                      std::uint16_t rdlen, MdnsAnswers& out) {
    if (type == kTypeA && rdlen == 4) {
        readARecord(p, rdata, out);
    } else if (type == kTypeSrv && rdlen >= 7) {
        out.srvPort = read16(p + rdata + 4); // priority(2) weight(2) port(2)
        out.haveSrv = true;
    } else if (type == kTypeTxt) {
        readTxtRecord(p, rdata, rdlen, out);
    } else if (type == kTypePtr && out.instance.empty()) {
        std::string n;
        // The instance label is the first component of the PTR target.
        if (readName(p, len, rdata, n)) { out.instance = n.substr(0, n.find('.')); }
    }
}

// Null when any record is malformed: a truncated answer section means the rest of the datagram
// cannot be trusted to start where the length says.
std::optional<MdnsAnswers> readAnswers(const std::uint8_t* p, std::size_t len, std::uint16_t count,
                                       std::size_t pos) {
    MdnsAnswers out;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t nameLen = skipName(p, len, pos);
        if (nameLen == 0) { return std::nullopt; }
        pos += nameLen;
        if (pos + 10 > len) { return std::nullopt; }
        const std::uint16_t type = read16(p + pos);
        const std::uint16_t rdlen = read16(p + pos + 8);
        const std::size_t rdata = pos + 10;
        if (rdata + rdlen > len) { return std::nullopt; }
        readAnswerRecord(p, len, type, rdata, rdlen, out);
        pos = rdata + rdlen;
    }
    return out;
}

std::optional<models::DiscoveredServer> parseResponse(const std::uint8_t* p, std::size_t len) {
    if (len < 12) { return std::nullopt; }
    const std::uint16_t qd = read16(p + 4);
    const std::uint16_t an = read16(p + 6);

    std::size_t pos = 12;
    if (!skipQuestions(p, len, qd, pos)) { return std::nullopt; }
    const auto answers = readAnswers(p, len, an, pos);
    if (!answers.has_value()) { return std::nullopt; }

    // An address with nothing describing the service behind it is not a hit: the mapping layer
    // needs at least one of the SRV port or the TXT record to place it.
    if (answers->ip.empty() || (!answers->haveSrv && !answers->haveTxt)) { return std::nullopt; }
    return mdnsServiceToServer(QString::fromStdString(answers->instance),
                               QString::fromStdString(answers->ip), answers->srvPort, answers->txt);
}

} // namespace detail

std::optional<int> mdnsTxtInt(const QHash<QString, QByteArray>& txt, const QString& key) {
    const auto it = txt.constFind(key);
    if (it == txt.constEnd() || it->isNull()) { return std::nullopt; }
    const QString trimmed = QString::fromUtf8(*it).trimmed();
    if (trimmed.isEmpty()) { return std::nullopt; }
    bool ok = false;
    const int value = trimmed.toInt(&ok);
    if (!ok) { return std::nullopt; }
    return value;
}

std::optional<QString> mdnsTxtString(const QHash<QString, QByteArray>& txt, const QString& key) {
    const auto it = txt.constFind(key);
    if (it == txt.constEnd() || it->isNull()) { return std::nullopt; }
    // Non-const on purpose: const blocks the implicit move into the returned
    // optional and costs a QString copy on every hit.
    QString trimmed = QString::fromUtf8(*it).trimmed();
    if (trimmed.isEmpty()) { return std::nullopt; }
    return trimmed;
}

std::optional<models::DiscoveredServer> mdnsServiceToServer(const QString& serviceName,
                                                            const QString& hostAddress, int srvPort,
                                                            const QHash<QString, QByteArray>& txt) {
    if (hostAddress.isEmpty()) { return std::nullopt; } // null host → nothing to connect to

    models::DiscoveredServer s;
    s.name = serviceName.isEmpty() ? hostAddress : serviceName;
    s.ip = hostAddress;
    if (const auto txtUdp = mdnsTxtInt(txt, QStringLiteral("udp"))) {
        s.udpPort = *txtUdp;
    } else if (srvPort > 0) {
        s.udpPort = srvPort;
    } else {
        s.udpPort = kMdnsDefaultUdp;
    }
    s.pairPort = mdnsTxtInt(txt, QStringLiteral("pair")).value_or(kMdnsDefaultPair);
    s.httpPort = mdnsTxtInt(txt, QStringLiteral("http")).value_or(kMdnsDefaultHttp);
    s.machineId = mdnsTxtString(txt, QStringLiteral("mid")).value_or(QString());
    s.source = models::DiscoverySource::Mdns;
    return s;
}

QList<models::DiscoveredServer> MdnsDiscovery::discover(int timeoutMs) {
    QList<models::DiscoveredServer> result;
    QSet<QString> seen;
    // A satellite answers for one ip+port pair; the same box on two interfaces is two servers.
    mdnsScan(buildQuery(), timeoutMs, [&](const std::uint8_t* p, std::size_t n) {
        const auto server = detail::parseResponse(p, n);
        if (!server) { return false; }
        const QString key = server->ip + QStringLiteral(":") + QString::number(server->udpPort);
        if (seen.contains(key)) { return false; }
        seen.insert(key);
        result.append(*server);
        return true;
    });
    return result;
}

} // namespace dish::net
