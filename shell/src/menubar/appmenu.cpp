// SPDX-License-Identifier: LGPL-2.1-or-later
#include "appmenu.h"

#include "aqua.h"
#include "x11.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QMenu>

Q_DECLARE_METATYPE(DBusMenuItem)

static const char *const registrarService = "com.canonical.AppMenu.Registrar";
static const char *const registrarPath = "/com/canonical/AppMenu/Registrar";
static const char *const menuInterface = "com.canonical.dbusmenu";

// (ia{sv}av): id, properties, children (each a variant holding the same structure)
QDBusArgument &operator<<(QDBusArgument &arg, const DBusMenuItem &item)
{
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.beginArray(qMetaTypeId<QDBusVariant>());
    for (const DBusMenuItem &child : item.children)
        arg << QDBusVariant(QVariant::fromValue(child));
    arg.endArray();
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, DBusMenuItem &item)
{
    arg.beginStructure();
    arg >> item.id >> item.properties;
    arg.beginArray();
    item.children.clear();
    while (!arg.atEnd()) {
        QDBusVariant v;
        arg >> v;
        DBusMenuItem child;
        v.variant().value<QDBusArgument>() >> child;
        item.children.append(child);
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

QString dbusMenuLabel(const QString &label)
{
    // "_File" -> "File", "__" -> "_"
    QString out;
    for (int i = 0; i < label.size(); ++i) {
        if (label[i] == '_') {
            if (i + 1 < label.size() && label[i + 1] == '_')
                out += '_', ++i;
            continue;
        }
        out += label[i];
    }
    return out;
}

// ---- the registrar

AppMenuRegistrar::AppMenuRegistrar(QObject *parent) : QObject(parent)
{
    qDBusRegisterMetaType<DBusMenuItem>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(bus);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &service) {
        // an application gone: its windows' menus with it
        for (auto it = m_menus.begin(); it != m_menus.end();) {
            if (it->first == service) {
                uint window = it.key();
                it = m_menus.erase(it);
                emit WindowUnregistered(window);
                emit changed(window);
            } else {
                ++it;
            }
        }
        m_watcher->removeWatchedService(service);
    });
    m_registered = bus.registerObject(registrarPath, this, QDBusConnection::ExportScriptableContents) &&
                   bus.registerService(registrarService);
    if (!m_registered)
        qWarning("SystemUIServer: %s is taken (another global menu runs): the menu bar shows the common menus", registrarService);
}

void AppMenuRegistrar::RegisterWindow(uint windowId, const QDBusObjectPath &menuObjectPath)
{
    const QString service = message().service();
    m_menus.insert(windowId, {service, menuObjectPath.path()});
    m_watcher->addWatchedService(service);
    emit WindowRegistered(windowId, service, menuObjectPath);
    emit changed(windowId);
}

void AppMenuRegistrar::UnregisterWindow(uint windowId)
{
    if (m_menus.remove(windowId)) {
        emit WindowUnregistered(windowId);
        emit changed(windowId);
    }
}

QString AppMenuRegistrar::GetMenuForWindow(uint windowId, QDBusObjectPath &menuObjectPath)
{
    auto menu = m_menus.value(windowId);
    menuObjectPath = QDBusObjectPath(menu.second.isEmpty() ? QStringLiteral("/") : menu.second);
    return menu.first;
}

QPair<QString, QString> AppMenuRegistrar::menuFor(uint window) const
{
    auto it = m_menus.constFind(window);
    if (it != m_menus.constEnd())
        return *it;
    // KDE's way: X properties on the window
    QString service = QString::fromUtf8(X11::bytes(window, X11::atom("_KDE_NET_WM_APPMENU_SERVICE_NAME"), XCB_ATOM_STRING));
    QString path = QString::fromUtf8(X11::bytes(window, X11::atom("_KDE_NET_WM_APPMENU_OBJECT_PATH"), XCB_ATOM_STRING));
    if (!service.isEmpty() && !path.isEmpty())
        return {service, path};
    return {};
}

// ---- a window's menus

DBusMenu::DBusMenu(const QString &service, const QString &path, QObject *parent)
    : QObject(parent), m_service(service), m_path(path)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_refetch.setSingleShot(true);
    m_refetch.setInterval(50);
    connect(&m_refetch, &QTimer::timeout, this, &DBusMenu::fetch);
    // whatever changes, the layout again (they come in bursts: coalesced)
    bus.connect(service, path, menuInterface, "LayoutUpdated", &m_refetch, SLOT(start()));
    bus.connect(service, path, menuInterface, "ItemsPropertiesUpdated", &m_refetch, SLOT(start()));
    fetch();
}

