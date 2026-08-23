// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/system/SecretServiceStore.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QMap>
#include <QTimer>
#include <QVariant>

namespace dish::source {

namespace {

Q_LOGGING_CATEGORY(lcSecret, "dish.secret", QtInfoMsg)

constexpr const char* kService = "org.freedesktop.secrets";
constexpr const char* kServicePath = "/org/freedesktop/secrets";
constexpr const char* kServiceIface = "org.freedesktop.Secret.Service";
constexpr const char* kCollectionIface = "org.freedesktop.Secret.Collection";
constexpr const char* kItemIface = "org.freedesktop.Secret.Item";
constexpr const char* kPromptIface = "org.freedesktop.Secret.Prompt";
constexpr const char* kDefaultCollection = "/org/freedesktop/secrets/aliases/default";

// "plain" sends the secret over the session bus unencrypted. The bus socket is
// mode 0600 in the user's own runtime directory, so the alternative — the
// dh-ietf1024 handshake — would be encrypting a channel only this user can read
// against an attacker who, by then, is already this user.
constexpr const char* kPlainAlgorithm = "plain";

// A local D-Bus round trip is sub-millisecond. This bound exists only so a wedged
// or half-dead keyring cannot hang the connect path forever.
constexpr int kCallTimeoutMs = 3000;
// Longer: an unlock can legitimately be waiting for the user's password prompt.
constexpr int kPromptTimeoutMs = 120000;

// Distinguishes our items from every other application's in a shared keyring, and
// "xdg:schema" is the convention libsecret and every keyring UI expect.
constexpr const char* kSchema = "com.tinkernorth.Dish.PairingKey";

using StringMap = QMap<QString, QString>;

// org.freedesktop.Secret.Item.GetSecret returns (oayays).
struct SecretValue {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument& operator<<(QDBusArgument& arg, const SecretValue& s) {
    arg.beginStructure();
    arg << s.session << s.parameters << s.value << s.contentType;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, SecretValue& s) {
    arg.beginStructure();
    arg >> s.session >> s.parameters >> s.value >> s.contentType;
    arg.endStructure();
    // Returning the const reference parameter is the signature QtDBus REQUIRES
    // for a demarshalling operator — qDBusRegisterMetaType will not accept any
    // other shape. The lifetime concern the check flags cannot arise here: every
    // caller is QtDBus itself, streaming from an argument it owns for the whole
    // call, and no caller binds the result to a longer-lived reference.
    // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter)
    return arg;
}

void registerTypes() {
    static bool done = false;
    if (done) { return; }
    done = true;
    qDBusRegisterMetaType<SecretValue>();
    qDBusRegisterMetaType<StringMap>();
}

StringMap attributesFor(const QString& id) {
    StringMap attrs;
    attrs.insert(QStringLiteral("xdg:schema"), QLatin1String(kSchema));
    attrs.insert(QStringLiteral("application"), QStringLiteral("dish"));
    attrs.insert(QStringLiteral("satellite"), id);
    return attrs;
}

bool isNullPath(const QDBusObjectPath& path) {
    return path.path().isEmpty() || path.path() == QLatin1String("/");
}

// Blocks on a Prompt object until it completes, is dismissed, or the bound
// expires. Returns false on dismissal and on timeout alike: both mean "we did not
// get what we asked for", and the caller's response to each is the same.
bool awaitPrompt(const QDBusObjectPath& promptPath) {
    if (isNullPath(promptPath)) { return true; }

    QDBusInterface prompt(QLatin1String(kService), promptPath.path(), QLatin1String(kPromptIface),
                          QDBusConnection::sessionBus());
    if (!prompt.isValid()) { return false; }

    QEventLoop loop;
    bool dismissed = true;
    const bool connected = QDBusConnection::sessionBus().connect(
        QLatin1String(kService), promptPath.path(), QLatin1String(kPromptIface),
        QStringLiteral("Completed"), &loop, SLOT(quit()));
    if (!connected) { return false; }

    // The signal carries (bool dismissed, variant result); a second connection
    // reads the flag, since the SLOT(quit()) above cannot.
    QObject context;
    QDBusConnection::sessionBus().connect(QLatin1String(kService), promptPath.path(),
                                          QLatin1String(kPromptIface), QStringLiteral("Completed"),
                                          &context, SLOT(deleteLater()));

    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(kPromptTimeoutMs);

    const QDBusMessage reply = prompt.call(QStringLiteral("Prompt"), QString());
    if (reply.type() == QDBusMessage::ErrorMessage) { return false; }

    loop.exec();
    dismissed = !guard.isActive(); // fired == timed out
    return !dismissed;
}

} // namespace

SecretServiceStore::SecretServiceStore() {
    registerTypes();

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCInfo(lcSecret) << "no session bus; pairing keys stay in the config file";
        return;
    }

