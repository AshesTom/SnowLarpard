// SPDX-License-Identifier: LGPL-2.1-or-later
#include "xdgshell.h"

#include "layershell.h"
#include "server.h"

#include <QWaylandCompositor>
#include <QWaylandSurface>

namespace {
QWaylandSurfaceRole toplevelRole("xdg_toplevel");
QWaylandSurfaceRole popupRole("xdg_popup");

// xdg_positioner's anchor and gravity: 1 top, 2 bottom, 3 left, 4 right, 5 top left, 6 bottom left,
// 7 top right, 8 bottom right
bool hasLeft(uint32_t e) { return e == 3 || e == 5 || e == 6; }
bool hasRight(uint32_t e) { return e == 4 || e == 7 || e == 8; }
bool hasTop(uint32_t e) { return e == 1 || e == 5 || e == 7; }
bool hasBottom(uint32_t e) { return e == 2 || e == 6 || e == 8; }
uint32_t flipX(uint32_t e)
{
    static const uint32_t f[] = {0, 1, 2, 4, 3, 7, 8, 5, 6};
    return e <= 8 ? f[e] : e;
}
uint32_t flipY(uint32_t e)
{
    static const uint32_t f[] = {0, 2, 1, 3, 4, 6, 5, 8, 7};
    return e <= 8 ? f[e] : e;
}
} // namespace

// ---- the positioner: where a popup goes

QRect PositionerState::place(const QRect &bounds) const
{
    auto at = [&](uint32_t a, uint32_t g) {
        const QRect &r = anchorRect;
        const int ax = hasLeft(a) ? r.left() : hasRight(a) ? r.x() + r.width() : r.x() + r.width() / 2;
        const int ay = hasTop(a) ? r.top() : hasBottom(a) ? r.y() + r.height() : r.y() + r.height() / 2;
        const int x = hasLeft(g) ? ax - size.width() : hasRight(g) ? ax : ax - size.width() / 2;
        const int y = hasTop(g) ? ay - size.height() : hasBottom(g) ? ay : ay - size.height() / 2;
        return QRect(QPoint(x, y) + offset, size);
    };
    QRect place = at(anchor, gravity);
    if (bounds.isEmpty())
        return place;
    auto outX = [&](const QRect &p) { return p.left() < bounds.left() || p.right() > bounds.right(); };
    auto outY = [&](const QRect &p) { return p.top() < bounds.top() || p.bottom() > bounds.bottom(); };
    // flip, then slide, then resize, as xdg_positioner.constraint_adjustment orders them
    if (outX(place) && (constraints & 4)) {
        const QRect flipped = at(flipX(anchor), flipX(gravity));
        if (!outX(flipped))
            place.moveLeft(flipped.left());
    }
    if (outY(place) && (constraints & 8)) {
        const QRect flipped = at(flipY(anchor), flipY(gravity));
        if (!outY(flipped))
            place.moveTop(flipped.top());
    }
    if (outX(place) && (constraints & 1)) {
        if (place.right() > bounds.right())
            place.moveRight(bounds.right());
        if (place.left() < bounds.left())
            place.moveLeft(bounds.left());
    }
    if (outY(place) && (constraints & 2)) {
        if (place.bottom() > bounds.bottom())
            place.moveBottom(bounds.bottom());
        if (place.top() < bounds.top())
            place.moveTop(bounds.top());
    }
    if (outX(place) && (constraints & 16))
        place = place.intersected(QRect(bounds.left(), place.top(), bounds.width(), place.height()));
    if (outY(place) && (constraints & 32))
        place = place.intersected(QRect(place.left(), bounds.top(), place.width(), bounds.height()));
    return place;
}

// ---- xdg_wm_base

XdgShell::XdgShell(Server *server)
    : QObject(server), xdg_wm_base(server->compositor()->display(), 5), m_server(server)
{
}

void XdgShell::xdg_wm_base_destroy(Resource *resource) { wl_resource_destroy(resource->handle); }

void XdgShell::xdg_wm_base_create_positioner(Resource *resource, uint32_t id)
{
    new XdgPositioner(resource->client(), id, resource->version());
}

void XdgShell::xdg_wm_base_get_xdg_surface(Resource *resource, uint32_t id, struct ::wl_resource *surface)
{
    QWaylandSurface *s = QWaylandSurface::fromResource(surface);
    if (!s)
        return;
    new XdgSurface(this, s, resource->client(), id, resource->version());
}

XdgPositioner *XdgPositioner::from(wl_resource *r)
{
    auto *res = Resource::fromResource(r);
    return res ? static_cast<XdgPositioner *>(res->xdg_positioner_object) : nullptr;
}

// ---- xdg_surface

