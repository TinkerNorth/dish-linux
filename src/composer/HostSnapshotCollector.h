// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Gathers what each live Satellite link says about its session while the
// support report is being prepared (docs/diagnostics-bundle.md, "The hosts,
// from both ends"): one GET of the connection's diagnostics route per link,
// all at once, and one answer when the last of them is in. Pure: the request
// is the gateway's, and an answer that lands after the collector is gone (the
// page closed) reaches nobody.

#pragma once

#include "core/diagnostics/HostSnapshot.h"
#include "core/diagnostics/Json.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dish::composer {

// The route's answer as the gateway read it: `snapshot` only when the body was a JSON object of
// the connection-diagnostics format, already converted.
struct SnapshotFetchReply {
    int status = 0;
    bool timedOut = false;
    std::optional<diagnostics::Json> snapshot;
};

struct HostSnapshotOutcome {
    diagnostics::SnapshotError error = diagnostics::SnapshotError::Offline;
    // The host's own document, present only when `error` is None.
    std::optional<diagnostics::Json> snapshot;
};

// By the link's key (the connection's id in the app, never anything the bundle writes).
using HostSnapshots = std::map<std::string, HostSnapshotOutcome>;

class HostSnapshotGateway {
  public:
    using Reply = std::function<void(SnapshotFetchReply)>;
    virtual ~HostSnapshotGateway() = default;
    // Answers exactly once, from the event loop; within kSnapshotTimeoutMs or as a timeout.
    virtual void fetch(const std::string& connectionKey, Reply reply) = 0;

  protected:
    HostSnapshotGateway() = default;
    HostSnapshotGateway(const HostSnapshotGateway&) = default;
    HostSnapshotGateway& operator=(const HostSnapshotGateway&) = default;
};

HostSnapshotOutcome snapshotOutcomeOf(SnapshotFetchReply reply);

class HostSnapshotCollector {
  public:
    using Done = std::function<void(HostSnapshots)>;

    explicit HostSnapshotCollector(HostSnapshotGateway& gateway);
    ~HostSnapshotCollector();

    HostSnapshotCollector(const HostSnapshotCollector&) = delete;
    HostSnapshotCollector& operator=(const HostSnapshotCollector&) = delete;
    HostSnapshotCollector(HostSnapshotCollector&&) = delete;
    HostSnapshotCollector& operator=(HostSnapshotCollector&&) = delete;

    // Asks every link in `connectionKeys`; `done` runs once with an outcome for each, and at once
    // when there are none. Two collections run independently.
    void collect(const std::vector<std::string>& connectionKeys, Done done);

  private:
    struct Batch;
    static void onReply(const std::weak_ptr<Batch>& weakBatch, const std::string& key,
                        SnapshotFetchReply reply);

    HostSnapshotGateway& gateway_;
    // Every batch in flight; dropping them is what silences a late reply.
    std::vector<std::shared_ptr<Batch>> batches_;
};

} // namespace dish::composer
