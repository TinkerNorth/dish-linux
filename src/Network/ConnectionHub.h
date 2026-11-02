// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "ConnectionStore.h"
#include "Models/Models.h"
#include "WifiConnectionManager.h"
#include "core/thread/TrackedMutex.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <mutex>
#include <utility>

namespace dish::net {

// Aggregates the wifi pool into the flat ConnectionSummary list the UI consumes,
// and owns the slot to connection binding table.
class ConnectionHub : public QObject {
    Q_OBJECT
  public:
    ConnectionHub(WifiConnectionManager* wifi, ConnectionStore* store, QObject* parent = nullptr);

    QList<models::ConnectionSummary> connections() const { return summaries_; }
    // A copy taken under the lock the UI thread writes under: the SatelliteClient
    // receive threads call this while the UI thread binds and unbinds.
    QHash<QString, QString> bindings() const;

    // The seams below let bind() stamp per-device hardware facts onto the
    // REST descriptor. AppModel installs them off the SDL bridge's device
    // classification; each is unset in tests and before the bridge exists, and
    // the fallbacks are chosen so an unset resolver understates capability.

    // Unset means no lightbar, so CAP_LIGHTBAR is not advertised.
    using LightbarCapabilityFn = std::function<bool(const QString& slotId)>;
    void setLightbarCapabilityFn(LightbarCapabilityFn fn) { lightbarCapabilityFn_ = std::move(fn); }

    // Unset means no motion, so CAP_MOTION is not advertised.
    using MotionCapabilityFn = std::function<bool(const QString& slotId)>;
    void setMotionCapabilityFn(MotionCapabilityFn fn) { motionCapabilityFn_ = std::move(fn); }

    // Unset means no rumble, so CAP_RUMBLE is not advertised.
    using RumbleCapabilityFn = std::function<bool(const QString& slotId)>;
    void setRumbleCapabilityFn(RumbleCapabilityFn fn) { rumbleCapabilityFn_ = std::move(fn); }

    // Protocol-2 actuators. Unset reads as "no", which is what an older build's
    // wiring should mean: the satellite then never sends the message.
    using TriggerEffectsCapabilityFn = std::function<bool(const QString& slotId)>;
    void setTriggerEffectsCapabilityFn(TriggerEffectsCapabilityFn fn) {
        triggerEffectsCapabilityFn_ = std::move(fn);
    }

    using PlayerLedsCapabilityFn = std::function<bool(const QString& slotId)>;
    void setPlayerLedsCapabilityFn(PlayerLedsCapabilityFn fn) {
        playerLedsCapabilityFn_ = std::move(fn);
    }

    // Controller audio. Unset reads as "no", same as the other actuators, so
    // the satellite neither expects MIC_AUDIO nor sends SPEAKER_AUDIO for a
    // slot this build cannot route. AppModel folds the audio route with the
    // per-binding user toggle.
    using MicCapabilityFn = std::function<bool(const QString& slotId)>;
    void setMicCapabilityFn(MicCapabilityFn fn) { micCapabilityFn_ = std::move(fn); }

    using SpeakerCapabilityFn = std::function<bool(const QString& slotId)>;
    void setSpeakerCapabilityFn(SpeakerCapabilityFn fn) { speakerCapabilityFn_ = std::move(fn); }

    // Protocol 3: whether the slot plays the haptic waveform itself. Unset
    // reads as "no", which makes the host reduce the lanes to rumble instead.
    using HapticAudioCapabilityFn = std::function<bool(const QString& slotId)>;
    void setHapticAudioCapabilityFn(HapticAudioCapabilityFn fn) {
        hapticAudioCapabilityFn_ = std::move(fn);
    }

    // A proto CONTROLLER_TYPE_*, which is how a DualSense registers as a virtual
    // DS4 rather than an Xbox pad. Unset means CONTROLLER_TYPE_XBOX.
    using ControllerTypeFn = std::function<int(const QString& slotId)>;
    void setControllerTypeFn(ControllerTypeFn fn) { controllerTypeFn_ = std::move(fn); }

    // A proto::kTouchpadMode* value, folded by AppModel from the pad's touch
    // source, the type's catalog gate, and the per-satellite pick. The satellite
    // discards every MSG_TOUCHPAD unless the descriptor declares a mode.
    using TouchpadModeFn = std::function<std::uint8_t(const QString& slotId)>;
    void setTouchpadModeFn(TouchpadModeFn fn) { touchpadModeFn_ = std::move(fn); }

    void bind(const QString& slotId, const QString& connectionId);
    void unbind(const QString& slotId);
    std::optional<models::ConnectionSummary> boundConnection(const QString& slotId) const;
    std::optional<models::ConnectionSummary> summary(const QString& id) const;

  signals:
    void changed();

  private:
    void rebuild();
    void publishBindings(QHash<QString, QString> next);

    // rebuild's three steps, each answerable on its own.
    QSet<QString> knownIds(const QHash<QString, models::RememberedWifi>& remembered) const;
    std::optional<QString> boundSlotFor(const QString& id) const;
    std::optional<models::ConnectionSummary>
    summaryFor(const QString& id, const QHash<QString, models::RememberedWifi>& remembered,
               const QSet<QString>& discoveredIds) const;

    WifiConnectionManager* wifi_;
    ConnectionStore* store_;
    QList<models::ConnectionSummary> summaries_;
    // slotId -> connectionId. Written only on the UI thread, and only through
    // publishBindings; any other thread reads it only through bindings().
    QHash<QString, QString> bindings_;
    mutable thread::TrackedMutex bindingsMtx_{"hub.bindings"};
    LightbarCapabilityFn lightbarCapabilityFn_;
    MotionCapabilityFn motionCapabilityFn_;
    RumbleCapabilityFn rumbleCapabilityFn_;
    TriggerEffectsCapabilityFn triggerEffectsCapabilityFn_;
    PlayerLedsCapabilityFn playerLedsCapabilityFn_;
    MicCapabilityFn micCapabilityFn_;
    SpeakerCapabilityFn speakerCapabilityFn_;
    HapticAudioCapabilityFn hapticAudioCapabilityFn_;
    ControllerTypeFn controllerTypeFn_;
    TouchpadModeFn touchpadModeFn_;
};

} // namespace dish::net