XdgSurface::XdgSurface(XdgShell *shell, QWaylandSurface *surface, wl_client *client, uint32_t id, int version)
    : xdg_surface(client, id, version), m_shell(shell), m_surface(surface)
{
    connect(surface, &QWaylandSurface::redraw, this, &XdgSurface::handleCommit);
    connect(surface, &QWaylandSurface::surfaceDestroyed, this, [this] { m_surface = nullptr; });
}

XdgSurface::~XdgSurface() { emit surfaceGone(this); }

XdgSurface *XdgSurface::from(wl_resource *r)
{
    auto *res = r ? Resource::fromResource(r) : nullptr;
    return res ? static_cast<XdgSurface *>(res->xdg_surface_object) : nullptr;
}

QRect XdgSurface::windowGeometry() const
{
    if (m_geometry.isValid() && !m_geometry.isEmpty())
        return m_geometry;
    return QRect(QPoint(0, 0), m_surface ? m_surface->destinationSize() : QSize());
}

void XdgSurface::xdg_surface_destroy_resource(Resource *)
{
    delete m_toplevel;
    delete m_popup;
    delete this;
}

void XdgSurface::xdg_surface_get_toplevel(Resource *resource, uint32_t id)
{
    if (!m_surface || !m_surface->setRole(&toplevelRole, resource->handle, XDG_WM_BASE_ERROR_ROLE))
        return;
    m_toplevel = new XdgToplevel(this, resource->client(), id, resource->version());
    emit m_shell->toplevelCreated(m_toplevel);
}

void XdgSurface::xdg_surface_get_popup(Resource *resource, uint32_t id, struct ::wl_resource *parent, struct ::wl_resource *positioner)
{
    XdgPositioner *p = XdgPositioner::from(positioner);
    if (!m_surface || !p || !m_surface->setRole(&popupRole, resource->handle, XDG_WM_BASE_ERROR_ROLE))
        return;
    // a null parent: a layer surface's, given by zwlr_layer_surface_v1.get_popup next
    m_popup = new XdgPopup(this, XdgSurface::from(parent), p->state, resource->client(), id, resource->version());
    emit m_shell->popupCreated(m_popup);
}

void XdgSurface::xdg_surface_set_window_geometry(Resource *, int32_t x, int32_t y, int32_t w, int32_t h)
{
    m_pendingGeometry = QRect(x, y, w, h);
    m_hasPendingGeometry = true;
}

void XdgSurface::xdg_surface_ack_configure(Resource *, uint32_t serial)
{
    if (serial <= m_lastSerial)
        m_configured = true;
}

uint32_t XdgSurface::sendConfigure()
{
    if (m_toplevel)
        m_toplevel->sendState();
    else if (m_popup)
        m_popup->send_configure(m_popup->m_geometry.x(), m_popup->m_geometry.y(), m_popup->m_geometry.width(),
                                m_popup->m_geometry.height());
    m_lastSerial = m_shell->server()->compositor()->nextSerial();
    send_configure(m_lastSerial);
    m_initialSent = true;
    return m_lastSerial;
}

void XdgSurface::handleCommit()
{
    if (m_hasPendingGeometry) {
        m_hasPendingGeometry = false;
        if (m_pendingGeometry != m_geometry) {
            m_geometry = m_pendingGeometry;
            emit windowGeometryChanged();
        }
    }
    // the first commit, before any buffer: the role's first configure (a popup's once it has its
    // parent, a layer surface's after get_popup)
    if (!m_initialSent && (m_toplevel || (m_popup && (m_popup->parentXdgSurface() || m_popup->parentLayer())))) {
        if (m_popup)
            emit m_popup->repositioned();   // its place first
        sendConfigure();
    }
    emit committed();
}

// ---- xdg_toplevel

XdgToplevel::XdgToplevel(XdgSurface *surface, wl_client *client, uint32_t id, int version)
    : xdg_toplevel(client, id, version), m_surface(surface)
{
    if (version >= 5) {   // what the window menu-less server offers: maximize (zoom), full screen, minimize
        const uint32_t caps[] = {2, 3, 4};
        send_wm_capabilities(QByteArray(reinterpret_cast<const char *>(caps), sizeof caps));
    }
}

XdgToplevel::~XdgToplevel() = default;

XdgToplevel *XdgToplevel::from(wl_resource *r)
{
    auto *res = r ? Resource::fromResource(r) : nullptr;
    return res ? static_cast<XdgToplevel *>(res->xdg_toplevel_object) : nullptr;
}

void XdgToplevel::xdg_toplevel_destroy_resource(Resource *)
{
    m_surface->m_toplevel = nullptr;
    deleteLater();
}

void XdgToplevel::xdg_toplevel_set_parent(Resource *, struct ::wl_resource *parent)
{
    m_parent = XdgToplevel::from(parent);
    emit parentChanged();
}

void XdgToplevel::xdg_toplevel_set_title(Resource *, const QString &title)
{
    if (title == m_title)
        return;
    m_title = title;
    emit titleChanged();
}

