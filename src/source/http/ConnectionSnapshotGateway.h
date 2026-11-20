// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The support report's thin edge to each Satellite's connection-diagnostics
// route (docs/diagnostics-bundle.md, "The hosts, from both ends"). The request
// itself is the network layer's, behind the Fetch seam, with its pin gate and
// proof headers; this reads what came back. Only a JSON object of the
// connection-diagnostics format becomes a snapshot, converted into the bundle's
// own JSON so the Qt-free builder can embed and scrub it.

#pragma once

#include "composer/HostSnapshotCollector.h"
#include "core/diagnostics/Json.h"

#include <QByteArray>
#include <QJsonValue>
#include <QString>

#include <functional>
#include <optional>
#include <string>

namespace dish::source {

// Every JSON value, numbers kept integral where they are whole.
diagnostics::Json jsonFromQt(const QJsonValue& value);

// The body as a snapshot: empty unless it is an object whose `format` is the route's own.
std::optional<diagnostics::Json> connectionSnapshotFromBody(const QByteArray& body);

class ConnectionSnapshotGateway : public composer::HostSnapshotGateway {
  public:
    // The answer as the network layer heard it: the status (0 for none), whether the deadline
    // passed first, and the body.
    using RawReply = std::function<void(int status, bool timedOut, const QByteArray& body)>;
    // Sends the GET for one link, or answers at once with status 0 when it could not.
    using Fetch =
        std::function<void(const QString& connectionKey, int timeoutMs, const RawReply& reply)>;

    explicit ConnectionSnapshotGateway(Fetch fetch);

    void fetch(const std::string& connectionKey, Reply reply) override;

  private:
    Fetch fetch_;
};

} // namespace dish::source
