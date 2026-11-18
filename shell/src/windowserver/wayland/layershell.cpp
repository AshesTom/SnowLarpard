// SPDX-License-Identifier: LGPL-2.1-or-later
#include "layershell.h"

#include "server.h"
#include "xdgshell.h"

#include <QWaylandCompositor>
#include <QWaylandQuickItem>
#include <QWaylandSurface>

namespace {
QWaylandSurfaceRole layerRole("zwlr_layer_surface_v1");
}

LayerShell::LayerShell(Server *server) : QObject(server), zwlr_layer_shell_v1(server->compositor()->display(), 5), m_server(server) {}

void LayerShell::zwlr_layer_shell_v1_get_layer_surface(Resource *resource, uint32_t id, struct ::wl_resource *surface, struct ::wl_resource *,
                                                       uint32_t layer, const QString &nameSpace)
{
    QWaylandSurface *s = QWaylandSurface::fromResource(surface);
    if (!s)
        return;
    if (layer > LayerSurface::Overlay) {
        wl_resource_post_error(resource->handle, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER, "invalid layer");
        return;
    }
    if (!s->setRole(&layerRole, resource->handle, ZWLR_LAYER_SHELL_V1_ERROR_ROLE))
        return;
    new LayerSurface(m_server, s, layer, nameSpace, resource->client(), id, resource->version());
}

LayerSurface::LayerSurface(Server *server, QWaylandSurface *surface, uint32_t layer, const QString &nameSpace, wl_client *client,
                           uint32_t id, int version)
    : zwlr_layer_surface_v1(client, id, version), m_server(server), m_surface(surface), m_namespace(nameSpace)
{
    m_pending.layer = layer;
    connect(surface, &QWaylandSurface::redraw, this, &LayerSurface::handleCommit);
    connect(surface, &QWaylandSurface::surfaceDestroyed, this, [this] {
        m_surface = nullptr;
        if (m_mapped) {
            m_mapped = false;
            emit mappedChanged();
        }
    });
    m_server->addLayerSurface(this);
}

LayerSurface::~LayerSurface()
{
    m_server->removeLayerSurface(this);
    delete m_item;
}

void LayerSurface::zwlr_layer_surface_v1_destroy_resource(Resource *) { delete this; }

void LayerSurface::zwlr_layer_surface_v1_get_popup(Resource *, struct ::wl_resource *popup)
{
    XdgSurface *xs = nullptr;
    // the popup's xdg_surface: its wl_surface's
    if (auto *res = SLWaylandServer::xdg_popup::Resource::fromResource(popup))
        if (auto *p = static_cast<XdgPopup *>(res->xdg_popup_object))
            xs = p->xdgSurface();
    if (xs && xs->popup())
        xs->popup()->setParentLayer(this);
}

uint32_t LayerSurface::exclusiveEdge() const
{
    if (m_state.exclusiveZone <= 0)
        return 0;
    if (m_state.exclusiveEdge)
        return m_state.exclusiveEdge;
    // anchored to one edge, or to one edge and both its neighbours
    const uint32_t a = m_state.anchor;
    for (uint32_t edge : {uint32_t(AnchorTop), uint32_t(AnchorBottom), uint32_t(AnchorLeft), uint32_t(AnchorRight)}) {
        const uint32_t across = (edge == AnchorTop || edge == AnchorBottom) ? (AnchorLeft | AnchorRight) : (AnchorTop | AnchorBottom);
        if (a == edge || a == (edge | across))
            return edge;
    }
    return 0;
}

void LayerSurface::takeZone(const QRect &screen, QRect &usable) const
{
    const State &s = m_state;
    const QRect bounds = s.exclusiveZone == -1 ? screen : usable;
    const int zone = s.exclusiveZone;
    switch (exclusiveEdge()) {
    case AnchorTop: usable.setTop(qMax(usable.top(), bounds.top() + s.margins.top() + zone)); break;
    case AnchorBottom: usable.setBottom(qMin(usable.bottom(), bounds.bottom() - s.margins.bottom() - zone)); break;
    case AnchorLeft: usable.setLeft(qMax(usable.left(), bounds.left() + s.margins.left() + zone)); break;
    case AnchorRight: usable.setRight(qMin(usable.right(), bounds.right() - s.margins.right() - zone)); break;
    default: break;
    }
}

void LayerSurface::arrange(const QRect &screen, QRect &usable)
{
    const State &s = m_state;
    const QRect bounds = s.exclusiveZone == -1 ? screen : usable;
    QSize size = s.size;
    const bool both_h = (s.anchor & AnchorLeft) && (s.anchor & AnchorRight);
    const bool both_v = (s.anchor & AnchorTop) && (s.anchor & AnchorBottom);
    if (size.width() == 0 || both_h)
        size.setWidth(size.width() == 0 ? bounds.width() - s.margins.left() - s.margins.right() : size.width());
    if (size.height() == 0 || both_v)
        size.setHeight(size.height() == 0 ? bounds.height() - s.margins.top() - s.margins.bottom() : size.height());
    int x, y;
    if (both_h || !(s.anchor & (AnchorLeft | AnchorRight)))
        x = bounds.x() + (bounds.width() - size.width()) / 2;
    else if (s.anchor & AnchorLeft)
        x = bounds.x() + s.margins.left();
    else
        x = bounds.x() + bounds.width() - size.width() - s.margins.right();
    if (both_v || !(s.anchor & (AnchorTop | AnchorBottom)))
        y = bounds.y() + (bounds.height() - size.height()) / 2;
    else if (s.anchor & AnchorTop)
        y = bounds.y() + s.margins.top();
    else
        y = bounds.y() + bounds.height() - size.height() - s.margins.bottom();
    m_geometry = QRect(x, y, size.width(), size.height());

    takeZone(screen, usable);

    if (m_item)
        m_item->setPosition(m_geometry.topLeft());
    if (m_geometry.size() != m_sentSize || !m_initialSent) {
        m_sentSize = m_geometry.size();
        m_initialSent = true;
        send_configure(m_server->compositor()->nextSerial(), uint32_t(qMax(0, m_sentSize.width())), uint32_t(qMax(0, m_sentSize.height())));
    }
}

void LayerSurface::handleCommit()
{
    if (!m_surface)
        return;
    const bool layerChanged = m_pending.layer != m_state.layer;
    const bool changed = layerChanged || m_pending.anchor != m_state.anchor || m_pending.size != m_state.size ||
                         m_pending.exclusiveZone != m_state.exclusiveZone || m_pending.margins != m_state.margins ||
                         m_pending.keyboard != m_state.keyboard || m_pending.exclusiveEdge != m_state.exclusiveEdge || !m_initialSent;
    m_state = m_pending;
    if (!m_item) {
        m_item = m_server->createSurfaceItem(m_surface, m_server->layerItem(m_state.layer));
        m_item->setVisible(false);
    } else if (layerChanged) {
        m_item->setParentItem(m_server->layerItem(m_state.layer));
    }
    m_item->setFocusOnClick(m_state.keyboard != 0);
    if (changed) {
        m_server->arrangeLayers();
        emit stateChanged();
    }
    const bool mapped = m_surface->hasContent();
    if (mapped != m_mapped) {
        m_mapped = mapped;
        m_item->setVisible(mapped);
        m_server->arrangeLayers();
        emit mappedChanged();
    }
}