void XdgToplevel::xdg_toplevel_set_app_id(Resource *, const QString &appId)
{
    if (appId == m_appId)
        return;
    m_appId = appId;
    emit appIdChanged();
}

void XdgToplevel::xdg_toplevel_resize(Resource *, struct ::wl_resource *, uint32_t, uint32_t edges)
{
    // xdg_toplevel.resize_edge: 1 top, 2 bottom, 4 left, 8 right (bits)
    Qt::Edges e;
    if (edges & 1)
        e |= Qt::TopEdge;
    if (edges & 2)
        e |= Qt::BottomEdge;
    if (edges & 4)
        e |= Qt::LeftEdge;
    if (edges & 8)
        e |= Qt::RightEdge;
    emit resizeRequested(e);
}

bool XdgToplevel::serverSideDecoration() const { return decoration && decoration->mode() == 2; }

void XdgToplevel::setConfigure(const QSize &size, bool activated, bool maximized, bool fullscreen, bool resizing)
{
    if (size == m_size && activated == m_activated && maximized == m_maximized && fullscreen == m_fullscreen && resizing == m_resizing &&
        m_surface->m_initialSent)
        return;
    m_size = size;
    m_activated = activated;
    m_maximized = maximized;
    m_fullscreen = fullscreen;
    m_resizing = resizing;
    m_surface->sendConfigure();
}

void XdgToplevel::sendState()
{
    QList<uint32_t> states;
    if (m_maximized)
        states << Maximized;
    if (m_fullscreen)
        states << Fullscreen;
    if (m_resizing)
        states << Resizing;
    if (m_activated)
        states << Activated;
    if (decoration)
        decoration->sendMode();
    send_configure(qMax(0, m_size.width()), qMax(0, m_size.height()),
                   QByteArray(reinterpret_cast<const char *>(states.constData()), int(states.size() * sizeof(uint32_t))));
}

// ---- xdg_popup

XdgPopup::XdgPopup(XdgSurface *surface, XdgSurface *parent, const PositionerState &positioner, wl_client *client, uint32_t id, int version)
    : xdg_popup(client, id, version), m_surface(surface), m_parent(parent), m_positioner(positioner)
{
}

XdgPopup::~XdgPopup() = default;

void XdgPopup::setParentLayer(LayerSurface *layer)
{
    m_parentLayer = layer;
    emit parentAttached();
}

void XdgPopup::xdg_popup_destroy_resource(Resource *)
{
    m_surface->m_popup = nullptr;
    deleteLater();
}

void XdgPopup::xdg_popup_grab(Resource *, struct ::wl_resource *, uint32_t)
{
    m_grab = true;
    emit grabRequested();
}

void XdgPopup::xdg_popup_reposition(Resource *, struct ::wl_resource *positioner, uint32_t token)
{
    if (XdgPositioner *p = XdgPositioner::from(positioner))
        m_positioner = p->state;
    emit repositioned();
    send_repositioned(token);
    m_surface->sendConfigure();
}

// ---- xdg-decoration

XdgDecorationManager::XdgDecorationManager(wl_display *display) : zxdg_decoration_manager_v1(display, 1) {}

void XdgDecorationManager::zxdg_decoration_manager_v1_get_toplevel_decoration(Resource *resource, uint32_t id, struct ::wl_resource *toplevel)
{
    XdgToplevel *t = XdgToplevel::from(toplevel);
    if (!t)
        return;
    if (t->decoration) {
        wl_resource_post_error(resource->handle, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED, "already has a decoration");
        return;
    }
    new Decoration(t, resource->client(), id, resource->version());
}

Decoration::Decoration(XdgToplevel *toplevel, wl_client *client, uint32_t id, int version)
    : zxdg_toplevel_decoration_v1(client, id, version), m_toplevel(toplevel)
{
    toplevel->decoration = this;
    emit toplevel->decorationChanged();
}

Decoration::~Decoration()
{
    if (m_toplevel) {
        m_toplevel->decoration = nullptr;
        emit m_toplevel->decorationChanged();
    }
}

void Decoration::zxdg_toplevel_decoration_v1_set_mode(Resource *, uint32_t mode)
{
    // an application that draws its own title bar keeps it (Chromium's, Electron's frameless windows)
    m_mode = mode == 1 ? 1 : 2;
    if (m_toplevel) {
        emit m_toplevel->decorationChanged();
        if (m_toplevel->xdgSurface()->initialConfigureSent())
            m_toplevel->xdgSurface()->sendConfigure();
    }
}

void Decoration::zxdg_toplevel_decoration_v1_unset_mode(Resource *)
{
    m_mode = 2;
    if (m_toplevel) {
        emit m_toplevel->decorationChanged();
        if (m_toplevel->xdgSurface()->initialConfigureSent())
            m_toplevel->xdgSurface()->sendConfigure();
    }
}
