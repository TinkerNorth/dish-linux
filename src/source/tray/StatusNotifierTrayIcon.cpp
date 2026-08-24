// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/tray/StatusNotifierTrayIcon.h"

#include "source/tray/SniIcon.h"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QStringLiteral>

namespace dish::source {

namespace {

Q_LOGGING_CATEGORY(lcDishTray, "dish.tray")

constexpr auto kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr auto kWatcherPath = "/StatusNotifierWatcher";
constexpr auto kItemInterface = "org.kde.StatusNotifierItem";
constexpr auto kItemPath = "/StatusNotifierItem";
constexpr auto kMenuPath = "/MenuBar";
constexpr auto kNewIconSignal = "NewIcon";
constexpr auto kNewToolTipSignal = "NewToolTip";

constexpr int kRegisterTimeoutMs = 2000;
constexpr int kRootItemId = 0;
constexpr int kShowItemId = 1;
constexpr int kQuitItemId = 2;
constexpr uint kMenuVersion = 3;

void registerDbusTypes() {
    static const bool sRegistered = [] {
        qDBusRegisterMetaType<SniIconPixmap>();
        qDBusRegisterMetaType<SniIconPixmapList>();
        qDBusRegisterMetaType<SniToolTip>();
        qDBusRegisterMetaType<DbusMenuLayoutItem>();
        qDBusRegisterMetaType<DbusMenuItemProperties>();
        qDBusRegisterMetaType<DbusMenuItemPropertiesList>();
        return true;
    }();
    static_cast<void>(sRegistered);
}

int nextConnectionIndex() {
    static int sIndex = 0;
    ++sIndex;
    return sIndex;
}

bool watcherOnBus() {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) { return false; }
    QDBusConnectionInterface* iface = bus.interface();
    if (iface == nullptr) { return false; }
    return iface->isServiceRegistered(QLatin1String(kWatcherService)).value();
}

QVariantMap filterProperties(const QVariantMap& properties, const QStringList& names) {
    if (names.isEmpty()) { return properties; }
    QVariantMap filtered;
    for (const QString& name : names) {
        const auto found = properties.constFind(name);
        if (found != properties.constEnd()) { filtered.insert(name, found.value()); }
    }
    return filtered;
}

} // namespace

// Returning the const reference parameter is the signature QtDBus REQUIRES of a
// demarshalling operator — qDBusRegisterMetaType accepts no other shape. The
// lifetime concern the check raises cannot arise here: every caller is QtDBus
// itself, streaming from an argument it owns for the duration of the call, and
// none binds the result to anything longer-lived.
// NOLINTBEGIN(bugprone-return-const-ref-from-parameter)
QDBusArgument& operator<<(QDBusArgument& argument, const SniIconPixmap& pixmap) {
    argument.beginStructure();
    argument << pixmap.width << pixmap.height << pixmap.data;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, SniIconPixmap& pixmap) {
    argument.beginStructure();
    argument >> pixmap.width >> pixmap.height >> pixmap.data;
    argument.endStructure();
    return argument;
}

QDBusArgument& operator<<(QDBusArgument& argument, const SniToolTip& tip) {
    argument.beginStructure();
    argument << tip.iconName << tip.iconPixmap << tip.title << tip.description;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, SniToolTip& tip) {
    argument.beginStructure();
    argument >> tip.iconName >> tip.iconPixmap >> tip.title >> tip.description;
    argument.endStructure();
    return argument;
}

// The children of (ia{sv}av) are variants each wrapping a whole (ia{sv}av), not
// a nested array of structs; a host handed a(ia{sv}av) opens an empty menu.
QDBusArgument& operator<<(QDBusArgument& argument, const DbusMenuLayoutItem& item) {
    argument.beginStructure();
    argument << item.id << item.properties;
    argument.beginArray(QMetaType::fromType<QDBusVariant>());
    for (const DbusMenuLayoutItem& child : item.children) {
        argument << QDBusVariant(QVariant::fromValue(child));
    }
    argument.endArray();
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, DbusMenuLayoutItem& item) {
    argument.beginStructure();
    argument >> item.id >> item.properties;
    argument.beginArray();
    item.children.clear();
    while (!argument.atEnd()) {
        QDBusVariant child;
        argument >> child;
        item.children.append(qdbus_cast<DbusMenuLayoutItem>(child.variant()));
    }
    argument.endArray();
    argument.endStructure();
    return argument;
}

QDBusArgument& operator<<(QDBusArgument& argument, const DbusMenuItemProperties& entry) {
    argument.beginStructure();
    argument << entry.id << entry.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, DbusMenuItemProperties& entry) {
    argument.beginStructure();
    argument >> entry.id >> entry.properties;
    argument.endStructure();
    return argument;
}
// NOLINTEND(bugprone-return-const-ref-from-parameter)

class StatusNotifierItemAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")
    Q_PROPERTY(QString Category READ category)
    Q_PROPERTY(QString Id READ id)
    Q_PROPERTY(QString Title READ title)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(int WindowId READ windowId)
    Q_PROPERTY(QString IconName READ iconName)
    Q_PROPERTY(dish::source::SniIconPixmapList IconPixmap READ iconPixmap)
    Q_PROPERTY(QString OverlayIconName READ overlayIconName)
    Q_PROPERTY(dish::source::SniIconPixmapList OverlayIconPixmap READ overlayIconPixmap)
    Q_PROPERTY(QString AttentionIconName READ attentionIconName)
    Q_PROPERTY(dish::source::SniIconPixmapList AttentionIconPixmap READ attentionIconPixmap)
    Q_PROPERTY(QString AttentionMovieName READ attentionMovieName)
    Q_PROPERTY(dish::source::SniToolTip ToolTip READ toolTip)
    Q_PROPERTY(QString IconThemePath READ iconThemePath)
    Q_PROPERTY(bool ItemIsMenu READ itemIsMenu)
    Q_PROPERTY(QDBusObjectPath Menu READ menu)
  public:
    StatusNotifierItemAdaptor(QObject* host, StatusNotifierTrayIcon* owner)
        : QDBusAbstractAdaptor(host), owner_(owner) {}

    QString category() const { return QStringLiteral("ApplicationStatus"); }
    QString id() const { return QLatin1String(kSniIconName); }
    QString title() const { return QStringLiteral("Dish"); }
    // Pinned Active: a Passive item may be hidden by the host, and a hidden
    // item is a running Dish the user can neither reach nor quit.
    QString status() const { return QStringLiteral("Active"); }
    int windowId() const { return 0; }
    QString iconName() const { return owner_->iconName(); }
    SniIconPixmapList iconPixmap() const { return owner_->iconPixmap(); }
    QString overlayIconName() const { return {}; }
    SniIconPixmapList overlayIconPixmap() const { return {}; }
    QString attentionIconName() const { return {}; }
    SniIconPixmapList attentionIconPixmap() const { return {}; }
    QString attentionMovieName() const { return {}; }
    SniToolTip toolTip() const { return owner_->toolTip(); }
    QString iconThemePath() const { return {}; }
    // False so a left click still activates on hosts that honour it; GNOME
    // ignores the property and routes every click to the menu regardless.
    bool itemIsMenu() const { return false; }
    QDBusObjectPath menu() const { return QDBusObjectPath(owner_->menuObjectPath()); }

  public slots:
    void Activate(int, int) { owner_->requestShowWindow(); }
    void SecondaryActivate(int, int) { owner_->requestShowWindow(); }
    void ContextMenu(int, int) {}
    void Scroll(int, const QString&) {}

  signals:
    void NewTitle();
    void NewIcon();
    void NewAttentionIcon();
    void NewOverlayIcon();
    void NewToolTip();
    void NewStatus(const QString&);

  private:
    StatusNotifierTrayIcon* owner_;
};

class DbusMenuAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.dbusmenu")
    Q_PROPERTY(uint Version READ version)
    Q_PROPERTY(QString TextDirection READ textDirection)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(QStringList IconThemePath READ iconThemePath)
  public:
    DbusMenuAdaptor(QObject* host, StatusNotifierTrayIcon* owner)
        : QDBusAbstractAdaptor(host), owner_(owner) {}

    uint version() const { return kMenuVersion; }
    QString textDirection() const {
        return QGuiApplication::isRightToLeft() ? QStringLiteral("rtl") : QStringLiteral("ltr");
    }
    QString status() const { return QStringLiteral("normal"); }
    QStringList iconThemePath() const { return {}; }

  public slots:
    uint GetLayout(int parentId, int recursionDepth, const QStringList& propertyNames,
                   dish::source::DbusMenuLayoutItem& layout) {
        layout = owner_->menuLayout(parentId, recursionDepth, propertyNames);
        return owner_->menuRevision();
    }
    dish::source::DbusMenuItemPropertiesList GetGroupProperties(const QList<int>& ids,
                                                                const QStringList& propertyNames) {
        return owner_->menuGroupProperties(ids, propertyNames);
    }
    QDBusVariant GetProperty(int id, const QString& name) {
        return QDBusVariant(owner_->menuItemProperties(id).value(name));
    }
    // Called synchronously from the shell's own thread, so it queues and
    // returns rather than doing the work here.
    void Event(int id, const QString& eventId, const QDBusVariant&, uint) {
        owner_->handleMenuEvent(id, eventId);
    }
    bool AboutToShow(int) { return false; }

  signals:
    void ItemActivationRequested(int, uint);
    void LayoutUpdated(uint, int);

  private:
    StatusNotifierTrayIcon* owner_;
};

