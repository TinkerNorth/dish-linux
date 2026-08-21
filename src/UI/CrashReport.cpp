// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "UI/CrashReport.h"

#include "UI/CrashHandler.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUrlQuery>

#include <cstdlib>

namespace dish::crash {

namespace {

constexpr const char* kIssueUrl = "https://github.com/TinkerNorth/dish-linux/issues/new";

// Marks a body the URL could not carry, so the user knows the issue is short and
// the file is the real artifact rather than assuming the paste was complete.
constexpr const char* kTruncationNotice =
    "\n\n[report truncated — please attach the full crash.log from the path above]";

} // namespace

QString crashLogPath() {
    char buffer[kPathMax];
    logPathFor(buffer, kPathMax, std::getenv("XDG_STATE_HOME"), std::getenv("HOME"));
    return QString::fromLocal8Bit(buffer);
}

bool hasCrashLog() {
    const QString path = crashLogPath();
    if (path.isEmpty()) { return false; }
    const QFileInfo info(path);
    return info.exists() && info.size() > 0;
}

QString readCrashLog() {
    const QString path = crashLogPath();
    if (path.isEmpty()) { return {}; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { return {}; }
    return QString::fromUtf8(file.readAll());
}

bool discardCrashLog() {
    const QString path = crashLogPath();
    if (path.isEmpty()) { return false; }
    // "Already gone" is the requested end state, not a failure.
    if (!QFileInfo::exists(path)) { return true; }
    return QFile::remove(path);
}

QString redact(const QString& raw, const QString& home) {
    QString out = raw;

    // Home first: it is the longest and most specific pattern, and collapsing it
    // early keeps the later passes from matching inside a path segment.
    if (!home.isEmpty() && home != QLatin1String("/")) { out.replace(home, QStringLiteral("~")); }

    // Key material before addresses: a 32-byte key in hex is 64 characters and
    // would otherwise survive as a long unmatched run.
    static const QRegularExpression hexRun(QStringLiteral("\\b[0-9a-fA-F]{32,}\\b"));
    out.replace(hexRun, QStringLiteral("[redacted]"));

    // Anchored on word boundaries so it cannot eat part of a version string.
    static const QRegularExpression ipv4(QStringLiteral("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b"));
    out.replace(ipv4, QStringLiteral("[ip]"));

    // mDNS names carry the owner's real name far more often than not
    // ("Lauras-MacBook-Pro.local").
    static const QRegularExpression mdnsHost(
        QStringLiteral("\\b[A-Za-z0-9][A-Za-z0-9-]*\\.local\\b"));
    out.replace(mdnsHost, QStringLiteral("[host].local"));

    return out;
}

QString buildReport(const QString& rawLog, const QString& home) {
    // Deliberately small: version, kernel and Qt are what a backtrace needs to be
    // actionable, and everything beyond that is one more thing the user has to
    // read and approve before sending.
    QString out;
    out += QStringLiteral("Dish %1\n").arg(QLatin1String(DISH_VERSION));
    out += QStringLiteral("OS: %1 %2 (%3)\n")
               .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
                    QSysInfo::currentCpuArchitecture());
    out += QStringLiteral("Qt: %1\n").arg(QLatin1String(qVersion()));
    out += QStringLiteral("\n--- backtrace ---\n");
    out += redact(rawLog, home);
    return out;
}

QUrl issueUrl(const QString& report) {
    QString body = report;
    // Budget the body against the ENCODED length: percent-encoding a backtrace
    // roughly triples it, so measuring the raw string would overshoot every time.
    QUrl url{QLatin1String(kIssueUrl)};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("title"), QStringLiteral("Crash report"));
    query.addQueryItem(QStringLiteral("body"), body);
    url.setQuery(query);

    while (url.toEncoded().size() > kMaxIssueUrlBytes && body.size() > 64) {
        // Halve and retry rather than trimming byte by byte: the encoded size is
        // not a linear function of the character count, so a single subtraction
        // cannot be trusted to converge.
        body.truncate(body.size() / 2);
        QString trimmed = body;
        trimmed += QLatin1String(kTruncationNotice);
        QUrlQuery retry;
        retry.addQueryItem(QStringLiteral("title"), QStringLiteral("Crash report"));
        retry.addQueryItem(QStringLiteral("body"), trimmed);
        url.setQuery(retry);
    }
    return url;
}

} // namespace dish::crash
