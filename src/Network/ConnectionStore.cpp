// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionStore.h"

namespace dish::net {

ConnectionStore::ConnectionStore(std::unique_ptr<QSettings> settings) {
    // An injected QSettings becomes the shared backing store for all three
    // repositories; otherwise the facade opens the default HKCU store.
    std::shared_ptr<QSettings> shared =
        settings ? std::shared_ptr<QSettings>(std::move(settings)) : nullptr;
    facade_ = std::make_unique<repository::ConnectionStore>(std::move(shared));
}

} // namespace dish::net
