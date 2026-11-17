// SPDX-License-Identifier: LGPL-2.1-or-later
// SystemUIServer's menu bar: the Apple menu, the active application's name and menus at the left,
// the menu extras at the right.
#pragma once

#include "apps.h"

#include <QPointer>
#include <QVector>
#include <QWidget>
#include <xcb/xcb.h>

class QMenu;
class MenuExtra;
class AppMenuRegistrar;
class DBusMenu;
namespace X11 { class Watcher; }

class MenuBar : public QWidget {
    Q_OBJECT
public:
    explicit MenuBar(QWidget *parent = nullptr);
    static constexpr int Height = 22;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    struct Item {
        QString title;
        QRect rect;
        QMenu *menu = nullptr;
        MenuExtra *extra = nullptr;
        bool apple = false, bold = false;
    };
    void rebuild();
    void layoutItems();
    int itemAt(const QPoint &p) const;
    void open(int index);
    void activeChanged();
    void reloadDesktop();

    QMenu *appleMenu();
    QVector<QMenu *> appMenus(const QString &name, bool finder);
    QMenu *windowMenu();
    QMenu *recentItems(QMenu *parent);

    QVector<Item> m_items;            // left: Apple, the application, its menus; then the extras
    QVector<MenuExtra *> m_extras;    // right to left
    int m_open = -1;
    QPointer<QWidget> m_popup;
    App m_app;
    xcb_window_t m_active = 0;
    bool m_desktopActive = false;   // the desktop (or nothing) in front: the Finder's window commands off
    QImage m_wallpaper;               // the desktop picture: what is behind the bar
    QImage m_generated;               // the bar the window server's filter makes from it
    bool m_generatedApple = false;    // ...with its Apple
    bool m_transparent = true;        // AppleEnableMenuBarTransparency
    X11::Watcher *m_watcher;
    // the active window's own menus, over D-Bus (appmenu.h)
    AppMenuRegistrar *m_registrar;
    QPointer<DBusMenu> m_imported;
    QString m_menuKey;                // its service and object path ("aquakit:<pid>": an AquaKit application's)
    int m_aquaKitPid = 0;             // the AquaKit application whose menus are shown
    uint32_t m_frontSerial = 0;       // _SNOWLEOPARD_FRONT_APP's serial last seen
    xcb_window_t m_frontOver = 0;     // the window active when the windowless application came forward
    bool m_rebuildPending = false;    // the menus changed while one was open
};
