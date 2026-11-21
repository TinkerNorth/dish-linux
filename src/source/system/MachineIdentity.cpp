// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/MachineIdentity.h"

#include "core/diagnostics/HostIdentity.h"

#include <QFile>
#include <QSysInfo>

namespace dish::source {

namespace {

// A DMI attribute is one line of text; a missing or unreadable one is empty.
QString dmiAttribute(const QString& dmiDir, const char* name) {
    QFile file(dmiDir + QLatin1Char('/') + QLatin1String(name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { return {}; }
    return QString::fromUtf8(file.readLine()).trimmed();
}

} // namespace

MachineIdentity readMachineIdentity(const QString& dmiDir) {
    const QString vendor = dmiAttribute(dmiDir, "sys_vendor");
    const QString product = dmiAttribute(dmiDir, "product_name");
    const QString kernel = QSysInfo::kernelVersion();

    MachineIdentity out;
    out.model = QString::fromStdString(
        diagnostics::machineModelOf(vendor.toStdString(), product.toStdString()));
    out.os = QStringLiteral("%1 (%2)").arg(QSysInfo::prettyProductName(), kernel);
    out.arch = QSysInfo::currentCpuArchitecture();
    out.kernelMajor = diagnostics::kernelMajorOf(kernel.toStdString());
    return out;
}

} // namespace dish::source
