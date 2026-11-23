// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/controllerdb/ControllerDbJson.h"

#include "core/controllerdb/DbFetch.h"
#include "core/controllerdb/Vocabulary.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dish::source::controllerdb {

namespace cdb = dish::controllerdb;

namespace {

std::string utf8(const QJsonValue& value) { return value.toString().toStdString(); }

QJsonValue member(const QJsonObject& object, const char* key) {
    return object.value(QLatin1String(key));
}

std::optional<QJsonObject> parseRoot(const QByteArray& body, std::int64_t maxBytes) {
    if (body.size() > maxBytes) { return std::nullopt; }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) { return std::nullopt; }
    return doc.object();
}

bool schemaIsOne(const QJsonObject& object) {
    const QJsonValue schema = member(object, "schema");
    return schema.isDouble() && schema.toInt(-1) == cdb::kSchema;
}

std::optional<int> positiveInt(const QJsonValue& value) {
    if (!value.isDouble()) { return std::nullopt; }
    const int number = value.toInt(0);
    if (number <= 0) { return std::nullopt; }
    return number;
}

std::optional<std::vector<std::string>> stringList(const QJsonValue& value) {
    if (!value.isArray()) { return std::nullopt; }
    std::vector<std::string> out;
    for (const auto& item : value.toArray()) {
        if (!item.isString()) { return std::nullopt; }
        out.push_back(utf8(item));
    }
    return out;
}

std::optional<std::vector<cdb::DeviceId>> idList(const QJsonValue& value) {
    const std::optional<std::vector<std::string>> texts = stringList(value);
    if (!texts.has_value()) { return std::nullopt; }
    std::vector<cdb::DeviceId> out;
    for (const std::string& text : *texts) {
        const std::optional<cdb::DeviceId> id = cdb::parseDeviceId(text);
        if (!id.has_value()) { return std::nullopt; }
        out.push_back(*id);
    }
    return out;
}

template <class E, std::size_t N>
std::optional<E> token(const std::array<cdb::Token<E>, N>& table, const QJsonValue& value) {
    if (!value.isString()) { return std::nullopt; }
    return cdb::fromToken(table, utf8(value));
}

std::optional<cdb::DirectRule> directRuleOf(const QJsonValue& direct) {
    return token(cdb::kDirectRuleTokens, direct.toObject().value(QLatin1String("rule")));
}

std::optional<cdb::DirectRule> platformRuleOf(const QJsonObject& record) {
    const QJsonValue platforms = member(record, "platforms");
    const QJsonValue mine = platforms.toObject().value(QLatin1String(cdb::kPlatformName));
    const QJsonValue direct = mine.toObject().value(QLatin1String("direct"));
    if (!direct.isObject()) { return std::nullopt; }
    return directRuleOf(direct);
}

cdb::Expectation expectationOf(const QJsonValue& value) {
    if (!value.isBool()) { return cdb::Expectation::Unknown; }
    return value.toBool() ? cdb::Expectation::Present : cdb::Expectation::Absent;
}

cdb::InputCapabilities inputCapsOf(const QJsonObject& caps) {
    cdb::InputCapabilities out;
    out.motion = member(caps, "motion").toBool();
    out.touchpad = member(caps, "touchpad").toBool();
    out.trackpads = member(caps, "trackpads").toInt(0);
    out.battery = member(caps, "battery").toBool();
    out.extendedButtons = member(caps, "extendedButtons").toBool();
    return out;
}

cdb::OutputCapabilities outputCapsOf(const QJsonObject& caps) {
    cdb::OutputCapabilities out;
    out.rumble = member(caps, "rumble").toBool();
    out.triggerRumble = member(caps, "triggerRumble").toBool();
    out.lightbar = member(caps, "lightbar").toBool();
    out.playerLeds = member(caps, "playerLeds").toBool();
    out.micLed = member(caps, "micLed").toBool();
    out.triggerEffects = member(caps, "triggerEffects").toBool();
    return out;
}

