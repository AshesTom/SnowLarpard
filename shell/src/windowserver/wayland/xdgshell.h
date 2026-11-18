// SPDX-License-Identifier: LGPL-2.1-or-later
// xdg-shell and xdg-decoration, the server's own (Qt's refuses a popup without a parent, which a
// layer surface's menus are: the Dock's, the desktop's, the menu bar's): the applications' windows
// (toplevels, which Window shows with their title bar), their menus (popups, placed by their
// positioner), and the title bars WindowServer draws (server side) or the application's own.
#pragma once

#include "qwayland-server-xdg-decoration-unstable-v1.h"
#include "qwayland-server-xdg-shell.h"
#include "layershell.h"

#include <QObject>
#include <QWaylandQuickItem>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>

class QWaylandSurface;
class QWaylandQuickItem;
class Server;
class XdgSurface;
class XdgToplevel;
class XdgPopup;
class LayerSurface;
class XdgWindow;

// xdg_positioner's rules, kept by the popup that used them
struct PositionerState {
    QSize size;
    QRect anchorRect;
    uint32_t anchor = 0, gravity = 0, constraints = 0;
    QPoint offset;
    bool reactive = false;
    // the popup's place (its window geometry's top left) from its parent's window geometry, kept
    // inside `bounds` (the screen, in the same coordinates) as the constraint adjustments allow
    QRect place(const QRect &bounds) const;
};

class XdgShell : public QObject, public SLWaylandServer::xdg_wm_base {
    Q_OBJECT
public:
    explicit XdgShell(Server *server);
    Server *server() const { return m_server; }

signals:
    void toplevelCreated(XdgToplevel *toplevel);
    void popupCreated(XdgPopup *popup);

protected:
    void xdg_wm_base_create_positioner(Resource *resource, uint32_t id) override;
    void xdg_wm_base_get_xdg_surface(Resource *resource, uint32_t id, struct ::wl_resource *surface) override;
    void xdg_wm_base_pong(Resource *, uint32_t) override {}
    void xdg_wm_base_destroy(Resource *resource) override;

private:
    Server *m_server;
};

class XdgPositioner : public SLWaylandServer::xdg_positioner {
public:
    XdgPositioner(wl_client *client, uint32_t id, int version) : xdg_positioner(client, id, version) {}
    static XdgPositioner *from(wl_resource *r);
    PositionerState state;

protected:
    void xdg_positioner_destroy_resource(Resource *) override { delete this; }
    void xdg_positioner_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void xdg_positioner_set_size(Resource *, int32_t w, int32_t h) override { state.size = QSize(w, h); }
    void xdg_positioner_set_anchor_rect(Resource *, int32_t x, int32_t y, int32_t w, int32_t h) override { state.anchorRect = QRect(x, y, w, h); }
    void xdg_positioner_set_anchor(Resource *, uint32_t a) override { state.anchor = a; }
    void xdg_positioner_set_gravity(Resource *, uint32_t g) override { state.gravity = g; }
    void xdg_positioner_set_constraint_adjustment(Resource *, uint32_t c) override { state.constraints = c; }
    void xdg_positioner_set_offset(Resource *, int32_t x, int32_t y) override { state.offset = QPoint(x, y); }
    void xdg_positioner_set_reactive(Resource *) override { state.reactive = true; }
    void xdg_positioner_set_parent_size(Resource *, int32_t, int32_t) override {}
    void xdg_positioner_set_parent_configure(Resource *, uint32_t) override {}
};

class XdgSurface : public QObject, public SLWaylandServer::xdg_surface {
    Q_OBJECT
public:
    XdgSurface(XdgShell *shell, QWaylandSurface *surface, wl_client *client, uint32_t id, int version);
    ~XdgSurface() override;
    static XdgSurface *from(wl_resource *r);