    QDBusInterface service(QLatin1String(kService), QLatin1String(kServicePath),
                           QLatin1String(kServiceIface), bus);
    service.setTimeout(kCallTimeoutMs);
    if (!service.isValid()) {
        qCInfo(lcSecret) << "no Secret Service on the bus; pairing keys stay in the config file";
        return;
    }

    // OpenSession is also the liveness probe: a service that is on the bus but
    // cannot open a session is no more usable than one that is absent.
    const QDBusMessage reply =
        service.call(QStringLiteral("OpenSession"), QLatin1String(kPlainAlgorithm),
                     QVariant::fromValue(QDBusVariant(QString())));
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() < 2) {
        qCInfo(lcSecret) << "Secret Service refused a session:" << reply.errorMessage();
        return;
    }

    const auto path = qdbus_cast<QDBusObjectPath>(reply.arguments().at(1));
    if (isNullPath(path)) { return; }

    sessionPath_ = path.path();
    available_ = true;
    qCInfo(lcSecret) << "pairing keys are stored in the desktop keyring";
}

std::optional<QString> SecretServiceStore::read(const QString& id) const {
    if (!available_) { return std::nullopt; }

    QDBusInterface service(QLatin1String(kService), QLatin1String(kServicePath),
                           QLatin1String(kServiceIface), QDBusConnection::sessionBus());
    service.setTimeout(kCallTimeoutMs);

    const QDBusMessage found =
        service.call(QStringLiteral("SearchItems"), QVariant::fromValue(attributesFor(id)));
    if (found.type() == QDBusMessage::ErrorMessage || found.arguments().isEmpty()) {
        return std::nullopt;
    }

    auto unlocked = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(0));
    if (unlocked.isEmpty() && found.arguments().size() > 1) {
        // Locked: ask for an unlock, which may put a password prompt on screen.
        auto locked = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(1));
        if (locked.isEmpty()) { return std::nullopt; }

        const QDBusMessage unlockReply =
            service.call(QStringLiteral("Unlock"), QVariant::fromValue(locked));
        if (unlockReply.type() == QDBusMessage::ErrorMessage ||
            unlockReply.arguments().size() < 2) {
            return std::nullopt;
        }
        if (!awaitPrompt(qdbus_cast<QDBusObjectPath>(unlockReply.arguments().at(1)))) {
            return std::nullopt;
        }
        unlocked = qdbus_cast<QList<QDBusObjectPath>>(unlockReply.arguments().at(0));
        if (unlocked.isEmpty()) { unlocked = locked; }
    }
    if (unlocked.isEmpty()) { return std::nullopt; }

    QDBusInterface item(QLatin1String(kService), unlocked.first().path(), QLatin1String(kItemIface),
                        QDBusConnection::sessionBus());
    item.setTimeout(kCallTimeoutMs);
    const QDBusMessage secret =
        item.call(QStringLiteral("GetSecret"), QVariant::fromValue(QDBusObjectPath(sessionPath_)));
    if (secret.type() == QDBusMessage::ErrorMessage || secret.arguments().isEmpty()) {
        return std::nullopt;
    }

    SecretValue value;
    const auto arg = secret.arguments().at(0).value<QDBusArgument>();
    arg >> value;
    if (value.value.isEmpty()) { return std::nullopt; }
    return QString::fromUtf8(value.value);
}

