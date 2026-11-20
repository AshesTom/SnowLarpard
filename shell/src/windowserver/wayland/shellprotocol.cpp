// SPDX-License-Identifier: LGPL-2.1-or-later
#include "shellprotocol.h"

#include "layershell.h"
#include "server.h"
#include "window.h"
#include "xdgshell.h"
#include "effects.h"

#include <QFile>
#include <QPainter>
#include <QWaylandClient>
#include <QWaylandCompositor>
#include <QWaylandSurface>
#include <unistd.h>

namespace {

QList<uint32_t> uints(wl_array *a)
{
    QList<uint32_t> out;
    if (!a)
        return out;
    const auto *v = static_cast<const uint32_t *>(a->data);
    for (size_t i = 0; i < a->size / sizeof(uint32_t); ++i)
        out << v[i];
    return out;
}

QByteArray array(const QList<uint32_t> &v) { return QByteArray(reinterpret_cast<const char *>(v.constData()), int(v.size() * sizeof(uint32_t))); }

// A capture in progress: its PNG goes to the fd, then done (unless the client gave up)
class Capture : public QObject, public SLWaylandServer::sl_capture_v1 {
public:
    Capture(wl_client *client, uint32_t id, int version, int fd) : sl_capture_v1(client, id, version), m_fd(fd) {}
    ~Capture() override
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    void finish(const QImage &image, QPoint at)
    {
        bool ok = false;
        if (m_fd >= 0 && !image.isNull()) {
            QFile f;
            if (f.open(m_fd, QIODevice::WriteOnly, QFile::AutoCloseHandle)) {
                ok = image.save(&f, "PNG");
                f.close();
                m_fd = -1;
            }
        }
        if (m_alive)
            send_done(ok ? 1 : 0, at.x(), at.y());
    }
    void answer(uint32_t ok, int x, int y)
    {
        if (m_alive)
            send_done(ok, x, y);
    }

protected:
    void sl_capture_v1_destroy_resource(Resource *) override
    {
        m_alive = false;
        deleteLater();
    }
    void sl_capture_v1_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }

private:
    int m_fd;
    bool m_alive = true;
};

} // namespace

ShellProtocol::ShellProtocol(Server *server) : QObject(server), sl_window_server_v1(server->compositor()->display(), 1), m_server(server)
{
    m_flush.setSingleShot(true);
    m_flush.setInterval(0);
    connect(&m_flush, &QTimer::timeout, this, &ShellProtocol::flush);
    auto state = [this] {
        m_stateDirty = true;
        schedule();
    };
    connect(server, &Server::windowsChanged, this, state);
    connect(server, &Server::activeChanged, this, state);
    connect(server, &Server::workAreaChanged, this, state);
    connect(server, &Server::frontAppChanged, this, state);
}

void ShellProtocol::sendWindow(Resource *r, Window *w)
{
    const QRect f = w->frameGeometry();
    send_window(r->handle, w->id(), w->pid(), w->appId(), w->title(), w->flags(), f.x(), f.y(), f.width(), f.height(), w->menuService(),
                w->menuPath());
}

void ShellProtocol::sendState(Resource *r)
{
    QList<uint32_t> order;
    for (Window *w : m_server->windows())
        order << w->id();
    send_order(r->handle, array(order));
    send_active(r->handle, m_server->active() ? m_server->active()->id() : 0);
    send_front_app(r->handle, m_server->frontAppPid(), m_server->frontAppSerial());
    const QRect a = m_server->workArea(), s = m_server->screen();
    send_screen(r->handle, s.width(), s.height());
    send_work_area(r->handle, a.x(), a.y(), a.width(), a.height());
}

void ShellProtocol::sl_window_server_v1_bind_resource(Resource *r)
{
    for (Window *w : m_server->windows())
        sendWindow(r, w);
    sendState(r);
    send_done(r->handle);
}

void ShellProtocol::sl_window_server_v1_destroy_resource(Resource *r)
{
    m_hotKeys.remove(r);
    if (QWaylandClient *c = QWaylandClient::fromWlClient(m_server->compositor(), r->client())) {
        m_server->setKeyEquivalents(c, {});
        if (m_server->frontAppPid() && c->processId() == qint64(m_server->frontAppPid()))
            m_server->setFrontApp(c, 0);
    }
}

