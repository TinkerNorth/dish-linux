// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/http/ConnectionSnapshotGateway.h"

#include "core/diagnostics/HostSnapshot.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dish::source {

namespace {

// A JSON number is a double to QJsonValue; a whole one inside qint64's range is written back as
// an integer, so a count does not come out as 12345.0.
diagnostics::Json numberFromQt(double value) {
    constexpr auto kLowest = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto kHighest = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    const bool whole = std::isfinite(value) && std::trunc(value) == value;
    const bool inRange = value >= kLowest && value < kHighest;
    if (whole && inRange) { return {static_cast<std::int64_t>(value)}; }
    return {value};
}

diagnostics::Json arrayFromQt(const QJsonArray& array) {
    diagnostics::Json out = diagnostics::Json::array();
    for (const auto& item : array) { out.push(jsonFromQt(item)); }
    return out;
}

diagnostics::Json objectFromQt(const QJsonObject& object) {
    diagnostics::Json out = diagnostics::Json::object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        out.set(it.key().toStdString(), jsonFromQt(it.value()));
    }
    return out;
}

} // namespace

diagnostics::Json jsonFromQt(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Bool:
        return {value.toBool()};
    case QJsonValue::Double:
        return numberFromQt(value.toDouble());
    case QJsonValue::String:
        return {value.toString().toStdString()};
    case QJsonValue::Array:
        return arrayFromQt(value.toArray());
    case QJsonValue::Object:
        return objectFromQt(value.toObject());
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return {};
    }
    return {};
}

std::optional<diagnostics::Json> connectionSnapshotFromBody(const QByteArray& body) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { return std::nullopt; }
    const QJsonObject object = document.object();
    const QString format = object.value(QStringLiteral("format")).toString();
    const QString expected =
        QString::fromLatin1(diagnostics::kConnectionSnapshotFormat.data(),
                            static_cast<qsizetype>(diagnostics::kConnectionSnapshotFormat.size()));
    if (format != expected) { return std::nullopt; }
    return objectFromQt(object);
}

ConnectionSnapshotGateway::ConnectionSnapshotGateway(Fetch fetch) : fetch_(std::move(fetch)) {}

void ConnectionSnapshotGateway::fetch(const std::string& connectionKey, Reply reply) {
    fetch_(
        QString::fromStdString(connectionKey), diagnostics::kSnapshotTimeoutMs,
        [reply = std::move(reply)](int status, bool timedOut, const QByteArray& body) {
            reply(composer::SnapshotFetchReply{status, timedOut, connectionSnapshotFromBody(body)});
        });
}

} // namespace dish::source