StatusNotifierTrayIcon::StatusNotifierTrayIcon(QObject* parent) : TrayIcon(parent) {
    registerDbusTypes();
}

StatusNotifierTrayIcon::~StatusNotifierTrayIcon() { teardown(); }

bool StatusNotifierTrayIcon::isAvailable() const { return available_; }

QString StatusNotifierTrayIcon::iconName() const { return sniIconName(themeHasIcon_); }

SniIconPixmapList StatusNotifierTrayIcon::iconPixmap() const { return iconPixmap_; }

QString StatusNotifierTrayIcon::menuObjectPath() const { return QLatin1String(kMenuPath); }

uint StatusNotifierTrayIcon::menuRevision() const { return 1; }

SniToolTip StatusNotifierTrayIcon::toolTip() const {
    SniToolTip tip;
    tip.iconName = sniIconName(themeHasIcon_);
    tip.iconPixmap = iconPixmap_;
    tip.title = QStringLiteral("Dish");
    switch (presentation_.activity) {
    case reducer::TrayActivity::Idle:
        tip.description = tr("Idle");
        return tip;
    case reducer::TrayActivity::Streaming:
        tip.description = tr("Streaming to %n controller(s)", "", presentation_.streamingSlots);
        return tip;
    }
    return tip;
}

QVariantMap StatusNotifierTrayIcon::menuItemProperties(int id) const {
    QVariantMap properties;
    if (id == kRootItemId) {
        properties.insert(QStringLiteral("children-display"), QStringLiteral("submenu"));
        return properties;
    }
    if (id != kShowItemId && id != kQuitItemId) { return properties; }
    properties.insert(QStringLiteral("label"), id == kShowItemId ? tr("Show Dish") : tr("Quit"));
    properties.insert(QStringLiteral("enabled"), true);
    properties.insert(QStringLiteral("visible"), true);
    return properties;
}

DbusMenuLayoutItem StatusNotifierTrayIcon::menuLayout(int parentId, int recursionDepth,
                                                      const QStringList& propertyNames) const {
    DbusMenuLayoutItem item;
    item.id = parentId;
    item.properties = filterProperties(menuItemProperties(parentId), propertyNames);
    if (parentId != kRootItemId || recursionDepth == 0) { return item; }
    for (const int childId : {kShowItemId, kQuitItemId}) {
        DbusMenuLayoutItem child;
        child.id = childId;
        child.properties = filterProperties(menuItemProperties(childId), propertyNames);
        item.children.append(child);
    }
    return item;
}

DbusMenuItemPropertiesList
StatusNotifierTrayIcon::menuGroupProperties(const QList<int>& ids,
                                            const QStringList& propertyNames) const {
    const QList<int> wanted =
        ids.isEmpty() ? QList<int>{kRootItemId, kShowItemId, kQuitItemId} : ids;
    DbusMenuItemPropertiesList entries;
    entries.reserve(wanted.size());
    for (const int id : wanted) {
        DbusMenuItemProperties entry;
        entry.id = id;
        entry.properties = filterProperties(menuItemProperties(id), propertyNames);
        entries.append(entry);
    }
    return entries;
}

void StatusNotifierTrayIcon::handleMenuEvent(int id, const QString& eventId) {
    if (eventId != QLatin1String("clicked")) { return; }
    if (id == kShowItemId) {
        requestShowWindow();
        return;
    }
    if (id == kQuitItemId) { requestQuit(); }
}

void StatusNotifierTrayIcon::requestShowWindow() {
    QMetaObject::invokeMethod(this, [this] { emit showWindowRequested(); }, Qt::QueuedConnection);
}

void StatusNotifierTrayIcon::requestQuit() {
    QMetaObject::invokeMethod(this, [this] { emit quitRequested(); }, Qt::QueuedConnection);
}

void StatusNotifierTrayIcon::show() {
    if (shown_) { return; }
    if (!QDBusConnection::sessionBus().isConnected()) {
        qCDebug(lcDishTray) << "no session bus; tray item disabled";
        return;
    }
    shown_ = true;
    if (watcher_ == nullptr) {
        watcher_ =
            new QDBusServiceWatcher(QLatin1String(kWatcherService), QDBusConnection::sessionBus(),
                                    QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher_, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString&, const QString&, const QString& newOwner) {
                    handleWatcherOwner(newOwner);
                });
    }
    watcherPresent_ = watcherOnBus();
    ensureExported();
    registerItem();
    refreshAvailability();
}

void StatusNotifierTrayIcon::hide() {
    if (!shown_) { return; }
    shown_ = false;
    registered_ = false;
    teardown();
    refreshAvailability();
}

