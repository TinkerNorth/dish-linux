// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

#include <atomic>
#include <memory>

namespace dish {

// How the controller light bar should behave while a slot is bound.
//
// Mirrors the choice DS4Windows / DualSenseX expose ("lightbar from game" vs
// "off") and dish-mac's LightbarMode. Dish has no custom-colour picker — a
// forwarder reflects what the host game sets — so the meaningful axis is just
// follow-vs-off.
enum class LightbarMode {
    // Apply the colour the host game writes (forwarded via MSG_LIGHTBAR).
    FollowGame,
    // Ignore the host colour; leave the controller's light bar untouched.
    Off,
};

// Pure mappings between LightbarMode and its persisted / displayed strings.
// Free functions (no QObject) so unit tests can pin them without a settings
// store. `lightbarModeFromKey` is lenient: an unknown / empty key falls back
// to the documented default (FollowGame).
QString lightbarModeToKey(LightbarMode mode);
LightbarMode lightbarModeFromKey(const QString& key);
QString lightbarModeLabel(LightbarMode mode);

// User-facing on/off preferences for the forwarded controller features that
// Dish *acts on* from the host game. Today this is just the light bar; the
// class is the C++ analogue of dish-mac's FeatureSettings and is the natural
// home for future rumble / motion toggles.
//
// Persisted to QSettings so the choice survives relaunch — the same store
// ConnectionStore uses. The QSettings instance is injectable so tests can
// supply an in-memory / temp-file store.
//
// Thread-safety: setLightbarMode() / lightbarMode() are main-thread only (they
// touch QSettings + emit a Qt signal). lightbarFollowGame() reads a plain
// std::atomic and is safe to call from any thread — it exists so the
// SatelliteClient receive thread can gate the light-bar apply without
// touching the Qt object.
class FeatureSettings : public QObject {
    Q_OBJECT
  public:
    explicit FeatureSettings(QObject* parent = nullptr);
    // Test seam: inject the backing QSettings.
    explicit FeatureSettings(std::unique_ptr<QSettings> settings, QObject* parent = nullptr);
    ~FeatureSettings() override;

    LightbarMode lightbarMode() const { return lightbarMode_; }
    void setLightbarMode(LightbarMode mode);

    // Thread-safe gate for the hot path: true when the light bar should follow
    // the host game (LightbarMode::FollowGame), false when it is Off. Read on
    // the SatelliteClient receive thread.
    bool lightbarFollowGame() const { return lightbarFollowGame_.load(std::memory_order_relaxed); }

  signals:
    // Emitted after any feature preference changes so the UI re-renders and
    // AppModel can refresh thread-safe snapshots.
    void changed();

  private:
    void load();

    std::unique_ptr<QSettings> settings_;
    LightbarMode lightbarMode_ = LightbarMode::FollowGame;
    // Mirror of (lightbarMode_ == FollowGame), kept in sync by setLightbarMode
    // / load so the receive thread never has to read lightbarMode_ directly.
    std::atomic<bool> lightbarFollowGame_{true};
};

} // namespace dish
