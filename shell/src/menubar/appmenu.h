// SPDX-License-Identifier: LGPL-2.1-or-later
// The applications' own menus in the menu bar, over D-Bus, as Linux desktops with a global menu do:
//
//  - AppMenuRegistrar is com.canonical.AppMenu.Registrar: applications (Qt's QDBusMenuBar, GTK's
//    appmenu-gtk-module, KDE's KXmlGui) register which D-Bus object holds each window's menus.
//    KDE applications may name it in X properties instead (_KDE_NET_WM_APPMENU_SERVICE_NAME, ..._OBJECT_PATH).
//  - DBusMenu reads such an object (com.canonical.dbusmenu: GetLayout, AboutToShow, Event, LayoutUpdated)
//    and fills QMenus with it; choosing an item sends its "clicked" event back.
#pragma once

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

class QDBusServiceWatcher;
class QMenu;

struct DBusMenuItem {
    int id = 0;
    QVariantMap properties;
    QList<DBusMenuItem> children;
};

class AppMenuRegistrar : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.AppMenu.Registrar")
public:
    explicit AppMenuRegistrar(QObject *parent = nullptr);
    bool registered() const { return m_registered; }
    // the D-Bus service and object of a window's menus, or empty
    QPair<QString, QString> menuFor(uint window) const;

public slots:
    Q_SCRIPTABLE void RegisterWindow(uint windowId, const QDBusObjectPath &menuObjectPath);
    Q_SCRIPTABLE void UnregisterWindow(uint windowId);
    Q_SCRIPTABLE QString GetMenuForWindow(uint windowId, QDBusObjectPath &menuObjectPath);

signals:
    Q_SCRIPTABLE void WindowRegistered(uint windowId, const QString &service, const QDBusObjectPath &menuObjectPath);
    Q_SCRIPTABLE void WindowUnregistered(uint windowId);
    void changed(uint windowId);

private:
    QHash<uint, QPair<QString, QString>> m_menus;
    QDBusServiceWatcher *m_watcher;
    bool m_registered = false;
};

class DBusMenu : public QObject {
    Q_OBJECT
public:
    DBusMenu(const QString &service, const QString &path, QObject *parent = nullptr);
    QString service() const { return m_service; }
    QString path() const { return m_path; }
    bool ready() const { return m_ready; }
    // the menu bar's titles: the root's visible children
    QList<DBusMenuItem> topLevel() const;
    // a top-level menu, filled now and again each time it opens (the application may fill it lazily)
    QMenu *createMenu(int id, const QString &title);

signals:
    void layoutChanged();

private:
    void fetch();
    void fill(QMenu *menu, int id);
    void addItems(QMenu *menu, const QList<DBusMenuItem> &items);
    const DBusMenuItem *item(int id) const;
    void activate(int id);
    void aboutToShow(int id);

    QString m_service, m_path;
    DBusMenuItem m_root;
    bool m_ready = false;
    QTimer m_refetch;
    QHash<QMenu *, int> m_open;   // the menus made here, by the item they show
};

QString dbusMenuLabel(const QString &label);
