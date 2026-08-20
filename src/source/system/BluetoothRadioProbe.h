// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// "Is there a Bluetooth radio, and is it switched on?" — two genuinely
// different facts that need different copy on the wizard's waiting step:
//
//   present && !enabled  -> "Bluetooth is off." + Open settings
//   !present             -> "This machine has no Bluetooth adapter."
//
// so they are probed separately. Presence reads sysfs (the hardware node exists
// even with the radio soft-blocked, and it answers with bluetoothd stopped);
// power reads BlueZ's Adapter1.Powered over the system bus.

#pragma once

namespace dish::source {

struct BluetoothRadioState {
    bool present = false;
    bool enabled = false;
};

BluetoothRadioState probeBluetoothRadio();

} // namespace dish::source
