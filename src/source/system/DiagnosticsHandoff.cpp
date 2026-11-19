// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/DiagnosticsHandoff.h"

#include "core/diagnostics/MailHandoff.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <string>
#include <utility>

namespace dish::source {

namespace {

// xdg-email returns once the mail client has the message, except where it runs the client in the
// foreground (Thunderbird when none is running), which then holds a compose window: past this,
// the mail is the client's.
constexpr int kXdgEmailGraceMs = 10'000;

QString qtText(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QStringList xdgEmailArgumentsOf(const DiagnosticsMail& mail) {
    QStringList out;
    for (const std::string& argument : diagnostics::xdgEmailArguments(
             mail.attachmentPath.toStdString(), mail.subject.toStdString(), mail.body.toStdString(),
             mail.to.toStdString())) {
        out.append(QString::fromStdString(argument));
    }
    return out;
}

} // namespace

QString diagnosticsFolder() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) { return {}; }
    return base + QStringLiteral("/diagnostics");
}

QString saveDiagnosticsFile(const QString& folder, const QString& fileName,
                            const QByteArray& bundle) {
    if (folder.isEmpty() || !QDir().mkpath(folder)) { return {}; }
    // Not const: it is returned, and a const local cannot be moved out.
    QString path = folder + QLatin1Char('/') + fileName;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { return {}; }
    const bool wrote = file.write(bundle) == bundle.size();
    if (!wrote || !file.commit()) { return {}; }
    return path;
}

void revealFolderOf(const QString& filePath, const UrlOpener& open) {
    open(QUrl::fromLocalFile(QFileInfo(filePath).absolutePath()));
}

namespace {

void openWithDesktopServices(const QUrl& url) { QDesktopServices::openUrl(url); }

} // namespace

DiagnosticsHandoff::DiagnosticsHandoff(QObject* parent)
    : DiagnosticsHandoff(qtText(diagnostics::kXdgEmail), &openWithDesktopServices, parent) {}

DiagnosticsHandoff::DiagnosticsHandoff(QString xdgEmail, UrlOpener open, QObject* parent)
    : QObject(parent), xdgEmail_(std::move(xdgEmail)), open_(std::move(open)),
      grace_(new QTimer(this)) {
    grace_->setSingleShot(true);
    grace_->setInterval(kXdgEmailGraceMs);
    QObject::connect(grace_, &QTimer::timeout, this, &DiagnosticsHandoff::onGraceExpired);
}

// A process still running here is a mail client xdg-email ran in the foreground. QProcess ends
// the xdg-email shell, never the client it started, which outlives the app as its own process.
DiagnosticsHandoff::~DiagnosticsHandoff() = default;

void DiagnosticsHandoff::reveal(const QString& filePath) const { revealFolderOf(filePath, open_); }

bool DiagnosticsHandoff::send(const DiagnosticsMail& mail) {
    if (busy_) { return false; }
    const QString program = QStandardPaths::findExecutable(xdgEmail_);
    switch (diagnostics::firstMailStep(!program.isEmpty())) {
    case diagnostics::MailStep::RunXdgEmail:
        break;
    case diagnostics::MailStep::Done:
    case diagnostics::MailStep::OpenMailtoAndRevealFolder:
        openMailtoAndReveal(mail);
        emit handedOff(false);
        return true;
    }
    busy_ = true;
    pending_ = mail;
    process_ = new QProcess(this);
    QObject::connect(process_, &QProcess::finished, this, &DiagnosticsHandoff::onFinished);
    QObject::connect(process_, &QProcess::finished, process_, &QObject::deleteLater);
    QObject::connect(process_, &QProcess::errorOccurred, this,
                     &DiagnosticsHandoff::onErrorOccurred);
    process_->start(program, xdgEmailArgumentsOf(mail));
    grace_->start();
    return true;
}

void DiagnosticsHandoff::onFinished(int exitCode, QProcess::ExitStatus status) {
    if (!busy_ || sender() != process_) { return; }
    // A crash has no exit code of its own; it is a failure like any code but 0.
    constexpr int kCrashedCode = -1;
    const bool exited = status == QProcess::NormalExit;
    const int code = exited ? exitCode : kCrashedCode;
    const diagnostics::MailStep step =
        diagnostics::nextStepAfterXdgEmail(diagnostics::XdgEmailEnd::Exited, code);
    conclude(step == diagnostics::MailStep::Done);
}

// Only a process that never started ends here without a finished() to follow.
void DiagnosticsHandoff::onErrorOccurred(QProcess::ProcessError error) {
    if (!busy_ || sender() != process_ || error != QProcess::FailedToStart) { return; }
    const diagnostics::MailStep step =
        diagnostics::nextStepAfterXdgEmail(diagnostics::XdgEmailEnd::FailedToStart, 0);
    conclude(step == diagnostics::MailStep::Done);
}

void DiagnosticsHandoff::onGraceExpired() {
    if (!busy_) { return; }
    const diagnostics::MailStep step =
        diagnostics::nextStepAfterXdgEmail(diagnostics::XdgEmailEnd::StillRunning, 0);
    conclude(step == diagnostics::MailStep::Done);
}

void DiagnosticsHandoff::conclude(bool viaMailClient) {
    busy_ = false;
    grace_->stop();
    if (!viaMailClient) { openMailtoAndReveal(pending_); }
    emit handedOff(viaMailClient);
}

void DiagnosticsHandoff::openMailtoAndReveal(const DiagnosticsMail& mail) {
    const std::string url = diagnostics::mailtoUrl(
        mail.to.toStdString(), mail.subject.toStdString(), mail.body.toStdString());
    open_(QUrl::fromEncoded(QByteArray::fromStdString(url), QUrl::StrictMode));
    revealFolderOf(mail.attachmentPath, open_);
}

} // namespace dish::source
