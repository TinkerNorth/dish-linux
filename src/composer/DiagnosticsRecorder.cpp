// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/DiagnosticsRecorder.h"

#include <string>
#include <utility>

namespace dish::composer {

namespace {

reducer::PadPath padPathOf(const models::ControllerSlot& slot) {
    if (slot.usbDirect) { return reducer::PadPath::UsbDirect; }
    if (slot.bluetooth) { return reducer::PadPath::Bluetooth; }
    return reducer::PadPath::UsbStandard;
}

// A binding whose connection row has gone keeps its id, which is still better
// than a blank in a bug report.
std::string boundLabelOf(const models::ControllerSlot& slot) {
    if (slot.boundStatus.has_value()) { return slot.boundStatus->label.toStdString(); }
    return slot.boundConnectionId.value_or(QString()).toStdString();
}

} // namespace

reducer::LinkSnapshot linkSnapshotOf(const ConnectionRow& row) {
    return {row.id, row.label, row.live};
}

reducer::PadSnapshot padSnapshotOf(const models::ControllerSlot& slot) {
    reducer::PadSnapshot pad;
    pad.id = slot.id.toStdString();
    pad.name = slot.name.toStdString();
    pad.path = padPathOf(slot);
    pad.phase = slot.pathPhase;
    pad.failure = slot.directFailure;
    pad.boundId = slot.boundConnectionId.value_or(QString()).toStdString();
    pad.boundLabel = boundLabelOf(slot);
    return pad;
}

DiagnosticsRecorder::DiagnosticsRecorder(const arch::Observable<std::vector<ConnectionRow>>& links,
                                         source::DiagnosticsLogStore* log, Clock clock)
    : log_(log), clock_(std::move(clock)) {
    linksSub_ =
        links.subscribe([this](const std::vector<ConnectionRow>& rows) { observeLinks(rows); });
}

void DiagnosticsRecorder::observeLinks(const std::vector<ConnectionRow>& rows) {
    std::vector<reducer::LinkSnapshot> next;
    next.reserve(rows.size());
    for (const auto& row : rows) { next.push_back(linkSnapshotOf(row)); }
    log_->record(reducer::linkEvents(links_, next, clock_()));
    links_ = std::move(next);
}

void DiagnosticsRecorder::observeSlots(const QList<models::ControllerSlot>& slotList) {
    std::vector<reducer::PadSnapshot> next;
    next.reserve(static_cast<std::size_t>(slotList.size()));
    for (const auto& slot : slotList) { next.push_back(padSnapshotOf(slot)); }
    log_->record(reducer::padEvents(pads_, next, clock_()));
    pads_ = std::move(next);
}

} // namespace dish::composer
