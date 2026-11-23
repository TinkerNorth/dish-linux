// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Linux end of "Save diagnostics" and "Report a problem"
// (docs/diagnostics-bundle.md, "The mail"): the bundle is written under the
// app's data folder ($XDG_DATA_HOME/TinkerNorth/Dish/diagnostics), then handed
// to the person's own mail client with
// `xdg-email --attach <file> --subject <s> --body <b> support@tinkernorth.com`,
// which opens a compose window and sends nothing itself. When xdg-email is not
// on the PATH, fails to start or exits with anything but 0, a mailto: link
// opens with the subject and body. The folder is shown either way, because
// xdg-email attaches the file for Thunderbird only, so the file can be
// attached by hand. Which of those runs is core/diagnostics/MailHandoff.h's
// decision.
//
// Main thread. One mail at a time.

#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>

#include <functional>

class QTimer;
class QUrl;

namespace dish::source {

// AppLocalDataLocation + /diagnostics, "" when it resolves to nothing.
QString diagnosticsFolder();

// Writes the bundle into the folder; the file's full path, or "" when it could not be written.
QString saveDiagnosticsFile(const QString& folder, const QString& fileName,
                            const QByteArray& bundle);

// What opens a mailto: link or a folder: QDesktopServices::openUrl in the app, which is xdg-open
// outside a sandbox and the OpenURI portal inside one.
using UrlOpener = std::function<void(const QUrl&)>;

// The file's folder in the file manager. A folder, not the file: xdg-open cannot select a file
// the way a file manager's own call can.
void revealFolderOf(const QString& filePath, const UrlOpener& open);

struct DiagnosticsMail {
    QString to;
    QString subject;
    QString body;
    QString attachmentPath;
};

class DiagnosticsHandoff : public QObject {
    Q_OBJECT
  public:
    explicit DiagnosticsHandoff(QObject* parent = nullptr);
    // `xdgEmail` is the program to run and `open` what opens a URL; a test hands them a script, or
    // a name that is not there, and a recorder.
    DiagnosticsHandoff(QString xdgEmail, UrlOpener open, QObject* parent);
    ~DiagnosticsHandoff() override;

    DiagnosticsHandoff(const DiagnosticsHandoff&) = delete;
    DiagnosticsHandoff& operator=(const DiagnosticsHandoff&) = delete;
    DiagnosticsHandoff(DiagnosticsHandoff&&) = delete;
    DiagnosticsHandoff& operator=(DiagnosticsHandoff&&) = delete;

    // False while the last mail's xdg-email has not answered yet.
    bool send(const DiagnosticsMail& mail);
    // "Save diagnostics": the saved file's folder in the file manager.
    void reveal(const QString& filePath) const;

  signals:
    // `viaMailClient` is true when xdg-email took it, false when the mailto: fallback ran; the
    // file's folder is open in both cases, and `fileName` is the file in it.
    void handedOff(bool viaMailClient, const QString& fileName);

  private:
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);
    void onGraceExpired();
    void conclude(bool viaMailClient);
    void openMailtoAndReveal(const DiagnosticsMail& mail);

    const QString xdgEmail_;
    const UrlOpener open_;
    // The last mail's xdg-email; a late answer from an earlier one is not this mail's.
    QPointer<QProcess> process_;
    QTimer* grace_ = nullptr;
    DiagnosticsMail pending_;
    bool busy_ = false;
};

} // namespace dish::source
