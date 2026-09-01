// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Models.h"

namespace dish::models {

namespace {

QString optString(const QJsonObject& obj, const char* key) {
    const auto v = obj.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

int intOr(const QJsonObject& obj, const char* key, int fallback) {
    const auto v = obj.value(QLatin1String(key));
    if (v.isDouble()) { return v.toInt(fallback); }
    return fallback;
}

bool boolOr(const QJsonObject& obj, const char* key, bool fallback) {
    const auto v = obj.value(QLatin1String(key));
    if (v.isBool()) { return v.toBool(fallback); }
    return fallback;
}

void setIfNonEmpty(std::optional<QString>& slot, const QJsonObject& obj, const char* key) {
    if (auto v = optString(obj, key); !v.isEmpty()) { slot = v; }
}

// Inverse of ControllerDescriptor::toJson's caps fold. Unknown keys are ignored
// so a newer server can add caps without breaking older clients.
std::uint16_t capsWordFromJson(const QJsonObject& caps) {
    std::uint16_t word = 0;
    if (boolOr(caps, "rumble", false)) { word |= proto::kCapRumble; }
    if (boolOr(caps, "motion", false)) { word |= proto::kCapMotion; }
    if (boolOr(caps, "analogTriggers", false)) { word |= proto::kCapAnalogTriggers; }
    if (boolOr(caps, "lightbar", false)) { word |= proto::kCapLightbar; }
    if (boolOr(caps, "triggerEffects", false)) { word |= proto::kCapTriggerEffects; }
    if (boolOr(caps, "playerLeds", false)) { word |= proto::kCapPlayerLeds; }
    return word;
}

HostCapabilityDto hostCapabilityFromJson(const QJsonObject& obj) {
    HostCapabilityDto d;
    d.supported = boolOr(obj, "supported", false);
    const auto avail = obj.value(QLatin1String("available"));
    if (avail.isBool()) { d.available = avail.toBool(); }
    return d;
}

} // namespace

QJsonObject DiscoveredServer::toJson() const {
    return QJsonObject{
        {"name", name},         {"ip", ip},
        {"udpPort", udpPort},   {"pairPort", pairPort},
        {"httpPort", httpPort}, {"machineId", machineId},
    };
}

DiscoveredServer DiscoveredServer::fromJson(const QJsonObject& obj) {
    DiscoveredServer s;
    s.name = optString(obj, "name");
    s.ip = optString(obj, "ip");
    s.udpPort = intOr(obj, "udpPort", kDefaultUdpPort);
    s.pairPort = intOr(obj, "pairPort", kDefaultPairPort);
    s.httpPort = intOr(obj, "httpPort", kDefaultHttpPort);
    // The beacon calls it "machineId", mDNS TXT calls it "mid". Accept either so
    // both discovery transports populate the keying identity.
    s.machineId = optString(obj, "machineId");
    if (s.machineId.isEmpty()) { s.machineId = optString(obj, "mid"); }
    return s;
}

PairResponse PairResponse::fromJson(const QJsonObject& obj) {
    PairResponse r;
    r.ok = boolOr(obj, "ok", false);
    r.pending = boolOr(obj, "pending", false);
    setIfNonEmpty(r.error, obj, "error");
    setIfNonEmpty(r.sharedKey, obj, "sharedKey");
    r.protocolVersion = intOr(obj, "protocolVersion", proto::kProtocolVersionMin);
    // Only on the 409 body; 0 means the server did not name a range.
    r.supportedProtocol = intOr(obj, "supported", 0);
    r.supportedProtocolMin = intOr(obj, "supportedMin", 0);
    // A JSON body parsed, so the server is reachable even when ok=false.
    // PairingClient sets reachable=false on every network-error path.
    r.reachable = true;
    return r;
}

PairResponse PairResponse::fromStatusJson(const QJsonObject& obj) {
    PairResponse r;
    r.ok = boolOr(obj, "ok", false);
    if (auto st = optString(obj, "status"); !st.isEmpty()) { r.status = st; }
    setIfNonEmpty(r.sharedKey, obj, "sharedKey");
    setIfNonEmpty(r.error, obj, "error");
    r.reachable = true;
    return r;
}

ControllerApplyDto ControllerApplyDto::fromJson(const QJsonObject& obj) {
    ControllerApplyDto d;
    d.ctrlIdx = intOr(obj, "ctrlIdx", 0);
    d.result = optString(obj, "result");
    d.resultCode = proto::applyResultFromName(d.result.toStdString());
    d.appliedType = intOr(obj, "appliedType", proto::kControllerTypeXbox);
    const auto motion = obj.value(QLatin1String("motion")).toObject();
    d.motionSinkSupportedForType = boolOr(motion, "sinkSupportedForType", false);
    d.motionBackendOk = boolOr(motion, "backendOk", false);
    return d;
}

HostFeatureGrant HostFeatureGrant::fromJson(const QJsonObject& obj) {
    HostFeatureGrant g;
    g.granted = boolOr(obj, "granted", false);
    if (auto reason = optString(obj, "reason"); !reason.isEmpty()) { g.reason = reason; }
    return g;
}

SessionResponse SessionResponse::fromJson(const QJsonObject& obj) {
    SessionResponse r;
    setIfNonEmpty(r.connectionId, obj, "connectionId");
    setIfNonEmpty(r.token, obj, "token");
    setIfNonEmpty(r.sessionSalt, obj, "sessionSalt");
    r.epoch = intOr(obj, "epoch", 0);
    r.maxControllers = intOr(obj, "maxControllers", 16);
    r.protocolVersion = intOr(obj, "protocolVersion", proto::kProtocolVersionMin);
    // Only on the 409 body; 0 means the server did not name a range.
    r.supportedProtocol = intOr(obj, "supported", 0);
    r.supportedProtocolMin = intOr(obj, "supportedMin", 0);
    for (const auto& v : obj.value(QLatin1String("controllers")).toArray()) {
        if (v.isObject()) { r.controllers.append(ControllerApplyDto::fromJson(v.toObject())); }
    }
    const auto hf = obj.value(QLatin1String("hostFeatures")).toObject();
    r.mouseControl = HostFeatureGrant::fromJson(hf.value(QLatin1String("mouseControl")).toObject());
    setIfNonEmpty(r.error, obj, "error");
    setIfNonEmpty(r.code, obj, "code");
    r.reachable = true;
    return r;
}

ControllerPutResponse ControllerPutResponse::fromJson(const QJsonObject& obj) {
    ControllerPutResponse r;
    r.epoch = intOr(obj, "epoch", 0);
    const auto ctrl = obj.value(QLatin1String("controller"));
    if (ctrl.isObject()) { r.controller = ControllerApplyDto::fromJson(ctrl.toObject()); }
    setIfNonEmpty(r.error, obj, "error");
    setIfNonEmpty(r.code, obj, "code");
    r.reachable = true;
    return r;
}

SessionViewControllerDto SessionViewControllerDto::fromJson(const QJsonObject& obj) {
    SessionViewControllerDto d;
    d.ctrlIdx = intOr(obj, "ctrlIdx", 0);
    d.active = boolOr(obj, "active", false);
    d.appliedType = intOr(obj, "appliedType", proto::kControllerTypeXbox);
    d.touchpadMode = optString(obj, "touchpadMode");
    const auto caps = obj.value(QLatin1String("caps"));
    if (caps.isObject()) {
        d.caps = capsWordFromJson(caps.toObject());
        d.capsPresent = true;
    }
    const auto motion = obj.value(QLatin1String("motion"));
    if (motion.isObject()) {
        const auto mo = motion.toObject();
        const auto sink = mo.value(QLatin1String("sinkSupportedForType"));
        if (sink.isBool()) { d.motionSinkSupportedForType = sink.toBool(); }
        const auto ok = mo.value(QLatin1String("backendOk"));
        if (ok.isBool()) { d.motionBackendOk = ok.toBool(); }
    }
    return d;
}

SessionViewDto SessionViewDto::fromJson(const QJsonObject& obj) {
    SessionViewDto r;
    setIfNonEmpty(r.connectionId, obj, "connectionId");
    r.epoch = intOr(obj, "epoch", 0);
    r.protocolVersion = intOr(obj, "protocolVersion", proto::kProtocolVersionMin);
    r.maxControllers = intOr(obj, "maxControllers", 16);
    for (const auto& v : obj.value(QLatin1String("controllers")).toArray()) {
        if (v.isObject()) {
            r.controllers.append(SessionViewControllerDto::fromJson(v.toObject()));
        }
    }
    const auto hf = obj.value(QLatin1String("hostFeatures")).toObject();
    r.mouseControl = HostFeatureGrant::fromJson(hf.value(QLatin1String("mouseControl")).toObject());
    setIfNonEmpty(r.error, obj, "error");
    setIfNonEmpty(r.code, obj, "code");
    r.reachable = true;
    return r;
}

CapabilitiesDto CapabilitiesDto::fromJson(const QJsonObject& obj) {
    CapabilitiesDto c;
    c.protocolVersion = intOr(obj, "protocolVersion", proto::kProtocolVersionMin);
    c.serverVersion = optString(obj, "serverVersion");
    c.maxControllers = intOr(obj, "maxControllers", 16);
    const auto backend = obj.value(QLatin1String("backend")).toObject();
    c.backendId = optString(backend, "id");
    c.backendSupported = boolOr(backend, "supported", false);
    c.backendAvailable = boolOr(backend, "available", false);
    if (auto ec = optString(backend, "errorCode"); !ec.isEmpty()) { c.backendErrorCode = ec; }
    const auto motion = obj.value(QLatin1String("motion")).toObject();
    c.motionAvailable = boolOr(motion, "available", false);
    const auto host = obj.value(QLatin1String("host"));
    if (host.isObject()) {
        const auto ho = host.toObject();
        c.hasHostBlock = true;
        c.hostCatalog = hostCapabilityFromJson(ho.value(QLatin1String("catalog")).toObject());
        c.hostMouseControl =
            hostCapabilityFromJson(ho.value(QLatin1String("mouseControl")).toObject());
        c.hostKeyboardControl =
            hostCapabilityFromJson(ho.value(QLatin1String("keyboardControl")).toObject());
        c.hostRumble = hostCapabilityFromJson(ho.value(QLatin1String("rumble")).toObject());
    }
    c.reachable = true;
    return c;
}

CatalogTypeDto CatalogTypeDto::fromJson(const QJsonObject& obj) {
    CatalogTypeDto t;
    t.id = intOr(obj, "id", 0);
    t.slug = optString(obj, "slug");
    t.name = optString(obj, "name");
    t.shortName = optString(obj, "shortName");
    t.description = optString(obj, "description");
    const auto image = obj.value(QLatin1String("image")).toObject();
    t.imageHref = optString(image, "href");
    t.imageEtag = optString(image, "etag");
    const auto features = obj.value(QLatin1String("features")).toObject();
    for (auto it = features.begin(); it != features.end(); ++it) {
        const auto fo = it.value().toObject();
        CatalogFeatureDto f;
        f.supported = boolOr(fo, "supported", false);
        if (auto req = optString(fo, "requires"); !req.isEmpty()) { f.requires_ = req; }
        for (const auto& m : fo.value(QLatin1String("modes")).toArray()) {
            if (m.isString()) { f.modes.append(m.toString()); }
        }
        t.features.insert(it.key(), f);
    }
    const auto emulates = obj.value(QLatin1String("emulates"));
    if (emulates.isObject()) {
        const auto eo = emulates.toObject();
        CatalogEmulatesDto e;
        e.sdlType = optString(eo, "sdlType");
        for (const auto& u : eo.value(QLatin1String("usb")).toArray()) {
            if (u.isString()) { e.usb.append(u.toString().toLower()); }
        }
        t.emulates = e;
    }
    return t;
}

CatalogDto CatalogDto::fromJson(const QJsonObject& obj) {
    CatalogDto c;
    c.locale = optString(obj, "locale");
    c.protocolVersion = intOr(obj, "protocolVersion", proto::kProtocolVersionMin);
    // Absent = the legacy v1 catalog per contract; never default to current.
    c.catalogVersion = intOr(obj, "catalogVersion", 1);
    c.serverVersion = optString(obj, "serverVersion");
    for (const auto& v : obj.value(QLatin1String("controllerTypes")).toArray()) {
        if (v.isObject()) { c.controllerTypes.append(CatalogTypeDto::fromJson(v.toObject())); }
    }
    const auto hf = obj.value(QLatin1String("hostFeatures")).toObject();
    for (auto it = hf.begin(); it != hf.end(); ++it) {
        const auto fo = it.value().toObject();
        CatalogHostFeatureDto f;
        f.supported = boolOr(fo, "supported", false);
        for (const auto& m : fo.value(QLatin1String("modes")).toArray()) {
            if (m.isString()) { f.modes.append(m.toString()); }
        }
        c.hostFeatures.insert(it.key(), f);
    }
    c.reachable = true;
    return c;
}

QJsonObject ControllerDescriptor::toJson() const {
    return QJsonObject{
        {"ctrlIdx", ctrlIdx},
        {"type", static_cast<int>(type)},
        {"caps",
         QJsonObject{
             {"rumble", (caps & proto::kCapRumble) != 0},
             {"motion", (caps & proto::kCapMotion) != 0},
             {"analogTriggers", (caps & proto::kCapAnalogTriggers) != 0},
             {"lightbar", (caps & proto::kCapLightbar) != 0},
             {"triggerEffects", (caps & proto::kCapTriggerEffects) != 0},
             {"playerLeds", (caps & proto::kCapPlayerLeds) != 0},
         }},
        {"touchpadMode",
         QString::fromUtf8(proto::touchpadModeName(touchpadMode).data(),
                           static_cast<int>(proto::touchpadModeName(touchpadMode).size()))},
    };
}

QJsonArray controllersJson(const QList<ControllerDescriptor>& descriptors) {
    QJsonArray arr;
    for (const auto& d : descriptors) { arr.append(d.toJson()); }
    return arr;
}

DiscoveredServer RememberedWifi::toDiscovered() const {
    DiscoveredServer s;
    s.name = name;
    s.ip = ip;
    s.udpPort = udpPort;
    s.pairPort = pairPort;
    s.httpPort = httpPort;
    s.machineId = machineId;
    return s;
}

QJsonObject RememberedWifi::toJson() const {
    return QJsonObject{
        {"id", id},
        {"name", name},
        {"ip", ip},
        {"udpPort", udpPort},
        {"pairPort", pairPort},
        {"httpPort", httpPort},
        {"machineId", machineId},
    };
}

RememberedWifi RememberedWifi::fromJson(const QJsonObject& obj) {
    RememberedWifi r;
    r.id = optString(obj, "id");
    r.name = optString(obj, "name");
    r.ip = optString(obj, "ip");
    r.udpPort = intOr(obj, "udpPort", kDefaultUdpPort);
    r.pairPort = intOr(obj, "pairPort", kDefaultPairPort);
    r.httpPort = intOr(obj, "httpPort", kDefaultHttpPort);
    r.machineId = optString(obj, "machineId");
    return r;
}

QJsonArray rememberedListToJson(const QList<RememberedWifi>& list) {
    QJsonArray arr;
    for (const auto& r : list) { arr.append(r.toJson()); }
    return arr;
}

QList<RememberedWifi> rememberedListFromJson(const QJsonArray& arr) {
    QList<RememberedWifi> out;
    out.reserve(arr.size());
    for (const auto& v : arr) {
        if (v.isObject()) { out.append(RememberedWifi::fromJson(v.toObject())); }
    }
    return out;
}

} // namespace dish::models
