// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What a fake gateway's scan hands up: each device with the model an attach pins when no database
// is installed, as HidrawGateway's scan does through the resolver. `autoClaims`, where a test
// gives it, stands for the verdict on going Direct by itself that the test is about.

#pragma once

#include "core/controllerdb/PadModel.h"
#include "source/usb/UsbDeviceGateway.h"

#include <optional>
#include <vector>

namespace dish::test {

inline std::vector<source::usb::UsbDeviceInfo>
pinnedWithoutDatabase(std::vector<source::usb::UsbDeviceInfo> devices,
                      std::optional<bool> autoClaims = std::nullopt) {
    for (source::usb::UsbDeviceInfo& d : devices) {
        d.model = controllerdb::modelWithoutDatabase(d.vendorId, d.productId);
        if (autoClaims.has_value()) { d.model.autoClaims = *autoClaims; }
    }
    return devices;
}

} // namespace dish::test