    XdgShell *shell() const { return m_shell; }
    QWaylandSurface *surface() const { return m_surface; }
    XdgToplevel *toplevel() const { return m_toplevel; }
    XdgPopup *popup() const { return m_popup; }
    // xdg_surface.set_window_geometry's, as committed (the surface's own rectangle when none)
    QRect windowGeometry() const;
    bool configured() const { return m_configured; }
    bool initialConfigureSent() const { return m_initialSent; }
    uint32_t sendConfigure();   // the role's state then xdg_surface.configure; its serial
    // the item that shows it (a toplevel's in its frame, a popup's in the popups' layer)
    QPointer<QWaylandQuickItem> item;

signals:
    void committed();
    void windowGeometryChanged();
    void destroyed(XdgSurface *self);

protected:
    void xdg_surface_destroy_resource(Resource *) override;
    void xdg_surface_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void xdg_surface_get_toplevel(Resource *resource, uint32_t id) override;
    void xdg_surface_get_popup(Resource *resource, uint32_t id, struct ::wl_resource *parent, struct ::wl_resource *positioner) override;
    void xdg_surface_set_window_geometry(Resource *, int32_t x, int32_t y, int32_t w, int32_t h) override;
    void xdg_surface_ack_configure(Resource *, uint32_t serial) override;

private:
    friend class XdgToplevel;
    friend class XdgPopup;
    void handleCommit();

    XdgShell *m_shell;
    QWaylandSurface *m_surface;
    XdgToplevel *m_toplevel = nullptr;
    XdgPopup *m_popup = nullptr;
    QRect m_pendingGeometry, m_geometry;
    bool m_hasPendingGeometry = false;
    bool m_configured = false, m_initialSent = false;
    uint32_t m_lastSerial = 0;
};

class XdgToplevel : public QObject, public SLWaylandServer::xdg_toplevel {
    Q_OBJECT
public:
    enum State : uint32_t { Maximized = 1, Fullscreen = 2, Resizing = 3, Activated = 4 };
    XdgToplevel(XdgSurface *surface, wl_client *client, uint32_t id, int version);
    ~XdgToplevel() override;
    static XdgToplevel *from(wl_resource *r);

    XdgSurface *xdgSurface() const { return m_surface; }
    QString title() const { return m_title; }
    QString appId() const { return m_appId; }
    XdgToplevel *parentToplevel() const { return m_parent; }
    QSize minSize() const { return m_minSize; }
    QSize maxSize() const { return m_maxSize; }
    bool serverSideDecoration() const;

    // the size (0 x 0: the application's own) and states the next configure gives
    void setConfigure(const QSize &size, bool activated, bool maximized, bool fullscreen, bool resizing);
    void sendClose() { send_close(); }
    QSize configuredSize() const { return m_size; }

    XdgWindow *window = nullptr;   // the window that shows it, once mapped
    class Decoration *decoration = nullptr;

signals:
    void titleChanged();
    void appIdChanged();
    void parentChanged();
    void moveRequested();
    void resizeRequested(Qt::Edges edges);
    void maximizeRequested(bool on);
    void fullscreenRequested(bool on);
    void minimizeRequested();
    void decorationChanged();

protected:
    void xdg_toplevel_destroy_resource(Resource *) override;
    void xdg_toplevel_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void xdg_toplevel_set_parent(Resource *, struct ::wl_resource *parent) override;
    void xdg_toplevel_set_title(Resource *, const QString &title) override;
    void xdg_toplevel_set_app_id(Resource *, const QString &appId) override;
    void xdg_toplevel_show_window_menu(Resource *, struct ::wl_resource *, uint32_t, int32_t, int32_t) override {}
    void xdg_toplevel_move(Resource *, struct ::wl_resource *, uint32_t) override { emit moveRequested(); }
    void xdg_toplevel_resize(Resource *, struct ::wl_resource *, uint32_t, uint32_t edges) override;
    void xdg_toplevel_set_max_size(Resource *, int32_t w, int32_t h) override { m_maxSize = QSize(w, h); }
    void xdg_toplevel_set_min_size(Resource *, int32_t w, int32_t h) override { m_minSize = QSize(w, h); }
    void xdg_toplevel_set_maximized(Resource *) override { emit maximizeRequested(true); }
    void xdg_toplevel_unset_maximized(Resource *) override { emit maximizeRequested(false); }
    void xdg_toplevel_set_fullscreen(Resource *, struct ::wl_resource *) override { emit fullscreenRequested(true); }
    void xdg_toplevel_unset_fullscreen(Resource *) override { emit fullscreenRequested(false); }
    void xdg_toplevel_set_minimized(Resource *) override { emit minimizeRequested(); }

private:
    friend class XdgSurface;
    void sendState();
    XdgSurface *m_surface;
    QPointer<XdgToplevel> m_parent;
    QString m_title, m_appId;
    QSize m_minSize, m_maxSize;
    QSize m_size;
    bool m_activated = false, m_maximized = false, m_fullscreen = false, m_resizing = false;
};

