// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionStore.h"

namespace dish::net {

ConnectionStore::ConnectionStore(std::unique_ptr<QSettings> settings) {
    // An injected QSettings becomes the shared backing store for all three
    // repositories; otherwise the facade opens the app's own config file.
    std::shared_ptr<QSettings> shared =
        settings ? std::shared_ptr<QSettings>(std::move(settings)) : nullptr;

    // The keyring is wired only on the production path — the no-settings one.
    // A test that injects its own QSettings gets the config-file behaviour and
    // never touches the developer's login keyring, which is the whole reason the
    // secret store is injected rather than constructed inside the repository.
    std::shared_ptr<source::SecretServiceStore> secrets;
    if (!shared) { secrets = std::make_shared<source::SecretServiceStore>(); }

    facade_ = std::make_unique<repository::ConnectionStore>(std::move(shared), std::move(secrets));
}

} // namespace dish::net
