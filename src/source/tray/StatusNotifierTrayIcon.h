// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The StatusNotifierItem tray item, spoken over QtDBus by hand: QSystemTrayIcon
// lives in QtWidgets and main() deliberately keeps that module out of the
// process.
//
// The item owns its own bus connection and registers under that connection's
// unique name rather than claiming a well-known one, because the Flatpak portal
// proxy refuses arbitrary RequestName calls.
//
// Availability follows org.kde.StatusNotifierWatcher being on the bus, not
// IsStatusNotifierHostRegistered, which Plasma, GNOME and XFCE all hardcode to
// true. Shell restarts are routine on GNOME, so the watcher name is watched for
// owner changes and the item re-registers on every reappearance.
//
// The menu is load-bearing, not decoration: GNOME's AppIndicator extension
// never delivers Activate on a single left click and ignores clicks entirely
// when the menu is empty, so "Show Dish" is the only dependable way back to a
// window that has been closed to the tray.

#pragma once

#include "core/reducer/TrayPresentation.h"
#include "source/tray/TrayIcon.h"

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

class QDBusServiceWatcher;

namespace dish::source {

struct SniIconPixmap {
    int width = 0;
    int height = 0;
    QByteArray data;

    bool operator==(const SniIconPixmap& o) const {
        return width == o.width && height == o.height && data == o.data;
    }
};
using SniIconPixmapList = QList<SniIconPixmap>;

struct SniToolTip {
    QString iconName;
    SniIconPixmapList iconPixmap;
    QString title;
    QString description;
};

struct DbusMenuLayoutItem {
    int id = 0;
    QVariantMap properties;
    QList<DbusMenuLayoutItem> children;
};

struct DbusMenuItemProperties {
    int id = 0;
    QVariantMap properties;
};
using DbusMenuItemPropertiesList = QList<DbusMenuItemProperties>;

QDBusArgument& operator<<(QDBusArgument& argument, const SniIconPixmap& pixmap);
const QDBusArgument& operator>>(const QDBusArgument& argument, SniIconPixmap& pixmap);
QDBusArgument& operator<<(QDBusArgument& argument, const SniToolTip& tip);
const QDBusArgument& operator>>(const QDBusArgument& argument, SniToolTip& tip);
QDBusArgument& operator<<(QDBusArgument& argument, const DbusMenuLayoutItem& item);
const QDBusArgument& operator>>(const QDBusArgument& argument, DbusMenuLayoutItem& item);
QDBusArgument& operator<<(QDBusArgument& argument, const DbusMenuItemProperties& entry);
const QDBusArgument& operator>>(const QDBusArgument& argument, DbusMenuItemProperties& entry);

class StatusNotifierTrayIcon final : public TrayIcon {
    Q_OBJECT
  public:
    explicit StatusNotifierTrayIcon(QObject* parent = nullptr);
    ~StatusNotifierTrayIcon() override;

    void show() override;
    void hide() override;
    bool isAvailable() const override;
    void setPresentation(const reducer::TrayPresentation& presentation) override;

    // Read back by the two adaptors, which are private to the .cpp.
    QString iconName() const;
    SniIconPixmapList iconPixmap() const;
    SniToolTip toolTip() const;
    QString menuObjectPath() const;
    uint menuRevision() const;
    QVariantMap menuItemProperties(int id) const;
    DbusMenuLayoutItem menuLayout(int parentId, int recursionDepth,
                                  const QStringList& propertyNames) const;
    DbusMenuItemPropertiesList menuGroupProperties(const QList<int>& ids,
                                                   const QStringList& propertyNames) const;
    void handleMenuEvent(int id, const QString& eventId);
    void requestShowWindow();
    void requestQuit();

  private:
    void ensureExported();
    void teardown();
    void registerItem();
    void refreshAvailability();
    void updateIcon();
    void handleWatcherOwner(const QString& newOwner);
    void emitItemSignal(const char* name);

    QDBusConnection connection_{QString()};
    QDBusServiceWatcher* watcher_ = nullptr;
    QObject* itemHost_ = nullptr;
    QObject* menuHost_ = nullptr;
    reducer::TrayPresentation presentation_;
    SniIconPixmapList iconPixmap_;
    bool shown_ = false;
    bool exported_ = false;
    bool registered_ = false;
    bool watcherPresent_ = false;
    bool available_ = false;
};

// The platform tray item. Never null: a desktop with no StatusNotifier host
// still gets an item that honestly reports isAvailable() == false.
std::unique_ptr<TrayIcon> makeSystemTrayIcon();

} // namespace dish::source

Q_DECLARE_METATYPE(dish::source::SniIconPixmap)
Q_DECLARE_METATYPE(dish::source::SniIconPixmapList)
Q_DECLARE_METATYPE(dish::source::SniToolTip)
Q_DECLARE_METATYPE(dish::source::DbusMenuLayoutItem)
Q_DECLARE_METATYPE(dish::source::DbusMenuItemProperties)
Q_DECLARE_METATYPE(dish::source::DbusMenuItemPropertiesList)