void ShellProtocol::windowChanged(Window *w)
{
    m_dirty.insert(w->id());
    schedule();
}

void ShellProtocol::windowRemoved(Window *w)
{
    m_dirty.remove(w->id());
    m_removed << w->id();
    m_stateDirty = true;
    schedule();
}

void ShellProtocol::flush()
{
    const auto all = resourceMap();
    for (Resource *r : all) {
        for (uint32_t id : std::as_const(m_removed))
            send_window_removed(r->handle, id);
        for (uint32_t id : std::as_const(m_dirty))
            if (Window *w = m_server->window(id))
                sendWindow(r, w);
        if (m_stateDirty)
            sendState(r);
        send_done(r->handle);
    }
    m_removed.clear();
    m_dirty.clear();
    m_stateDirty = false;
}

bool ShellProtocol::hotKey(int hotKey)
{
    for (auto it = m_hotKeys.begin(); it != m_hotKeys.end(); ++it)
        if (it.value().contains(uint32_t(hotKey))) {
            send_hot_key(it.key()->handle, uint32_t(hotKey));
            return true;
        }
    return false;
}

bool ShellProtocol::keyForFrontApp(uint32_t keysym, const QString &text, uint32_t modifiers)
{
    const uint32_t pid = m_server->frontAppPid();
    if (!pid)
        return false;
    bool sent = false;
    const auto all = resourceMap();
    for (Resource *r : all) {
        QWaylandClient *c = QWaylandClient::fromWlClient(m_server->compositor(), r->client());
        if (c && uint32_t(c->processId()) == pid) {
            send_key(r->handle, keysym, text, modifiers);
            sent = true;
        }
    }
    return sent;
}

void ShellProtocol::setMenu(QWaylandSurface *surface, const QString &service, const QString &path)
{
    m_menus[surface] = {service, path};
    for (Window *w : m_server->windows())
        if (w->surface() == surface && w->kind() == Window::Xdg)
            static_cast<XdgWindow *>(w)->setMenu(service, path);
}

void ShellProtocol::sl_window_server_v1_activate(Resource *, uint32_t window)
{
    if (Window *w = m_server->window(window))
        m_server->activate(w);
}

void ShellProtocol::sl_window_server_v1_close(Resource *, uint32_t window)
{
    if (Window *w = m_server->window(window))
        w->sendClose();
}

void ShellProtocol::sl_window_server_v1_minimize(Resource *, uint32_t window)
{
    if (Window *w = m_server->window(window))
        m_server->minimize(w);
}

void ShellProtocol::sl_window_server_v1_hide(Resource *, uint32_t window)
{
    if (Window *w = m_server->window(window))
        m_server->minimize(w, true);
}

void ShellProtocol::sl_window_server_v1_set_icon_geometry(Resource *, uint32_t window, int32_t x, int32_t y, int32_t w, int32_t h)
{
    Window *win = m_server->window(window);
    if (!win)
        return;
    const QRect r(x, y, w, h);
    if (r == win->iconGeometry())
        return;
    win->setIconGeometry(r);
    // inset by 4, as 10.6's Dock does
    if (w > 8 && h > 8)
        m_server->effects()->retarget(win, r.adjusted(4, 4, -4, -4));
}

void ShellProtocol::sl_window_server_v1_set_front_app(Resource *resource, uint32_t pid)
{
    if (QWaylandClient *c = QWaylandClient::fromWlClient(m_server->compositor(), resource->client()))
        m_server->setFrontApp(c, pid);
}

void ShellProtocol::sl_window_server_v1_register_hot_keys(Resource *resource, wl_array *hotKeys) { m_hotKeys[resource] = uints(hotKeys); }

void ShellProtocol::sl_window_server_v1_set_key_equivalents(Resource *resource, wl_array *keysyms)
{
    if (QWaylandClient *c = QWaylandClient::fromWlClient(m_server->compositor(), resource->client()))
        m_server->setKeyEquivalents(c, uints(keysyms));
}