// An os entry's version is an integer or a string in the schema; both read as its text.
std::vector<cdb::IssueOs> issueOsOf(const QJsonValue& value) {
    std::vector<cdb::IssueOs> out;
    const QJsonObject os = value.toObject();
    for (auto it = os.begin(); it != os.end(); ++it) {
        const QJsonValue version = it.value();
        const std::string text =
            version.isDouble() ? std::to_string(version.toInt()) : version.toString().toStdString();
        out.push_back(cdb::IssueOs{it.key().toStdString(), text});
    }
    return out;
}

std::optional<cdb::Issue> issueOf(const QJsonValue& value) {
    const QJsonObject issue = value.toObject();
    const std::optional<cdb::Transport> transport =
        token(cdb::kTransportTokens, member(issue, "transport"));
    const QJsonValue id = member(issue, "id");
    if (!id.isString() || !transport.has_value()) { return std::nullopt; }
    cdb::Issue out;
    out.id = utf8(id);
    const QJsonValue device = member(issue, "device");
    if (device.isString()) { out.device = utf8(device); }
    out.os = issueOsOf(member(issue, "os"));
    out.transport = *transport;
    out.lane = utf8(member(issue, "lane"));
    out.status = utf8(member(issue, "status"));
    return out;
}

bool readRequires(const QJsonObject& record, cdb::ControllerRecord& out) {
    const QJsonObject requires_ = member(record, "requires").toObject();
    auto families = stringList(member(requires_, "families"));
    auto ops = stringList(member(requires_, "ops"));
    if (!families.has_value() || !ops.has_value()) { return false; }
    out.requiresFamilies = std::move(*families);
    out.requiresOps = std::move(*ops);
    return true;
}

bool readIdentity(const QJsonObject& record, cdb::ControllerRecord& out) {
    const QJsonObject identity = member(record, "identity").toObject();
    auto usb = idList(member(identity, "usb"));
    auto bluetooth = idList(member(identity, "bluetooth"));
    if (!usb.has_value() || !bluetooth.has_value()) { return false; }
    out.usb = std::move(*usb);
    out.bluetooth = std::move(*bluetooth);
    return true;
}

bool readInput(const QJsonObject& record, cdb::ControllerRecord& out) {
    const QJsonObject input = member(record, "input").toObject();
    const QJsonObject protocol = member(input, "protocol").toObject();
    const QJsonValue ref = member(protocol, "ref");
    const std::optional<cdb::DirectRule> rule = directRuleOf(member(input, "direct"));
    if (!ref.isString() || !rule.has_value()) { return false; }
    out.family = cdb::familyFrom(utf8(ref));
    const QJsonValue mapping = member(input, "mapping");
    out.mapping = mapping.isString() ? cdb::mappingFrom(utf8(mapping)) : cdb::Mapping::None;
    out.streamsUnprompted = member(protocol, "streamsUnprompted").toBool();
    out.directRule = cdb::directRuleFor(platformRuleOf(record), *rule);
    out.input = inputCapsOf(member(input, "capabilities").toObject());
    return true;
}

bool readOutput(const QJsonObject& record, cdb::ControllerRecord& out) {
    const QJsonObject output = member(record, "output").toObject();
    const QJsonValue route = member(output, "route");
    const QJsonValue kind = member(output, "init").toObject().value(QLatin1String("kind"));
    if (!kind.isString()) { return false; }
    out.route = route.isNull() ? cdb::OutputRoute::None : cdb::routeFrom(utf8(route));
    out.init = cdb::initFrom(utf8(kind));
    out.output = outputCapsOf(member(output, "capabilities").toObject());
    return true;
}

void readExpect(const QJsonObject& record, cdb::ControllerRecord& out) {
    const QJsonObject endpoints =
        member(record, "expect").toObject().value(QLatin1String("endpoints")).toObject();
    out.expect.endpointIn = expectationOf(member(endpoints, "in"));
    out.expect.endpointOut = expectationOf(member(endpoints, "out"));
}

bool readSupport(const QJsonObject& record, cdb::ControllerRecord& out) {
    const QJsonObject support = member(record, "support").toObject();
    const QJsonObject verification = member(support, "verification").toObject();
    const auto state = token(cdb::kVerificationTokens, member(verification, "state"));
    const auto transport = token(cdb::kTransportTokens, member(verification, "transport"));
    for (const auto& item : member(support, "issues").toArray()) {
        if (auto issue = issueOf(item)) { out.issues.push_back(std::move(*issue)); }
    }
    if (!state.has_value() || !transport.has_value()) { return false; }
    out.verification = *state;
    out.verificationTransport = *transport;
    return true;
}

