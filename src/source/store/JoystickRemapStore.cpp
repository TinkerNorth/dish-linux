// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/JoystickRemapStore.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace dish::source {

QString joystickRemapKeyFor(int vendorId, int productId) {
    return QStringLiteral("%1:%2")
        .arg(static_cast<unsigned>(vendorId) & 0xFFFFu, 4, 16, QLatin1Char('0'))
        .arg(static_cast<unsigned>(productId) & 0xFFFFu, 4, 16, QLatin1Char('0'));
}

namespace {

using input::JoystickRemap;
using input::kRemapButtonCount;
using input::TriggerSourceKind;

// Compact field names — one persisted blob can hold many pads, so keep keys
// short. The shape is forward-compatible: joystickRemapFromJson reads each
// field independently and falls back to the default value when absent/garbled.
constexpr const char* kFieldKey = "vk";      // entry's own vid:pid key (Map<K,V> faithfulness)
constexpr const char* kFieldLsx = "lsx";     // left stick X axis
constexpr const char* kFieldLsy = "lsy";     // left stick Y axis
constexpr const char* kFieldRsx = "rsx";     // right stick X axis
constexpr const char* kFieldRsy = "rsy";     // right stick Y axis
constexpr const char* kFieldInvLy = "ily";   // invert left Y
constexpr const char* kFieldInvRy = "iry";   // invert right Y
constexpr const char* kFieldLtKind = "ltk";  // left trigger source kind (0 Axis / 1 Button)
constexpr const char* kFieldLtIdx = "lti";   // left trigger source index
constexpr const char* kFieldRtKind = "rtk";  // right trigger source kind
constexpr const char* kFieldRtIdx = "rti";   // right trigger source index
constexpr const char* kFieldButtons = "btn"; // logical-button -> source array
constexpr const char* kFieldHat = "hat";     // hat index
constexpr const char* kFieldAdaptRs = "ars"; // adaptive right stick flag
constexpr const char* kFieldAdaptTr = "atr"; // adaptive triggers flag

int triggerKindToInt(TriggerSourceKind k) { return k == TriggerSourceKind::Button ? 1 : 0; }
TriggerSourceKind triggerKindFromInt(int v) {
    return v == 1 ? TriggerSourceKind::Button : TriggerSourceKind::Axis;
}

// Read an int field, defaulting to `fallback` when the value is absent or not a
// number (forward-compat: a newer/garbled blob never breaks an older read).
int intOr(const QJsonObject& obj, const char* field, int fallback) {
    const auto v = obj.value(QLatin1String(field));
    return v.isDouble() ? v.toInt() : fallback;
}

bool boolOr(const QJsonObject& obj, const char* field, bool fallback) {
    const auto v = obj.value(QLatin1String(field));
    return v.isBool() ? v.toBool() : fallback;
}

} // namespace

QJsonObject joystickRemapToJson(const JoystickRemap& remap) {
    QJsonArray buttons;
    for (int i = 0; i < kRemapButtonCount; ++i) {
        buttons.append(remap.buttons[static_cast<std::size_t>(i)]);
    }
    return QJsonObject{
        {kFieldLsx, remap.leftStickX},
        {kFieldLsy, remap.leftStickY},
        {kFieldRsx, remap.rightStickX},
        {kFieldRsy, remap.rightStickY},
        {kFieldInvLy, remap.invertLeftY},
        {kFieldInvRy, remap.invertRightY},
        {kFieldLtKind, triggerKindToInt(remap.leftTrigger.kind)},
        {kFieldLtIdx, remap.leftTrigger.index},
        {kFieldRtKind, triggerKindToInt(remap.rightTrigger.kind)},
        {kFieldRtIdx, remap.rightTrigger.index},
        {kFieldButtons, buttons},
        {kFieldHat, remap.hatIndex},
        {kFieldAdaptRs, remap.useAdaptiveRightStick},
        {kFieldAdaptTr, remap.useAdaptiveTriggers},
    };
}

input::JoystickRemap joystickRemapFromJson(const QJsonValue& v) {
    // Start from the default so every field has a sane fallback; overlay only
    // the fields the blob actually carries in a known-good shape.
    JoystickRemap remap{}; // the historical default layout
    if (!v.isObject()) { return remap; }
    const auto obj = v.toObject();

    remap.leftStickX = intOr(obj, kFieldLsx, remap.leftStickX);
    remap.leftStickY = intOr(obj, kFieldLsy, remap.leftStickY);
    remap.rightStickX = intOr(obj, kFieldRsx, remap.rightStickX);
    remap.rightStickY = intOr(obj, kFieldRsy, remap.rightStickY);
    remap.invertLeftY = boolOr(obj, kFieldInvLy, remap.invertLeftY);
    remap.invertRightY = boolOr(obj, kFieldInvRy, remap.invertRightY);
    remap.leftTrigger.kind =
        triggerKindFromInt(intOr(obj, kFieldLtKind, triggerKindToInt(remap.leftTrigger.kind)));
    remap.leftTrigger.index = intOr(obj, kFieldLtIdx, remap.leftTrigger.index);
    remap.rightTrigger.kind =
        triggerKindFromInt(intOr(obj, kFieldRtKind, triggerKindToInt(remap.rightTrigger.kind)));
    remap.rightTrigger.index = intOr(obj, kFieldRtIdx, remap.rightTrigger.index);
    remap.hatIndex = intOr(obj, kFieldHat, remap.hatIndex);
    remap.useAdaptiveRightStick = boolOr(obj, kFieldAdaptRs, remap.useAdaptiveRightStick);
    remap.useAdaptiveTriggers = boolOr(obj, kFieldAdaptTr, remap.useAdaptiveTriggers);

    // Buttons array: only overlay a well-formed, correctly-sized array. A
    // wrong-length or non-array value leaves the default button map intact (a
    // newer build that added a logical button must not corrupt this build's).
    const auto bv = obj.value(QLatin1String(kFieldButtons));
    if (bv.isArray()) {
        const auto arr = bv.toArray();
        if (arr.size() == kRemapButtonCount) {
            for (int i = 0; i < kRemapButtonCount; ++i) {
                const auto e = arr.at(i);
                if (e.isDouble()) { remap.buttons[static_cast<std::size_t>(i)] = e.toInt(); }
            }
        }
    }
    return remap;
}