void ShellProtocol::sl_window_server_v1_set_magic_mirror(Resource *, const QString &layer, uint32_t direction, wl_array *quad)
{
    LayerSurface *l = m_server->layerSurface(layer);
    if (!l)
        return;
    QPolygon q;
    if (quad) {
        const auto *v = static_cast<const int32_t *>(quad->data);
        for (size_t i = 0; i + 1 < quad->size / sizeof(int32_t); i += 2)
            q << QPoint(v[i], v[i + 1]);
    }
    l->mirror = q.size() == 4 ? int(direction) : 0;
    l->mirrorQuad = q;
    m_server->arrangeLayers();
}

void ShellProtocol::sl_window_server_v1_capture_window(Resource *resource, uint32_t id, uint32_t window, uint32_t shadow, int32_t fd)
{
    auto *capture = new Capture(resource->client(), id, resource->version(), fd);
    QPointer<Capture> guard(capture);
    Window *w = m_server->window(window);
    if (!w) {
        capture->finish({}, {});
        return;
    }
    QPointer<Window> win(w);
    auto finish = [guard, win, shadow](const QImage &frame) {
        if (!guard)
            return;
        if (!shadow || !win || frame.isNull()) {
            guard->finish(frame, {});
            return;
        }
        // over its shadow, the shadow's room around it (CGSHWCaptureWindowList with the shadow)
        QPoint origin;
        const QImage s = win->shadowImage(&origin);
        if (s.isNull()) {
            guard->finish(frame, {});
            return;
        }
        const QRect all = QRect(origin, s.size()).united(QRect(QPoint(0, 0), frame.size()));
        QImage out(all.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.drawImage(origin - all.topLeft(), s);
        p.drawImage(-all.topLeft(), frame);
        p.end();
        guard->finish(out, -all.topLeft());
    };
    if (w->minimized() && !w->snapshot().isNull())
        finish(w->snapshot());
    else
        m_server->grab(w, finish);
}

void ShellProtocol::sl_window_server_v1_capture_screen(Resource *resource, uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fd)
{
    auto *capture = new Capture(resource->client(), id, resource->version(), fd);
    QPointer<Capture> guard(capture);
    const QRect r(x, y, w, h);
    m_server->grabScreen([guard, r](const QImage &screen) {
        if (guard)
            guard->finish(r.isEmpty() ? screen : screen.copy(r), r.topLeft());
    });
}

void ShellProtocol::sl_window_server_v1_pick_window(Resource *resource, uint32_t id, int32_t x, int32_t y)
{
    auto *capture = new Capture(resource->client(), id, resource->version(), -1);
    Window *w = m_server->windowAt(QPointF(x, y));
    capture->answer(w ? 1 : 0, w ? int(w->id()) : 0, 0);
}

// ---- org_kde_kwin_appmenu

namespace {
class AppMenu : public SLWaylandServer::org_kde_kwin_appmenu {
public:
    AppMenu(ShellProtocol *shell, QWaylandSurface *surface, wl_client *client, uint32_t id, int version)
        : org_kde_kwin_appmenu(client, id, version), m_shell(shell), m_surface(surface)
    {
    }

protected:
    void org_kde_kwin_appmenu_set_address(Resource *, const QString &service, const QString &path) override
    {
        if (m_surface)
            m_shell->setMenu(m_surface, service, path);
    }
    void org_kde_kwin_appmenu_release(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void org_kde_kwin_appmenu_destroy_resource(Resource *) override { delete this; }

private:
    ShellProtocol *m_shell;
    QPointer<QWaylandSurface> m_surface;
};
} // namespace

AppMenuManager::AppMenuManager(Server *server, ShellProtocol *shell)
    : org_kde_kwin_appmenu_manager(server->compositor()->display(), 1), m_shell(shell)
{
}

void AppMenuManager::org_kde_kwin_appmenu_manager_create(Resource *resource, uint32_t id, struct ::wl_resource *surface)
{
    if (QWaylandSurface *s = QWaylandSurface::fromResource(surface))
        new AppMenu(m_shell, s, resource->client(), id, resource->version());
}
