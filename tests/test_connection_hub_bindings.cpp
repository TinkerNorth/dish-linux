// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The SatelliteClient receive threads read the bindings to find the slot a
// feedback message belongs to, while the UI thread binds and unbinds. Unguarded,
// the reader can copy a table the writer is freeing.

#include "Network/ConnectionHub.h"
#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QHash>
#include <QSettings>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

using dish::net::ConnectionHub;
using dish::test::makeSharedSettings;

namespace {

constexpr int kRebinds = 500;
const QString kSlot = QStringLiteral("sdl:3");
const QString kSatellite = QStringLiteral("mid:m1");

// The hub as AppModel builds it, over an in-memory store, so no socket opens.
struct HubUnderTest {
    HubUnderTest() {
        const auto shared = makeSharedSettings();
        store = std::make_unique<dish::net::ConnectionStore>(
            std::unique_ptr<QSettings>(new QSettings(shared->fileName(), QSettings::IniFormat)));
        wifi = std::make_unique<dish::net::WifiConnectionManager>(store.get());
        hub = std::make_unique<ConnectionHub>(wifi.get(), store.get());
    }

    std::unique_ptr<dish::net::ConnectionStore> store;
    std::unique_ptr<dish::net::WifiConnectionManager> wifi;
    std::unique_ptr<ConnectionHub> hub;
};

bool isAPublishedTable(const QHash<QString, QString>& bindings) {
    const bool unbound = bindings.isEmpty();
    const bool bound = bindings.size() == 1 && bindings.value(kSlot) == kSatellite;
    return unbound || bound;
}

// A receive thread's side: read until the UI thread is done, counting any table
// that is neither of the two the UI thread ever published.
void readUntilDone(const ConnectionHub& hub, const std::atomic<bool>& done,
                   std::atomic<int>& unpublished) {
    while (!done.load()) {
        if (!isAPublishedTable(hub.bindings())) { ++unpublished; }
    }
}

} // namespace

TEST_CASE("a receive thread reading the bindings while the UI thread rebinds sees whole tables",
          "[hub][threads]") {
    HubUnderTest h;
    std::atomic<bool> done{false};
    std::atomic<int> unpublished{0};
    std::thread receiver(readUntilDone, std::cref(*h.hub), std::cref(done), std::ref(unpublished));

    for (int i = 0; i < kRebinds; ++i) {
        h.hub->bind(kSlot, kSatellite);
        h.hub->unbind(kSlot);
    }
    done.store(true);
    receiver.join();

    CHECK(unpublished.load() == 0);
}
