// SPDX-License-Identifier: LGPL-2.1-or-later
// wlr-layer-shell: the shell's own layers, as the Mac's window server has its levels for them: the
// Finder's desktop (background), the Dock and the menu bar (top, their exclusive zones the work
// area's edges), the screen shots' selection (overlay). Each is known by its namespace
// ("com.apple.dock", "com.apple.systemuiserver", "com.apple.finder.desktop"...).
#pragma once

#include "qwayland-server-wlr-layer-shell-unstable-v1.h"

#include <QMargins>
#include <QObject>
#include <QWaylandQuickItem>
#include <QPointer>
#include <QPolygon>
#include <QRect>

class QWaylandSurface;
class QWaylandQuickItem;
class Server;

class LayerShell : public QObject, public SLWaylandServer::zwlr_layer_shell_v1 {
    Q_OBJECT
public:
    explicit LayerShell(Server *server);

protected:
    void zwlr_layer_shell_v1_get_layer_surface(Resource *resource, uint32_t id, struct ::wl_resource *surface, struct ::wl_resource *output,
                                               uint32_t layer, const QString &nameSpace) override;
    void zwlr_layer_shell_v1_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }

private:
    Server *m_server;
};

class LayerSurface : public QObject, public SLWaylandServer::zwlr_layer_surface_v1 {
    Q_OBJECT
public:
    enum Layer { Background, Bottom, Top, Overlay };
    enum Anchor { AnchorTop = 1, AnchorBottom = 2, AnchorLeft = 4, AnchorRight = 8 };
    struct State {
        QSize size;
        uint32_t anchor = 0;
        int exclusiveZone = 0;
        QMargins margins;
        uint32_t keyboard = 0;   // none, exclusive, on demand
        uint32_t layer = Top;
        uint32_t exclusiveEdge = 0;
    };

    LayerSurface(Server *server, QWaylandSurface *surface, uint32_t layer, const QString &nameSpace, wl_client *client, uint32_t id, int version);
    ~LayerSurface() override;

    QWaylandSurface *surface() const { return m_surface; }
    QWaylandQuickItem *item() const { return m_item; }
    QString nameSpace() const { return m_namespace; }
    const State &state() const { return m_state; }
    bool mapped() const { return m_mapped; }
    QRect geometry() const { return m_geometry; }
    // the edge its exclusive zone keeps (an anchor bit), 0 for none
    uint32_t exclusiveEdge() const;
    // placed by the server (Server::arrangeLayers): within `usable`, the screen less the zones
    // of the layers before it; configured again when its size changes
    void arrange(const QRect &screen, QRect &usable);
    // its exclusive zone taken off `usable`
    void takeZone(const QRect &screen, QRect &usable) const;

    // CGSSetMagicMirror (sl_window_server_v1.set_magic_mirror): its direction and quadrangle
    int mirror = 0;
    QPolygon mirrorQuad;

signals:
    void mappedChanged();
    void stateChanged();

protected:
    void zwlr_layer_surface_v1_destroy_resource(Resource *) override;
    void zwlr_layer_surface_v1_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void zwlr_layer_surface_v1_set_size(Resource *, uint32_t w, uint32_t h) override { m_pending.size = QSize(int(w), int(h)); }
    void zwlr_layer_surface_v1_set_anchor(Resource *, uint32_t anchor) override { m_pending.anchor = anchor; }
    void zwlr_layer_surface_v1_set_exclusive_zone(Resource *, int32_t zone) override { m_pending.exclusiveZone = zone; }
    void zwlr_layer_surface_v1_set_margin(Resource *, int32_t t, int32_t r, int32_t b, int32_t l) override { m_pending.margins = QMargins(l, t, r, b); }
    void zwlr_layer_surface_v1_set_keyboard_interactivity(Resource *, uint32_t k) override { m_pending.keyboard = k; }
    void zwlr_layer_surface_v1_get_popup(Resource *, struct ::wl_resource *popup) override;
    void zwlr_layer_surface_v1_ack_configure(Resource *, uint32_t) override { m_configured = true; }
    void zwlr_layer_surface_v1_set_layer(Resource *, uint32_t layer) override { m_pending.layer = layer; }
    void zwlr_layer_surface_v1_set_exclusive_edge(Resource *, uint32_t edge) override { m_pending.exclusiveEdge = edge; }

private:
    void handleCommit();
    Server *m_server;
    QWaylandSurface *m_surface;
    QPointer<QWaylandQuickItem> m_item;
    QString m_namespace;
    State m_pending, m_state;
    QRect m_geometry;
    QSize m_sentSize{-1, -1};
    bool m_configured = false, m_initialSent = false, m_mapped = false;
};