bool SecretServiceStore::write(const QString& id, const QString& secretHex) {
    if (!available_) { return false; }

    QDBusInterface collection(QLatin1String(kService), QLatin1String(kDefaultCollection),
                              QLatin1String(kCollectionIface), QDBusConnection::sessionBus());
    collection.setTimeout(kCallTimeoutMs);
    if (!collection.isValid()) { return false; }

    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"),
                      QStringLiteral("Dish pairing key — %1").arg(id));
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                      QVariant::fromValue(attributesFor(id)));

    SecretValue value;
    value.session = QDBusObjectPath(sessionPath_);
    value.value = secretHex.toUtf8();
    value.contentType = QStringLiteral("text/plain");

    // replace=true: re-pairing must overwrite, not accumulate a second item that
    // SearchItems could then return in either order.
    const QDBusMessage reply =
        collection.call(QStringLiteral("CreateItem"), properties, QVariant::fromValue(value), true);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() < 2) {
        qCWarning(lcSecret) << "keyring write failed:" << reply.errorMessage();
        return false;
    }
    if (!awaitPrompt(qdbus_cast<QDBusObjectPath>(reply.arguments().at(1)))) { return false; }
    return !isNullPath(qdbus_cast<QDBusObjectPath>(reply.arguments().at(0)));
}

bool SecretServiceStore::erase(const QString& id) {
    if (!available_) { return false; }

    QDBusInterface service(QLatin1String(kService), QLatin1String(kServicePath),
                           QLatin1String(kServiceIface), QDBusConnection::sessionBus());
    service.setTimeout(kCallTimeoutMs);
    const QDBusMessage found =
        service.call(QStringLiteral("SearchItems"), QVariant::fromValue(attributesFor(id)));
    if (found.type() == QDBusMessage::ErrorMessage || found.arguments().isEmpty()) { return false; }

    auto paths = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(0));
    if (found.arguments().size() > 1) {
        paths += qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(1));
    }
    // Nothing stored is already the desired end state.
    if (paths.isEmpty()) { return true; }

    bool ok = true;
    for (const auto& path : paths) {
        QDBusInterface item(QLatin1String(kService), path.path(), QLatin1String(kItemIface),
                            QDBusConnection::sessionBus());
        item.setTimeout(kCallTimeoutMs);
        const QDBusMessage reply = item.call(QStringLiteral("Delete"));
        if (reply.type() == QDBusMessage::ErrorMessage) {
            ok = false;
            continue;
        }
        if (!reply.arguments().isEmpty()) {
            ok = awaitPrompt(qdbus_cast<QDBusObjectPath>(reply.arguments().at(0))) && ok;
        }
    }
    return ok;
}

std::vector<QString> SecretServiceStore::ids() const {
    std::vector<QString> out;
    if (!available_) { return out; }

    QDBusInterface service(QLatin1String(kService), QLatin1String(kServicePath),
                           QLatin1String(kServiceIface), QDBusConnection::sessionBus());
    service.setTimeout(kCallTimeoutMs);

    // Every Dish item, without the per-satellite attribute.
    StringMap attrs;
    attrs.insert(QStringLiteral("xdg:schema"), QLatin1String(kSchema));
    attrs.insert(QStringLiteral("application"), QStringLiteral("dish"));

    const QDBusMessage found =
        service.call(QStringLiteral("SearchItems"), QVariant::fromValue(attrs));
    if (found.type() == QDBusMessage::ErrorMessage || found.arguments().isEmpty()) { return out; }

    auto paths = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(0));
    if (found.arguments().size() > 1) {
        paths += qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(1));
    }
    out.reserve(static_cast<std::size_t>(paths.size()));
    for (const auto& path : paths) {
        // The id is read back off the item's attributes rather than its label:
        // the label is a human string and a keyring UI lets the user edit it.
        QDBusInterface item(QLatin1String(kService), path.path(),
                            QStringLiteral("org.freedesktop.DBus.Properties"),
                            QDBusConnection::sessionBus());
        item.setTimeout(kCallTimeoutMs);
        const QDBusMessage reply = item.call(QStringLiteral("Get"), QLatin1String(kItemIface),
                                             QStringLiteral("Attributes"));
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) { continue; }
        const auto variant = reply.arguments().at(0).value<QDBusVariant>().variant();
        const auto map = qdbus_cast<StringMap>(variant);
        const auto it = map.find(QStringLiteral("satellite"));
        if (it != map.end() && !it.value().isEmpty()) { out.push_back(it.value()); }
    }
    return out;
}

} // namespace dish::source