void DBusMenu::fetch()
{
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_path, menuInterface, "GetLayout");
    call << 0 << -1 << QStringList();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<uint, DBusMenuItem> reply = *w;
        w->deleteLater();
        if (reply.isError())
            return;
        m_root = reply.argumentAt<1>();
        m_ready = true;
        // menus open now take the new items at once
        for (auto it = m_open.constBegin(); it != m_open.constEnd(); ++it)
            if (it.key()->isVisible())
                fill(it.key(), it.value());
        emit layoutChanged();
    });
}

static bool visible(const DBusMenuItem &i) { return i.properties.value("visible", true).toBool(); }

QList<DBusMenuItem> DBusMenu::topLevel() const
{
    QList<DBusMenuItem> out;
    for (const DBusMenuItem &i : m_root.children)
        if (visible(i) && i.properties.value("type").toString() != "separator")
            out.append(i);
    return out;
}

const DBusMenuItem *DBusMenu::item(int id) const
{
    QList<const DBusMenuItem *> todo{&m_root};
    while (!todo.isEmpty()) {
        const DBusMenuItem *i = todo.takeFirst();
        if (i->id == id)
            return i;
        for (const DBusMenuItem &c : i->children)
            todo.append(&c);
    }
    return nullptr;
}

QMenu *DBusMenu::createMenu(int id, const QString &title)
{
    auto *menu = new QMenu(title);
    m_open.insert(menu, id);
    connect(menu, &QObject::destroyed, this, [this, menu] { m_open.remove(menu); });
    connect(menu, &QMenu::aboutToShow, this, [this, menu, id] {
        aboutToShow(id);
        fill(menu, id);
    });
    fill(menu, id);
    return menu;
}

void DBusMenu::fill(QMenu *menu, int id)
{
    menu->clear();
    if (const DBusMenuItem *i = item(id))
        addItems(menu, i->children);
    Aqua::prepareMenu(menu);
}

static QString shortcutText(const QVariant &value)
{
    // aas: the first combination, as the Mac writes it (⌃⌥⇧⌘ then the key)
    if (!value.canConvert<QDBusArgument>())
        return {};
    const QDBusArgument arg = value.value<QDBusArgument>();
    QStringList combo;
    arg.beginArray();
    if (!arg.atEnd())
        arg >> combo;
    while (!arg.atEnd()) {
        QStringList skip;
        arg >> skip;
    }
    arg.endArray();
    QString mods, key;
    for (const QString &k : combo) {
        if (k == "Control")
            mods += QString::fromUtf8("⌃");
        else if (k == "Alt")
            mods += QString::fromUtf8("⌥");
        else if (k == "Shift")
            mods += QString::fromUtf8("⇧");
        else if (k == "Super")
            mods += QString::fromUtf8("⌘");
        else
            key = k.size() == 1 ? k.toUpper() : k;
    }
    return mods + key;
}

void DBusMenu::addItems(QMenu *menu, const QList<DBusMenuItem> &items)
{
    for (const DBusMenuItem &i : items) {
        if (!visible(i))
            continue;
        const QVariantMap &p = i.properties;
        if (p.value("type").toString() == "separator") {
            menu->addSeparator();
            continue;
        }
        QString label = dbusMenuLabel(p.value("label").toString());
        if (p.value("children-display").toString() == "submenu" || !i.children.isEmpty()) {
            QMenu *sub = createMenu(i.id, label);
            sub->setParent(menu, sub->windowFlags());
            menu->addMenu(sub)->setEnabled(p.value("enabled", true).toBool());
            continue;
        }
        QString shortcut = shortcutText(p.value("shortcut"));
        QAction *a = menu->addAction(shortcut.isEmpty() ? label : label + '\t' + shortcut);
        a->setEnabled(p.value("enabled", true).toBool());
        QString toggle = p.value("toggle-type").toString();
        if (toggle == "checkmark" || toggle == "radio") {
            a->setCheckable(true);
            a->setChecked(p.value("toggle-state").toInt() == 1);
        }
        const int id = i.id;
        connect(a, &QAction::triggered, this, [this, id] { activate(id); });
    }
}

void DBusMenu::activate(int id)
{
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_path, menuInterface, "Event");
    call << id << QStringLiteral("clicked") << QVariant::fromValue(QDBusVariant(QString())) << 0u;
    QDBusConnection::sessionBus().asyncCall(call);
}

void DBusMenu::aboutToShow(int id)
{
    // the application may fill the menu now; it tells with LayoutUpdated, and the open menu follows
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_path, menuInterface, "AboutToShow");
    call << id;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<bool> reply = *w;
        w->deleteLater();
        if (!reply.isError() && reply.value())
            m_refetch.start();
    });
}
