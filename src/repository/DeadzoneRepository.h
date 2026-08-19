// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// DeadzoneRepository — durable per-device (stickFlat, triggerFlat) overrides
// under the "deadzone:<deviceId>" namespace of the shared connection-store
// QSettings (HKCU\Software\Dish\Dish).
//
// The SDL bridge reads this once at device-add and pushes the value into
// GamepadInputProcessor::setDeadzones; never per event, per the hot-path rule.
// A corrupt value for one device degrades to nullopt for that device only — a
// bad blob must not break controller setup. Namespace isolation is a hard
// invariant: clear() must touch only this repo's prefix, since the pin,
// shared-key, and list repos co-tenant the same store.

#pragma once

#include "core/input/Deadzones.h"

#include "architecture/Repository.h"

#include <QSettings>
#include <QString>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace dish::repository {

// `deviceId` is the SDL device id SDLGamepadBridge keys on; the flats use the
// core/input Deadzones scale (int16 stick, 0..255 trigger).
struct DeadzoneEntry {
    QString deviceId;
    std::int16_t stickFlat = 0;
    std::uint8_t triggerFlat = 0;

    input::deadzone::Deadzones deadzones() const { return {stickFlat, triggerFlat}; }

    bool operator==(const DeadzoneEntry& o) const {
        return deviceId == o.deviceId && stickFlat == o.stickFlat && triggerFlat == o.triggerFlat;
    }
    bool operator!=(const DeadzoneEntry& o) const { return !(*this == o); }
};

class DeadzoneRepository : public arch::KeyedRepository<QString, DeadzoneEntry> {
  public:
    // Production passes the shared connection-store QSettings so the
    // deadzone/pin/key/list repos co-tenant one backing file; tests inject their
    // own. nullptr -> the default HKCU store.
    explicit DeadzoneRepository(std::shared_ptr<QSettings> settings = nullptr);

    QString keyOf(const DeadzoneEntry& value) const override { return value.deviceId; }

    std::optional<input::deadzone::Deadzones> deadzonesFor(const QString& deviceId) const;
    void setDeadzones(const QString& deviceId, const input::deadzone::Deadzones& dz);

    std::optional<DeadzoneEntry> get(const QString& deviceId) const override;
    std::vector<DeadzoneEntry> all() const override;
    void put(const QString& deviceId, const DeadzoneEntry& value) override;
    void remove(const QString& deviceId) override;
    void clear() override;

    // Un-hide the KeyedRepository value-overloads the declarations above shadow.
    using arch::KeyedRepository<QString, DeadzoneEntry>::put;
    using arch::KeyedRepository<QString, DeadzoneEntry>::removeValue;

  private:
    std::shared_ptr<QSettings> settings_;
    mutable std::mutex mutex_;
};

} // namespace dish::repository
