// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "ConnectionStore.h"
#include "Models/Models.h"
#include "WifiConnectionManager.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <utility>

namespace dish::net {

// Aggregates the wifi pool into the flat ConnectionSummary list the UI consumes,
// and owns the slot to connection binding table.
class ConnectionHub : public QObject {
    Q_OBJECT
  public:
    using ReportSender = std::function<void(std::uint16_t, std::uint8_t, std::uint8_t, std::int16_t,
                                            std::int16_t, std::int16_t, std::int16_t)>;

    // gyroX/Y/Z, accelX/Y/Z, timestampDeltaUs.
    using MotionSender = std::function<void(std::int16_t, std::int16_t, std::int16_t, std::int16_t,
                                            std::int16_t, std::int16_t, std::uint32_t)>;

    // level (0..100 or 0xFF), status (BATTERY_STATUS_*).
    using BatterySender = std::function<void(std::uint8_t, std::uint8_t)>;

    // finger0(active,id,x,y), finger1(active,id,x,y), button, eventTimeMs.
    using TouchpadSender =
        std::function<void(bool, std::uint8_t, std::int16_t, std::int16_t, bool, std::uint8_t,
                           std::int16_t, std::int16_t, bool, std::uint32_t)>;

    ConnectionHub(WifiConnectionManager* wifi, ConnectionStore* store, QObject* parent = nullptr);

    QList<models::ConnectionSummary> connections() const { return summaries_; }
    QHash<QString, QString> bindings() const { return bindings_; }

    // Empty when nothing is bound. The returned closure does one mutex-guarded
    // shared_ptr load, so it is safe to call from the SDL input thread.
    ReportSender reportSenderForSlot(const QString& slotId) const;

    MotionSender motionSenderForSlot(const QString& slotId) const;
    BatterySender batterySenderForSlot(const QString& slotId) const;
    TouchpadSender touchpadSenderForSlot(const QString& slotId) const;

    // seq, opus bytes, length -> sent. For the mic capture engine, which calls
    // it from the audio thread (same closure contract as ReportSender from the
    // SDL input thread).
    using MicAudioSender = std::function<bool(std::uint16_t, const std::uint8_t*, std::size_t)>;
    MicAudioSender micAudioSenderForSlot(const QString& slotId) const;

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
    // slot this build cannot route. AppModel folds the audio route (a Wave-2
    // seam, false today) with the per-binding user toggle.
    using MicCapabilityFn = std::function<bool(const QString& slotId)>;
    void setMicCapabilityFn(MicCapabilityFn fn) { micCapabilityFn_ = std::move(fn); }

    using SpeakerCapabilityFn = std::function<bool(const QString& slotId)>;
    void setSpeakerCapabilityFn(SpeakerCapabilityFn fn) { speakerCapabilityFn_ = std::move(fn); }

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

    WifiConnectionManager* wifi_;
    ConnectionStore* store_;
    QList<models::ConnectionSummary> summaries_;
    QHash<QString, QString> bindings_; // slotId -> connectionId
    LightbarCapabilityFn lightbarCapabilityFn_;
    MotionCapabilityFn motionCapabilityFn_;
    RumbleCapabilityFn rumbleCapabilityFn_;
    TriggerEffectsCapabilityFn triggerEffectsCapabilityFn_;
    PlayerLedsCapabilityFn playerLedsCapabilityFn_;
    MicCapabilityFn micCapabilityFn_;
    SpeakerCapabilityFn speakerCapabilityFn_;
    ControllerTypeFn controllerTypeFn_;
    TouchpadModeFn touchpadModeFn_;
};

} // namespace dish::net
