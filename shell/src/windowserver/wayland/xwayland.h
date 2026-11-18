// SPDX-License-Identifier: LGPL-2.1-or-later
// The X11 programs on the Wayland server: an Xwayland of its own, rootless, started when the first
// X11 client connects to its display (the sockets are the server's until then), and its window
// manager here, as the Mac's window server is the X11.app's quartz-wm's: each X window is a Window
// of the server (its title bar, shadow, the Dock's list), its menus and tooltips (override
// redirect) over them. The X window and its wl_surface meet by Xwayland's WL_SURFACE_ID message.
#pragma once

#include "window.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSocketNotifier>
#include <QString>
#include <xcb/xcb.h>

class QWaylandClient;
class QWaylandQuickItem;
class QWaylandSurface;
class Server;
class Xwayland;

class XWindow : public Window {
    Q_OBJECT
public:
    XWindow(Server *server, Xwayland *xwm, xcb_window_t window, QWaylandSurface *surface);
    Kind kind() const override { return X11; }
    xcb_window_t xid() const { return m_xid; }
    QString title() const override { return m_title; }
    QString appId() const override { return m_class; }
    uint32_t pid() const override { return m_pid ? m_pid : Window::pid(); }
    Window *parentWindow() const override;
    bool decorated() const override { return m_decorated && !m_fullscreen; }
    bool utility() const override { return m_utility; }
    bool skipTaskbar() const override { return m_skipTaskbar; }
    QSize minimumSize() const override { return m_minSize; }
    QString menuService() const override { return m_menuService; }
    QString menuPath() const override { return m_menuPath; }
    void configure(const QSize &content) override;
    void sendClose() override;
    void moved() override;
    void activated(bool active) override;

    void readProperties();
    void readProperty(xcb_atom_t atom);
    void setGeometryFromX(const QRect &r);   // where the X window is (its own request)
    QRect xGeometry() const { return m_xGeometry; }

private:
    void sendConfigure();
    Xwayland *m_xwm;
    xcb_window_t m_xid;
    QRect m_xGeometry;
    QString m_title, m_class, m_menuService, m_menuPath;
    uint32_t m_pid = 0;
    xcb_window_t m_transientFor = 0;
    bool m_decorated = true, m_utility = false, m_skipTaskbar = false, m_deleteWindow = false, m_takeFocus = false, m_acceptsInput = true;
    QSize m_minSize;
};

class Xwayland : public QObject {
    Q_OBJECT
public:
    explicit Xwayland(Server *server);
    ~Xwayland() override;
    // the X display's sockets, listened to here until a client comes (false without a free display)
    bool listen();
    QString displayName() const { return m_display >= 0 ? QStringLiteral(":%1").arg(m_display) : QString(); }

    xcb_connection_t *conn() const { return m_conn; }
    xcb_atom_t atom(const char *name);
    XWindow *find(xcb_window_t w) const;

private:
    void start();
    void startWm();
    void readEvents();
    void handleEvent(xcb_generic_event_t *e);
    void surfaceCreated(QWaylandSurface *s);
    void associate(xcb_window_t w, uint32_t surfaceId);
    void tryAssociate(xcb_window_t w);
    void mapManaged(xcb_window_t w, QWaylandSurface *s);
    void mapOverride(xcb_window_t w, QWaylandSurface *s);
    void unmap(xcb_window_t w);
    void updateLists();
    void setActive(xcb_window_t w);
    void cleanup();

    Server *m_server;
    int m_display = -1;
    int m_listen[2] = {-1, -1};
    QSocketNotifier *m_listenNotifier[2] = {};
    QProcess *m_process = nullptr;
    int m_wmFd = -1;
    QWaylandClient *m_client = nullptr;
    xcb_connection_t *m_conn = nullptr;
    xcb_window_t m_root = 0, m_check = 0;
    QSocketNotifier *m_notifier = nullptr;
    QHash<QByteArray, xcb_atom_t> m_atoms;

    struct Unmanaged {
        xcb_window_t window = 0;
        bool overrideRedirect = false;
        QRect geometry;
        uint32_t surfaceId = 0;
        bool mapped = false;
        QPointer<QWaylandQuickItem> item;   // an override-redirect window's (a menu, a tooltip)
    };
    QHash<xcb_window_t, Unmanaged> m_known;
    QHash<xcb_window_t, XWindow *> m_windows;
    friend class XWindow;
};