class XdgPopup : public QObject, public SLWaylandServer::xdg_popup {
    Q_OBJECT
public:
    XdgPopup(XdgSurface *surface, XdgSurface *parent, const PositionerState &positioner, wl_client *client, uint32_t id, int version);
    ~XdgPopup() override;

    XdgSurface *xdgSurface() const { return m_surface; }
    // its parent: an xdg surface, or a layer surface (zwlr_layer_surface_v1.get_popup)
    XdgSurface *parentXdgSurface() const { return m_parent; }
    LayerSurface *parentLayer() const { return m_parentLayer; }
    void setParentLayer(LayerSurface *layer);
    const PositionerState &positioner() const { return m_positioner; }
    QRect geometry() const { return m_geometry; }   // from the parent's window geometry
    void setGeometry(const QRect &r) { m_geometry = r; }
    bool grabbed() const { return m_grab; }
    void dismiss() { send_popup_done(); }

signals:
    void grabRequested();
    void repositioned();
    void parentAttached();

protected:
    void xdg_popup_destroy_resource(Resource *) override;
    void xdg_popup_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void xdg_popup_grab(Resource *, struct ::wl_resource *, uint32_t) override;
    void xdg_popup_reposition(Resource *, struct ::wl_resource *positioner, uint32_t token) override;

private:
    friend class XdgSurface;
    XdgSurface *m_surface;
    QPointer<XdgSurface> m_parent;
    QPointer<LayerSurface> m_parentLayer;
    PositionerState m_positioner;
    QRect m_geometry;
    bool m_grab = false;
    uint32_t m_repositionToken = 0;
    bool m_reposition = false;
};

class XdgDecorationManager : public SLWaylandServer::zxdg_decoration_manager_v1 {
public:
    explicit XdgDecorationManager(wl_display *display);

protected:
    void zxdg_decoration_manager_v1_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void zxdg_decoration_manager_v1_get_toplevel_decoration(Resource *resource, uint32_t id, struct ::wl_resource *toplevel) override;
};

class Decoration : public SLWaylandServer::zxdg_toplevel_decoration_v1 {
public:
    Decoration(XdgToplevel *toplevel, wl_client *client, uint32_t id, int version);
    ~Decoration() override;
    // what the next configure says (WindowServer draws the title bar unless the application asks
    // to draw its own)
    uint32_t mode() const { return m_mode; }
    void sendMode() { send_configure(m_mode); }

protected:
    void zxdg_toplevel_decoration_v1_destroy_resource(Resource *) override { delete this; }
    void zxdg_toplevel_decoration_v1_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void zxdg_toplevel_decoration_v1_set_mode(Resource *, uint32_t mode) override;
    void zxdg_toplevel_decoration_v1_unset_mode(Resource *) override;

private:
    QPointer<XdgToplevel> m_toplevel;
    uint32_t m_mode = 2;   // server_side
};