void StatusNotifierTrayIcon::ensureExported() {
    if (exported_) { return; }
    const QString key = QStringLiteral("org.kde.StatusNotifierItem-%1-%2")
                            .arg(QCoreApplication::applicationPid())
                            .arg(nextConnectionIndex());
    connection_ = QDBusConnection::connectToBus(QDBusConnection::SessionBus, key);
    if (!connection_.isConnected()) {
        qCWarning(lcDishTray) << "tray bus connection failed:" << connection_.lastError().message();
        connection_ = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus(key);
        return;
    }
    if (itemHost_ == nullptr) {
        itemHost_ = new QObject(this);
        menuHost_ = new QObject(this);
        new StatusNotifierItemAdaptor(itemHost_, this);
        new DbusMenuAdaptor(menuHost_, this);
    }
    updateIcon();
    connection_.registerObject(QLatin1String(kItemPath), itemHost_,
                               QDBusConnection::ExportAdaptors);
    connection_.registerObject(QLatin1String(kMenuPath), menuHost_,
                               QDBusConnection::ExportAdaptors);
    // Best effort only: the name helps hosts that still scan for it, and the
    // Flatpak portal proxy denies it without harming the unique-name path.
    connection_.registerService(key);
    exported_ = true;
}

void StatusNotifierTrayIcon::teardown() {
    if (!exported_) { return; }
    exported_ = false;
    connection_.unregisterObject(QLatin1String(kMenuPath));
    connection_.unregisterObject(QLatin1String(kItemPath));
    const QString key = connection_.name();
    // Drop this handle before disconnecting, or QtDBus warns that the
    // connection is still referenced and leaves it open.
    connection_ = QDBusConnection(QString());
    QDBusConnection::disconnectFromBus(key);
}

// Async with a short cap: this runs on the startup path and the default 25s
// blocking timeout would stall it behind an unresponsive shell.
void StatusNotifierTrayIcon::registerItem() {
    if (!exported_ || !connection_.isConnected()) { return; }
    QDBusMessage message = QDBusMessage::createMethodCall(
        QLatin1String(kWatcherService), QLatin1String(kWatcherPath), QLatin1String(kWatcherService),
        QStringLiteral("RegisterStatusNotifierItem"));
    message << connection_.baseService();
    auto* pending =
        new QDBusPendingCallWatcher(connection_.asyncCall(message, kRegisterTimeoutMs), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* call) {
                const QDBusPendingReply<> reply = *call;
                registered_ = !reply.isError();
                if (!registered_) {
                    qCWarning(lcDishTray)
                        << "RegisterStatusNotifierItem failed:" << reply.error().message();
                }
                refreshAvailability();
                call->deleteLater();
            });
}

void StatusNotifierTrayIcon::handleWatcherOwner(const QString& newOwner) {
    watcherPresent_ = !newOwner.isEmpty();
    registered_ = false;
    refreshAvailability();
    if (!watcherPresent_ || !shown_) { return; }
    // A restarted shell has forgotten every item, so the export is rebuilt and
    // re-announced rather than assumed to have survived.
    teardown();
    ensureExported();
    registerItem();
}

void StatusNotifierTrayIcon::refreshAvailability() {
    const bool nowAvailable = watcherPresent_ && registered_;
    if (nowAvailable == available_) { return; }
    available_ = nowAvailable;
    emit availabilityChanged(available_);
}

// Theme lookup misses entirely under Flatpak, Snap and AppImage, so IconPixmap
// always ships alongside IconName instead of only when the theme fails.
void StatusNotifierTrayIcon::updateIcon() {
    themeHasIcon_ = QIcon::hasThemeIcon(QLatin1String(kSniIconName));
    SniIconPixmapList pixmaps = sniTrayPixmaps();
    if (pixmaps == iconPixmap_) { return; }
    iconPixmap_ = pixmaps;
    if (exported_) { emitItemSignal(kNewIconSignal); }
}

void StatusNotifierTrayIcon::setPresentation(const reducer::TrayPresentation& presentation) {
    if (presentation_ == presentation) { return; }
    presentation_ = presentation;
    // The icon is activity-independent, so only the tooltip has actually
    // changed and NewIcon would be pure churn for the host.
    if (exported_) { emitItemSignal(kNewToolTipSignal); }
}

void StatusNotifierTrayIcon::emitItemSignal(const char* name) {
    if (!connection_.isConnected()) { return; }
    const QDBusMessage signal = QDBusMessage::createSignal(
        QLatin1String(kItemPath), QLatin1String(kItemInterface), QLatin1String(name));
    connection_.send(signal);
}

std::unique_ptr<TrayIcon> makeSystemTrayIcon() {
    return std::make_unique<StatusNotifierTrayIcon>();
}

} // namespace dish::source

#include "StatusNotifierTrayIcon.moc"
