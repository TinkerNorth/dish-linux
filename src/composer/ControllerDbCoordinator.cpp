// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/ControllerDbCoordinator.h"

#include "source/controllerdb/ControllerDbJson.h"

#include <QCryptographicHash>
#include <QTimer>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace dish::composer {

namespace cdb = dish::controllerdb;
namespace cdbsrc = dish::source::controllerdb;

namespace {

QString qtText(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string sha256Hex(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

} // namespace

ControllerDbCoordinator::ControllerDbCoordinator(cdbsrc::ControllerDbStore* store,
                                                 source::ControllerDbPreferenceStore* prefs,
                                                 cdbsrc::ControllerDbPort* port, Clock wallClockMs,
                                                 QObject* parent)
    : QObject(parent), store_(store), prefs_(prefs), port_(port),
      wallClockMs_(std::move(wallClockMs)), firstTimer_(new QTimer(this)),
      intervalTimer_(new QTimer(this)) {
    firstTimer_->setSingleShot(true);
    firstTimer_->setInterval(static_cast<int>(cdb::kFirstCheckDelayMs));
    intervalTimer_->setInterval(static_cast<int>(cdb::kCheckIntervalMs));
    QObject::connect(firstTimer_, &QTimer::timeout, this, &ControllerDbCoordinator::scheduledCheck);
    QObject::connect(intervalTimer_, &QTimer::timeout, this,
                     &ControllerDbCoordinator::scheduledCheck);
    prefsSub_ = prefs_->state().subscribe(
        [this](const source::ControllerDbPreferences& p) { onPreferences(p); },
        /*emitCurrent=*/false);
}

ControllerDbCoordinator::~ControllerDbCoordinator() {
    prefsSub_ = arch::Observable<source::ControllerDbPreferences>::Subscription{};
    port_->cancel();
}

void ControllerDbCoordinator::start() {
    firstTimer_->start();
    intervalTimer_->start();
}

void ControllerDbCoordinator::checkNow() { run(cdb::CheckTrigger::CheckNow); }

void ControllerDbCoordinator::scheduledCheck() { run(cdb::CheckTrigger::Schedule); }

void ControllerDbCoordinator::onPreferences(const source::ControllerDbPreferences& prefs) {
    if (prefs.checksEnabled || !inFlight_) { return; }
    port_->cancel();
    finish(cdb::CheckResult::Off);
}

void ControllerDbCoordinator::run(cdb::CheckTrigger trigger) {
    if (inFlight_) { return; }
    if (!cdb::checkMayRun(prefs_->checksEnabled(), trigger)) {
        store_->recordCheck(wallClockMs_(), cdb::CheckResult::Off);
        return;
    }
    inFlight_ = true;
    store_->setChecking(true);
    const int activeVersion = store_->state().value().activeVersion;
    const std::optional<std::string> etag =
        cdb::ifNoneMatchFor(prefs_->etag().toStdString(), prefs_->etagVersion(), activeVersion);
    const QByteArray ifNoneMatch =
        etag.has_value() ? QByteArray::fromStdString(*etag) : QByteArray();
    port_->get(qtText(cdb::kManifestPath), ifNoneMatch,
               [this](const cdbsrc::DbReply& reply) { onManifest(reply); });
}

void ControllerDbCoordinator::onManifest(const cdbsrc::DbReply& reply) {
    const int activeVersion = store_->state().value().activeVersion;
    switch (cdb::replyVerdict(reply.transportFailed, reply.status)) {
    case cdb::ReplyVerdict::Offline:
        finish(cdb::CheckResult::Offline);
        return;
    case cdb::ReplyVerdict::NotModified:
        finish(cdb::CheckResult::UpToDate);
        return;
    case cdb::ReplyVerdict::Body:
        break;
    }
    auto parsed = cdbsrc::parseDbManifest(reply.body);
    auto* manifest = std::get_if<cdb::DbManifest>(&parsed);
    if (manifest == nullptr || !cdb::manifestIsUsable(*manifest)) {
        finish(cdb::CheckResult::Invalid);
        return;
    }
    if (cdb::manifestStep(*manifest, activeVersion) == cdb::ManifestStep::UpToDate) {
        prefs_->setEtag(reply.etag, activeVersion);
        finish(cdb::CheckResult::UpToDate);
        return;
    }
    toFetch_ = cdb::filesThisBuildFetches(*manifest);
    manifestEtag_ = reply.etag;
    download_ = cdbsrc::DbFiles{};
    download_.manifest = reply.body;
    nextFile_ = 0;
    fetchNextFile();
}

void ControllerDbCoordinator::fetchNextFile() {
    if (nextFile_ >= toFetch_.size()) {
        commitDownload();
        return;
    }
    port_->get(qtText(toFetch_[nextFile_].path), QByteArray(),
               [this](const cdbsrc::DbReply& reply) { onFile(reply); });
}

void ControllerDbCoordinator::onFile(const cdbsrc::DbReply& reply) {
    switch (cdb::replyVerdict(reply.transportFailed, reply.status)) {
    case cdb::ReplyVerdict::Offline:
        finish(cdb::CheckResult::Offline);
        return;
    // A file is asked for without an ETag, so a 304 for it is an answer to a request never sent.
    case cdb::ReplyVerdict::NotModified:
        finish(cdb::CheckResult::Invalid);
        return;
    case cdb::ReplyVerdict::Body:
        break;
    }
    const cdb::ManifestFile& promised = toFetch_[nextFile_];
    if (!cdb::fileMatches(promised, sha256Hex(reply.body), reply.body.size())) {
        finish(cdb::CheckResult::Invalid);
        return;
    }
    download_.files.insert(qtText(promised.path), reply.body);
    ++nextFile_;
    fetchNextFile();
}

// The same reader the bundled copy goes through, so a download is held to everything the shipped
// files are: every file as promised, every file decoding, schema 1 and the announced version.
void ControllerDbCoordinator::commitDownload() {
    const cdb::CatalogPtr catalog = cdbsrc::catalogFromFiles(download_, cdb::DbSource::Cache);
    if (catalog == nullptr) {
        finish(cdb::CheckResult::Invalid);
        return;
    }
    if (!store_->commit(download_, catalog)) {
        finish(cdb::CheckResult::Invalid);
        return;
    }
    prefs_->setEtag(manifestEtag_, catalog->dbVersion());
    finish(cdb::CheckResult::Updated);
}

void ControllerDbCoordinator::finish(cdb::CheckResult result) {
    inFlight_ = false;
    download_ = cdbsrc::DbFiles{};
    toFetch_.clear();
    store_->setChecking(false);
    store_->recordCheck(wallClockMs_(), result);
}

} // namespace dish::composer