// ── Repository ──────────────────────────────────────────────────────────────

JoystickRemapRepository::JoystickRemapRepository(std::shared_ptr<QSettings> settings)
    : settings_(settings
                    ? std::move(settings)
                    : std::make_shared<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {
}

namespace {

// Decode one persisted value. The faithful form is an object carrying the
// entry's own key (`vk`) plus the remap fields; an object missing `vk` falls
// back to the storage key. A non-object value decodes to the storage key with
// the DEFAULT remap (forward-compat — never drop a key, never crash).
JoystickRemapEntry decodeValue(const QString& storageKey, const QJsonValue& v) {
    const QString vk = (v.isObject() && v.toObject().contains(QLatin1String(kFieldKey)))
                           ? v.toObject().value(QLatin1String(kFieldKey)).toString()
                           : storageKey;
    return JoystickRemapEntry{vk, joystickRemapFromJson(v)};
}

} // namespace

std::map<QString, JoystickRemapEntry> JoystickRemapRepository::readMap() const {
    const auto raw = settings_->value(QLatin1String(kRemapsKey)).toByteArray();
    if (raw.isEmpty()) { return {}; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(raw, &err);
    // Corrupt blob / wrong shape -> empty (don't crash on garbled prefs).
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return {}; }

    std::map<QString, JoystickRemapEntry> out;
    const auto obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        out.emplace(it.key(), decodeValue(it.key(), it.value()));
    }
    return out;
}

void JoystickRemapRepository::writeMap(const std::map<QString, JoystickRemapEntry>& map) {
    QJsonObject obj;
    for (const auto& [key, entry] : map) {
        QJsonObject o = joystickRemapToJson(entry.remap);
        o.insert(QLatin1String(kFieldKey), entry.key); // preserve the entry's own key verbatim
        obj.insert(key, o);
    }
    settings_->setValue(QLatin1String(kRemapsKey),
                        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

std::optional<JoystickRemapEntry> JoystickRemapRepository::get(const QString& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto map = readMap();
    const auto it = map.find(key);
    if (it == map.end()) { return std::nullopt; }
    return it->second;
}

std::vector<JoystickRemapEntry> JoystickRemapRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JoystickRemapEntry> out;
    for (const auto& [key, entry] : readMap()) { out.push_back(entry); }
    return out;
}

void JoystickRemapRepository::put(const QString& key, const JoystickRemapEntry& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto map = readMap();
    map[key] = value;
    writeMap(map);
}

void JoystickRemapRepository::remove(const QString& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto map = readMap();
    map.erase(key);
    writeMap(map);
}

void JoystickRemapRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(QLatin1String(kRemapsKey));
}

// ── StateSource ───────────────────────────────────────────────────────────--

JoystickRemapMap JoystickRemapStore::hydrate(JoystickRemapRepository* repo) {
    JoystickRemapMap out;
    if (repo == nullptr) { return out; }
    for (const auto& e : repo->all()) { out[e.key.toStdString()] = e.remap; }
    return out;
}

JoystickRemapStore::JoystickRemapStore(JoystickRemapRepository* repo)
    : arch::StateSource<JoystickRemapMap>(hydrate(repo)), repo_(repo) {}

std::optional<input::JoystickRemap> JoystickRemapStore::remapFor(int vendorId,
                                                                 int productId) const {
    const auto& snapshot = state().value();
    const auto it = snapshot.find(joystickRemapKeyFor(vendorId, productId).toStdString());
    if (it == snapshot.end()) { return std::nullopt; }
    return it->second;
}

void JoystickRemapStore::setRemap(int vendorId, int productId, const input::JoystickRemap& remap) {
    const std::string key = joystickRemapKeyFor(vendorId, productId).toStdString();
    // Short-circuit an unchanged write so the Observable does not re-emit.
    const auto& current = state().value();
    const auto it = current.find(key);
    if (it != current.end() && it->second == remap) { return; }

    if (repo_ != nullptr) {
        repo_->put(JoystickRemapEntry{joystickRemapKeyFor(vendorId, productId), remap});
    }
    setState([&](const JoystickRemapMap& cur) {
        JoystickRemapMap next = cur;
        next[key] = remap;
        return next;
    });
}

void JoystickRemapStore::clearRemap(int vendorId, int productId) {
    const std::string key = joystickRemapKeyFor(vendorId, productId).toStdString();
    if (repo_ != nullptr) { repo_->remove(joystickRemapKeyFor(vendorId, productId)); }
    setState([&](const JoystickRemapMap& cur) {
        if (cur.find(key) == cur.end()) { return cur; }
        JoystickRemapMap next = cur;
        next.erase(key);
        return next;
    });
}

} // namespace dish::source
