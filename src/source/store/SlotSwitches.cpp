// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/SlotSwitches.h"

#include <QString>

namespace dish::source {

namespace {

// One store's half of the carry. Motion, mic and speaker are keyed by std::string
// and rumble by QString, so the key type is the store's own.
template <class Store, class Key>
void carrySwitch(Store& store, const Key& fromSlotId, const Key& toSlotId) {
    const auto switches = store.state().value();
    const auto it = switches.find(fromSlotId);
    if (it == switches.end()) {
        store.forget(toSlotId);
        return;
    }
    store.setEnabled(toSlotId, it->second);
}

} // namespace

void carrySlotSwitches(const SlotSwitchStores& stores, const std::string& fromSlotId,
                       const std::string& toSlotId) {
    carrySwitch(stores.motion, fromSlotId, toSlotId);
    carrySwitch(stores.rumble, QString::fromStdString(fromSlotId),
                QString::fromStdString(toSlotId));
    carrySwitch(stores.mic, fromSlotId, toSlotId);
    carrySwitch(stores.speaker, fromSlotId, toSlotId);
}

} // namespace dish::source