// The id and the identity index the row, so a row without them is dropped. Every other section
// is read in the order the contract lists them; one that does not decode keeps the row, inert,
// because a later database's new word must not take a pad's own bundled row away (section 6.6).
std::optional<cdb::ControllerRecord> recordOf(const QJsonValue& value) {
    if (!value.isObject()) { return std::nullopt; }
    const QJsonObject record = value.toObject();
    const QJsonValue id = member(record, "id");
    if (!schemaIsOne(record) || !id.isString()) { return std::nullopt; }
    cdb::ControllerRecord out;
    out.id = utf8(id);
    if (!readIdentity(record, out)) { return std::nullopt; }
    const bool requiresRead = readRequires(record, out);
    const bool inputRead = readInput(record, out);
    const bool outputRead = readOutput(record, out);
    const bool supportRead = readSupport(record, out);
    out.decoded = requiresRead && inputRead && outputRead && supportRead;
    readExpect(record, out);
    return out;
}

} // namespace

std::variant<cdb::ControllerDb, DbParseError> parseControllerDb(const QByteArray& body) {
    if (body.size() > cdb::kFileMaxBytes) { return DbParseError::Oversize; }
    const std::optional<QJsonObject> root = parseRoot(body, cdb::kFileMaxBytes);
    if (!root.has_value()) { return DbParseError::BadJson; }
    if (!schemaIsOne(*root)) { return DbParseError::UnsupportedSchema; }
    const std::optional<int> version = positiveInt(member(*root, "dbVersion"));
    if (!version.has_value()) { return DbParseError::BadVersion; }
    const QJsonValue records = member(*root, "records");
    if (!records.isArray()) { return DbParseError::BadShape; }

    cdb::ControllerDb db;
    db.dbVersion = *version;
    for (const auto& item : records.toArray()) {
        if (auto record = recordOf(item)) {
            db.records.push_back(std::move(*record));
        } else {
            ++db.droppedRows;
        }
    }
    return db;
}

std::variant<cdb::StringsTable, DbParseError> parseStringsTable(const QByteArray& body) {
    if (body.size() > cdb::kFileMaxBytes) { return DbParseError::Oversize; }
    const std::optional<QJsonObject> root = parseRoot(body, cdb::kFileMaxBytes);
    if (!root.has_value()) { return DbParseError::BadJson; }
    cdb::StringsTable table;
    for (auto it = root->begin(); it != root->end(); ++it) {
        if (!it.value().isString()) { return DbParseError::BadShape; }
        table.emplace(it.key().toStdString(), it.value().toString().toStdString());
    }
    return table;
}

std::variant<cdb::DbManifest, DbParseError> parseDbManifest(const QByteArray& body) {
    if (body.size() > cdb::kManifestMaxBytes) { return DbParseError::Oversize; }
    const std::optional<QJsonObject> root = parseRoot(body, cdb::kManifestMaxBytes);
    if (!root.has_value()) { return DbParseError::BadJson; }
    if (!schemaIsOne(*root)) { return DbParseError::UnsupportedSchema; }
    const std::optional<int> version = positiveInt(member(*root, "dbVersion"));
    if (!version.has_value()) { return DbParseError::BadVersion; }
    const QJsonValue files = member(*root, "files");
    if (!files.isObject()) { return DbParseError::BadShape; }

    cdb::DbManifest manifest;
    manifest.dbVersion = *version;
    manifest.publishedAt = utf8(member(*root, "publishedAt"));
    const QJsonObject entries = files.toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const QJsonValue bytes = member(entry, "bytes");
        if (!bytes.isDouble()) { return DbParseError::BadShape; }
        manifest.files.push_back(cdb::ManifestFile{it.key().toStdString(),
                                                   utf8(member(entry, "sha256")),
                                                   static_cast<std::int64_t>(bytes.toDouble())});
    }
    return manifest;
}

} // namespace dish::source::controllerdb
