// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/HostSnapshotCollector.h"

#include <algorithm>
#include <utility>

namespace dish::composer {

struct HostSnapshotCollector::Batch {
    HostSnapshotCollector* owner = nullptr;
    std::size_t waiting = 0;
    HostSnapshots outcomes;
    Done done;
};

HostSnapshotOutcome snapshotOutcomeOf(SnapshotFetchReply reply) {
    const diagnostics::SnapshotReply seen{reply.status, reply.timedOut, reply.snapshot.has_value()};
    HostSnapshotOutcome outcome;
    outcome.error = diagnostics::snapshotErrorOf(seen);
    if (outcome.error == diagnostics::SnapshotError::None) {
        outcome.snapshot = std::move(reply.snapshot);
    }
    return outcome;
}

HostSnapshotCollector::HostSnapshotCollector(HostSnapshotGateway& gateway) : gateway_(gateway) {}

HostSnapshotCollector::~HostSnapshotCollector() = default;

void HostSnapshotCollector::collect(const std::vector<std::string>& connectionKeys, Done done) {
    if (connectionKeys.empty()) {
        done(HostSnapshots{});
        return;
    }
    auto batch = std::make_shared<Batch>();
    batch->owner = this;
    batch->waiting = connectionKeys.size();
    batch->done = std::move(done);
    batches_.push_back(batch);
    const std::weak_ptr<Batch> weakBatch = batch;
    // Each closure holds a plain copy of its key: a capture of the loop's const reference would
    // make the closure's move a string copy, which can throw.
    for (const std::string& key : connectionKeys) {
        gateway_.fetch(key, [weakBatch, connectionKey = key](SnapshotFetchReply reply) {
            onReply(weakBatch, connectionKey, std::move(reply));
        });
    }
}

// The batch leaves the collector before its answer runs, so an answer that starts another
// collection does not find itself still waiting.
void HostSnapshotCollector::onReply(const std::weak_ptr<Batch>& weakBatch, const std::string& key,
                                    SnapshotFetchReply reply) {
    const std::shared_ptr<Batch> batch = weakBatch.lock();
    if (batch == nullptr) { return; }
    batch->outcomes[key] = snapshotOutcomeOf(std::move(reply));
    --batch->waiting;
    if (batch->waiting > 0) { return; }
    auto& batches = batch->owner->batches_;
    batches.erase(std::remove(batches.begin(), batches.end(), batch), batches.end());
    batch->done(std::move(batch->outcomes));
}

} // namespace dish::composer
