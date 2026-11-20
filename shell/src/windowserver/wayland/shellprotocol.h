// SPDX-License-Identifier: LGPL-2.1-or-later
// sl_window_server_v1 (protocols/snowleopard-window-server-v1.xml): the CGS calls of the shell's
// programs, the Dock's, SystemUIServer's, the Finder's, loginwindow's, screencapture's; and KDE's
// appmenu (org_kde_kwin_appmenu: a Qt application's D-Bus menu, for the menu bar).
#pragma once

#include "qwayland-server-appmenu.h"
#include "qwayland-server-snowleopard-window-server-v1.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

class QWaylandSurface;
class Server;
class Window;

class ShellProtocol : public QObject, public SLWaylandServer::sl_window_server_v1 {
    Q_OBJECT
public:
    enum Flags : uint32_t {
        Minimized = 1, Hidden = 2, Utility = 4, Dialog = 8, SkipTaskbar = 16, Fullscreen = 32, Zoomed = 64, X11Window = 128, Above = 256
    };
    explicit ShellProtocol(Server *server);

    void windowChanged(Window *w);
    void windowRemoved(Window *w);
    // a symbolic hot key pressed: true when a connection has it
    bool hotKey(int hotKey);
    // a key for the application in front without a window: true when it has a connection to get it
    bool keyForFrontApp(uint32_t keysym, const QString &text, uint32_t modifiers);
    // a Qt application's menu (org_kde_kwin_appmenu) for its surface
    void setMenu(QWaylandSurface *surface, const QString &service, const QString &path);
    QPair<QString, QString> menu(QWaylandSurface *surface) const { return m_menus.value(surface); }

protected:
    void sl_window_server_v1_bind_resource(Resource *resource) override;
    void sl_window_server_v1_destroy_resource(Resource *resource) override;
    void sl_window_server_v1_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void sl_window_server_v1_activate(Resource *, uint32_t window) override;
    void sl_window_server_v1_close(Resource *, uint32_t window) override;
    void sl_window_server_v1_minimize(Resource *, uint32_t window) override;
    void sl_window_server_v1_hide(Resource *, uint32_t window) override;
    void sl_window_server_v1_set_icon_geometry(Resource *, uint32_t window, int32_t x, int32_t y, int32_t w, int32_t h) override;
    void sl_window_server_v1_set_front_app(Resource *resource, uint32_t pid) override;
    void sl_window_server_v1_register_hot_keys(Resource *resource, wl_array *hotKeys) override;
    void sl_window_server_v1_set_key_equivalents(Resource *resource, wl_array *keysyms) override;
    void sl_window_server_v1_set_magic_mirror(Resource *, const QString &layer, uint32_t direction, wl_array *quad) override;
    void sl_window_server_v1_capture_window(Resource *resource, uint32_t id, uint32_t window, uint32_t shadow, int32_t fd) override;
    void sl_window_server_v1_capture_screen(Resource *resource, uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fd) override;
    void sl_window_server_v1_pick_window(Resource *resource, uint32_t id, int32_t x, int32_t y) override;

private:
    void sendWindow(Resource *r, Window *w);
    void sendState(Resource *r);
    void flush();
    void schedule() { if (!m_flush.isActive()) m_flush.start(); }

    Server *m_server;
    QHash<Resource *, QList<uint32_t>> m_hotKeys;
    QSet<uint32_t> m_dirty;
    QList<uint32_t> m_removed;
    bool m_stateDirty = false;
    QTimer m_flush;
    QHash<QWaylandSurface *, QPair<QString, QString>> m_menus;
};

class AppMenuManager : public SLWaylandServer::org_kde_kwin_appmenu_manager {
public:
    AppMenuManager(Server *server, ShellProtocol *shell);

protected:
    void org_kde_kwin_appmenu_manager_create(Resource *resource, uint32_t id, struct ::wl_resource *surface) override;

private:
    ShellProtocol *m_shell;
};
