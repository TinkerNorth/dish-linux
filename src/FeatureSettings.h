// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

#include <atomic>
#include <memory>

namespace dish {

// There is no custom-colour picker: a forwarder reflects what the host game sets,
// so the only meaningful axis is follow versus off.
enum class LightbarMode {
    FollowGame, // apply the colour the host game writes, via MSG_LIGHTBAR
    Off,        // leave the controller's light bar untouched
};

// `lightbarModeFromKey` is lenient: an unknown or empty key falls back to
// FollowGame.
QString lightbarModeToKey(LightbarMode mode);
LightbarMode lightbarModeFromKey(const QString& key);
QString lightbarModeLabel(LightbarMode mode);

// User-facing on/off preferences for the forwarded features Dish acts on from the
// host game, persisted to QSettings.
//
// setLightbarMode() and lightbarMode() are main-thread only: they touch QSettings
// and emit a Qt signal. lightbarFollowGame() reads an atomic and is callable from
// any thread, which is how the SatelliteClient receive thread gates the apply
// without touching the QObject.
class FeatureSettings : public QObject {
    Q_OBJECT
  public:
    explicit FeatureSettings(QObject* parent = nullptr);
    // Test seam: inject an in-memory or temp-file store.
    explicit FeatureSettings(std::unique_ptr<QSettings> settings, QObject* parent = nullptr);
    ~FeatureSettings() override;

    LightbarMode lightbarMode() const { return lightbarMode_; }
    void setLightbarMode(LightbarMode mode);

    bool lightbarFollowGame() const { return lightbarFollowGame_.load(std::memory_order_relaxed); }

  signals:
    void changed();

  private:
    void load();

    std::unique_ptr<QSettings> settings_;
    LightbarMode lightbarMode_ = LightbarMode::FollowGame;
    // Mirrors (lightbarMode_ == FollowGame) so the receive thread never reads
    // lightbarMode_ directly.
    std::atomic<bool> lightbarFollowGame_{true};
};

} // namespace dish
