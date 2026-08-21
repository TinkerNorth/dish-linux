// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A desktop notification, for the one thing the in-window toast host cannot
// carry: the window is gone by the time this has to be read.

#pragma once

#include <QObject>
#include <QString>

namespace dish::source {

class DesktopNotifier : public QObject {
    Q_OBJECT
  public:
    explicit DesktopNotifier(QObject* parent = nullptr) : QObject(parent) {}
    ~DesktopNotifier() override = default;

    virtual void notify(const QString& summary, const QString& body) = 0;
};

class FreedesktopNotifier : public DesktopNotifier {
    Q_OBJECT
  public:
    explicit FreedesktopNotifier(QObject* parent = nullptr) : DesktopNotifier(parent) {}

    void notify(const QString& summary, const QString& body) override;
};

} // namespace dish::source
