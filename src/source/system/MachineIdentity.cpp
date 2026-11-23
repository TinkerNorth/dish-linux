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

// os-release's PRETTY_NAME, its quotes taken off; empty where the file or the key is missing.
QString prettyNameIn(const QString& osReleasePath) {
    QFile file(osReleasePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { return {}; }
    const QByteArray key = QByteArrayLiteral("PRETTY_NAME=");
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (!line.startsWith(key)) { continue; }
        QByteArray value = line.mid(key.size());
        const bool quoted = value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
                            value.back() == value.front();
        if (quoted) { value = value.mid(1, value.size() - 2); }
        return QString::fromUtf8(value);
    }
    return {};
}

// The host's distribution where a sandbox publishes its os-release, else Qt's answer, which is
// the os-release this process sees.
QString distributionName(const QString& hostOsRelease) {
    const QString host = prettyNameIn(hostOsRelease);
    return host.isEmpty() ? QSysInfo::prettyProductName() : host;
}

} // namespace

MachineIdentity readMachineIdentity(const QString& dmiDir, const QString& hostOsRelease) {
    const QString vendor = dmiAttribute(dmiDir, "sys_vendor");
    const QString product = dmiAttribute(dmiDir, "product_name");
    const QString kernel = QSysInfo::kernelVersion();

    MachineIdentity out;
    out.model = QString::fromStdString(
        diagnostics::machineModelOf(vendor.toStdString(), product.toStdString()));
    out.os = QStringLiteral("%1 (%2)").arg(distributionName(hostOsRelease), kernel);
    out.arch = QSysInfo::currentCpuArchitecture();
    out.kernelMajor = diagnostics::kernelMajorOf(kernel.toStdString());
    return out;
}

} // namespace dish::source
