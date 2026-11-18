// SPDX-License-Identifier: LGPL-2.1-or-later
#include "xwayland.h"

#include "server.h"

#include <QFile>
#include <QStandardPaths>
#include <QWaylandClient>
#include <QWaylandCompositor>
#include <QWaylandQuickItem>
#include <QWaylandSurface>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xcb/composite.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>

namespace {

int bindUnix(const QByteArray &path, bool abstract)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    socklen_t len;
    if (abstract) {
        addr.sun_path[0] = 0;
        memcpy(addr.sun_path + 1, path.constData(), size_t(path.size()));
        len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + size_t(path.size()));
    } else {
        unlink(path.constData());
        memcpy(addr.sun_path, path.constData(), size_t(path.size()) + 1);
        len = socklen_t(offsetof(sockaddr_un, sun_path) + size_t(path.size()) + 1);
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), len) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

QString propertyString(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
    QString out;
    if (auto *r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, prop, type, 0, 4096), nullptr)) {
        if (r->format == 8)
            out = QString::fromUtf8(static_cast<const char *>(xcb_get_property_value(r)), xcb_get_property_value_length(r));
        free(r);
    }
    return out;
}

QList<uint32_t> propertyCardinals(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
    QList<uint32_t> out;
    if (auto *r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, prop, type, 0, 4096), nullptr)) {
        if (r->format == 32) {
            const auto *v = static_cast<const uint32_t *>(xcb_get_property_value(r));
            for (int i = 0; i < xcb_get_property_value_length(r) / 4; ++i)
                out << v[i];
        }
        free(r);
    }
    return out;
}

} // namespace

// ---- an X11 window

XWindow::XWindow(Server *server, Xwayland *xwm, xcb_window_t window, QWaylandSurface *surface)
    : Window(server, surface), m_xwm(xwm), m_xid(window)
{
    createItems();
    readProperties();
}

Window *XWindow::parentWindow() const { return m_transientFor ? m_xwm->find(m_transientFor) : nullptr; }

void XWindow::readProperties()
{
    xcb_connection_t *c = m_xwm->conn();
    for (const char *name : {"_NET_WM_NAME", "WM_NAME", "WM_CLASS", "_NET_WM_PID", "WM_TRANSIENT_FOR", "_NET_WM_WINDOW_TYPE", "WM_PROTOCOLS",
                             "_MOTIF_WM_HINTS", "WM_NORMAL_HINTS", "WM_HINTS", "_NET_WM_STATE", "_KDE_NET_WM_APPMENU_SERVICE_NAME",
                             "_KDE_NET_WM_APPMENU_OBJECT_PATH"})
        readProperty(m_xwm->atom(name));
    Q_UNUSED(c)
}

void XWindow::readProperty(xcb_atom_t a)
{
    xcb_connection_t *c = m_xwm->conn();
    Xwayland *x = m_xwm;
    if (a == x->atom("_NET_WM_NAME") || a == XCB_ATOM_WM_NAME) {
        QString t = propertyString(c, m_xid, x->atom("_NET_WM_NAME"), x->atom("UTF8_STRING"));
        if (t.isEmpty())
            t = propertyString(c, m_xid, XCB_ATOM_WM_NAME, XCB_ATOM_STRING);
        if (t != m_title) {
            m_title = t;
            updateTitle();
            emit changed();
        }
    } else if (a == XCB_ATOM_WM_CLASS) {
        // its class part, as the X11 server's Dock reads it ("firefox")
        const QString v = propertyString(c, m_xid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
        const QStringList parts = v.split(QChar(0));
        m_class = parts.size() > 1 ? parts[1] : parts.value(0);
        emit changed();
    } else if (a == x->atom("_NET_WM_PID")) {
        m_pid = propertyCardinals(c, m_xid, a, XCB_ATOM_CARDINAL).value(0);
    } else if (a == XCB_ATOM_WM_TRANSIENT_FOR) {
        m_transientFor = propertyCardinals(c, m_xid, a, XCB_ATOM_WINDOW).value(0);
    } else if (a == x->atom("_NET_WM_WINDOW_TYPE")) {
        const QList<uint32_t> types = propertyCardinals(c, m_xid, a, XCB_ATOM_ATOM);
        m_utility = types.contains(x->atom("_NET_WM_WINDOW_TYPE_UTILITY")) || types.contains(x->atom("_NET_WM_WINDOW_TYPE_TOOLBAR"));
        if (types.contains(x->atom("_NET_WM_WINDOW_TYPE_SPLASH")))
            m_decorated = false;
        relayout();
    } else if (a == x->atom("WM_PROTOCOLS")) {
        const QList<uint32_t> p = propertyCardinals(c, m_xid, a, XCB_ATOM_ATOM);
        m_deleteWindow = p.contains(x->atom("WM_DELETE_WINDOW"));
        m_takeFocus = p.contains(x->atom("WM_TAKE_FOCUS"));
    } else if (a == x->atom("_MOTIF_WM_HINTS")) {
        // flags, functions, decorations: decorations 0 is an application drawing its own frame
        const QList<uint32_t> h = propertyCardinals(c, m_xid, a, a);
        const bool undecorated = h.size() >= 3 && (h[0] & 2) && h[2] == 0;
        if (undecorated == m_decorated) {
            m_decorated = !undecorated;
            relayout();
        }
    } else if (a == XCB_ATOM_WM_NORMAL_HINTS) {
        xcb_size_hints_t hints;
        if (xcb_icccm_get_wm_normal_hints_reply(c, xcb_icccm_get_wm_normal_hints(c, m_xid), &hints, nullptr) &&
            (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE))
            m_minSize = QSize(hints.min_width, hints.min_height);
    } else if (a == XCB_ATOM_WM_HINTS) {
        xcb_icccm_wm_hints_t hints;
        if (xcb_icccm_get_wm_hints_reply(c, xcb_icccm_get_wm_hints(c, m_xid), &hints, nullptr))
            m_acceptsInput = !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;
    } else if (a == x->atom("_NET_WM_STATE")) {
        const QList<uint32_t> s = propertyCardinals(c, m_xid, a, XCB_ATOM_ATOM);
        m_skipTaskbar = s.contains(x->atom("_NET_WM_STATE_SKIP_TASKBAR"));
        setAbove(s.contains(x->atom("_NET_WM_STATE_ABOVE")));
    } else if (a == x->atom("_KDE_NET_WM_APPMENU_SERVICE_NAME") || a == x->atom("_KDE_NET_WM_APPMENU_OBJECT_PATH")) {
        m_menuService = propertyString(c, m_xid, x->atom("_KDE_NET_WM_APPMENU_SERVICE_NAME"), XCB_ATOM_STRING);
        m_menuPath = propertyString(c, m_xid, x->atom("_KDE_NET_WM_APPMENU_OBJECT_PATH"), XCB_ATOM_STRING);
        emit changed();
    }
}

void XWindow::setGeometryFromX(const QRect &r) { m_xGeometry = r; }

void XWindow::sendConfigure()
{
    xcb_connection_t *c = m_xwm->conn();
    const uint32_t v[] = {uint32_t(m_xGeometry.x()), uint32_t(m_xGeometry.y()), uint32_t(m_xGeometry.width()), uint32_t(m_xGeometry.height()), 0};
    xcb_configure_window(c, m_xid,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_BORDER_WIDTH,
                         v);
    xcb_flush(c);
}

void XWindow::configure(const QSize &content)
{
    if (content.isEmpty())
        return;
    m_xGeometry.setSize(content);
    sendConfigure();
}

void XWindow::moved()
{
    // rootless: the X screen is the Wayland one, the X window where its content is
    m_xGeometry.moveTopLeft(contentGeometry().topLeft());
    sendConfigure();
}

void XWindow::sendClose()
{
    xcb_connection_t *c = m_xwm->conn();
    if (m_deleteWindow) {
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = m_xid;
        ev.type = m_xwm->atom("WM_PROTOCOLS");
        ev.data.data32[0] = m_xwm->atom("WM_DELETE_WINDOW");
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(c, 0, m_xid, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
    } else {
        xcb_kill_client(c, m_xid);
    }
    xcb_flush(c);
}

void XWindow::activated(bool active)
{
    xcb_connection_t *c = m_xwm->conn();
    if (active) {
        if (m_acceptsInput)
            xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, m_xid, XCB_CURRENT_TIME);
        if (m_takeFocus) {
            xcb_client_message_event_t ev = {};
            ev.response_type = XCB_CLIENT_MESSAGE;
            ev.format = 32;
            ev.window = m_xid;
            ev.type = m_xwm->atom("WM_PROTOCOLS");
            ev.data.data32[0] = m_xwm->atom("WM_TAKE_FOCUS");
            ev.data.data32[1] = XCB_CURRENT_TIME;
            xcb_send_event(c, 0, m_xid, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
        }
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(c, m_xid, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        m_xwm->setActive(m_xid);
    }
    xcb_flush(c);
}

// ---- Xwayland

Xwayland::Xwayland(Server *server) : QObject(server), m_server(server)
{
    connect(server->compositor(), &QWaylandCompositor::surfaceCreated, this, &Xwayland::surfaceCreated);
    connect(server, &Server::activeChanged, this, [this] {
        if (m_conn && !(m_server->active() && m_server->active()->kind() == Window::X11))
            setActive(0);
    });
}

Xwayland::~Xwayland() { cleanup(); }

void Xwayland::cleanup()
{
    if (m_process) {
        m_process->terminate();
        m_process->waitForFinished(1000);
        delete m_process;
        m_process = nullptr;
    }
    if (m_conn)
        xcb_disconnect(m_conn);
    m_conn = nullptr;
    for (int &fd : m_listen)
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    if (m_display >= 0) {
        unlink(QStringLiteral("/tmp/.X11-unix/X%1").arg(m_display).toLocal8Bit().constData());
        unlink(QStringLiteral("/tmp/.X%1-lock").arg(m_display).toLocal8Bit().constData());
    }
}

bool Xwayland::listen()
{
    mkdir("/tmp/.X11-unix", 01777);
    for (int n = 1; n < 64; ++n) {
        const QByteArray lock = QStringLiteral("/tmp/.X%1-lock").arg(n).toLocal8Bit();
        const int fd = open(lock.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
        if (fd < 0)
            continue;   // taken (an X server, another session's)
        const QByteArray pid = QByteArray::number(qint64(getpid())).rightJustified(10) + '\n';
        if (write(fd, pid.constData(), size_t(pid.size())) != pid.size()) {
            close(fd);
            unlink(lock.constData());
            continue;
        }
        close(fd);
        const QByteArray path = QStringLiteral("/tmp/.X11-unix/X%1").arg(n).toLocal8Bit();
        m_listen[0] = bindUnix(path, false);
        m_listen[1] = bindUnix(path, true);
        if (m_listen[0] < 0) {
            for (int &l : m_listen)
                if (l >= 0) {
                    close(l);
                    l = -1;
                }
            unlink(lock.constData());
            continue;
        }
        m_display = n;
        break;
    }
    if (m_display < 0)
        return false;
    // the first client starts Xwayland
    for (int i = 0; i < 2; ++i)
        if (m_listen[i] >= 0) {
            m_listenNotifier[i] = new QSocketNotifier(m_listen[i], QSocketNotifier::Read, this);
            connect(m_listenNotifier[i], &QSocketNotifier::activated, this, &Xwayland::start);
        }
    return true;
}

void Xwayland::start()
{
    if (m_process)
        return;
    for (QSocketNotifier *n : m_listenNotifier)
        if (n)
            n->setEnabled(false);
    int wl[2], wm[2], ready[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl) < 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0 ||
        pipe2(ready, O_CLOEXEC) < 0)
        return;
    // Xwayland as a client of this server, by the socket's other end
    wl_client *client = wl_client_create(m_server->compositor()->display(), wl[0]);
    m_client = QWaylandClient::fromWlClient(m_server->compositor(), client);
    m_wmFd = wm[0];
    const int childWl = wl[1], childWm = wm[1], childReady = ready[1], l0 = m_listen[0], l1 = m_listen[1];

    m_process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("WAYLAND_SOCKET", QString::number(childWl));
    env.remove("DISPLAY");
    m_process->setProcessEnvironment(env);
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    m_process->setChildProcessModifier([childWl, childWm, childReady, l0, l1] {
        for (int fd : {childWl, childWm, childReady, l0, l1})
            if (fd >= 0)
                fcntl(fd, F_SETFD, 0);   // inherited, the others not
    });
    QStringList args = {displayName(), "-rootless", "-core", "-nolisten", "tcp", "-wm", QString::number(childWm), "-displayfd",
                        QString::number(childReady), "-listenfd", QString::number(l0)};
    if (l1 >= 0)
        args << "-listenfd" << QString::number(l1);
    connect(m_process, &QProcess::finished, this, [this] {
        fprintf(stderr, "WindowServer-wayland: Xwayland ended\n");
    });
    m_process->start(QStringLiteral("Xwayland"), args);
    close(childWl);
    close(childWm);
    close(childReady);
    // ready when it writes its display's number
    auto *n = new QSocketNotifier(ready[0], QSocketNotifier::Read, this);
    connect(n, &QSocketNotifier::activated, this, [this, n, fd = ready[0]] {
        char buf[16];
        (void)read(fd, buf, sizeof buf);
        n->deleteLater();
        close(fd);
        startWm();
    });
}

xcb_atom_t Xwayland::atom(const char *name)
{
    auto it = m_atoms.constFind(name);
    if (it != m_atoms.constEnd())
        return it.value();
    xcb_atom_t a = 0;
    if (m_conn)
        if (auto *r = xcb_intern_atom_reply(m_conn, xcb_intern_atom(m_conn, 0, uint16_t(strlen(name)), name), nullptr)) {
            a = r->atom;
            free(r);
        }
    m_atoms.insert(name, a);
    return a;
}

XWindow *Xwayland::find(xcb_window_t w) const { return m_windows.value(w); }

void Xwayland::startWm()
{
    m_conn = xcb_connect_to_fd(m_wmFd, nullptr);
    if (xcb_connection_has_error(m_conn)) {
        fprintf(stderr, "WindowServer-wayland: no connection to Xwayland's window manager socket\n");
        xcb_disconnect(m_conn);
        m_conn = nullptr;
        return;
    }
    m_root = xcb_setup_roots_iterator(xcb_get_setup(m_conn)).data->root;
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(m_conn, m_root, XCB_CW_EVENT_MASK, &mask);
    xcb_composite_redirect_subwindows(m_conn, m_root, XCB_COMPOSITE_REDIRECT_MANUAL);
    // EWMH: a window manager is there
    m_check = xcb_generate_id(m_conn);
    xcb_create_window(m_conn, XCB_COPY_FROM_PARENT, m_check, m_root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0,
                      nullptr);
    const char name[] = "WindowServer";
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_check, atom("_NET_WM_NAME"), atom("UTF8_STRING"), 8, sizeof name - 1, name);
    for (xcb_window_t w : {m_root, m_check})
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, w, atom("_NET_SUPPORTING_WM_CHECK"), XCB_ATOM_WINDOW, 32, 1, &m_check);
    QList<xcb_atom_t> supported;
    for (const char *a : {"_NET_SUPPORTED", "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING", "_NET_ACTIVE_WINDOW", "_NET_WM_NAME", "_NET_WM_STATE",
                          "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_HIDDEN",
                          "_NET_WM_WINDOW_TYPE", "_NET_WM_PID", "_NET_WM_MOVERESIZE", "_NET_CLOSE_WINDOW"})
        supported << atom(a);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_SUPPORTED"), XCB_ATOM_ATOM, 32, uint32_t(supported.size()),
                        supported.constData());
    // the theme's arrow over the root (the session's XCURSOR_THEME)
    xcb_cursor_context_t *ctx = nullptr;
    if (xcb_cursor_context_new(m_conn, xcb_setup_roots_iterator(xcb_get_setup(m_conn)).data, &ctx) >= 0) {
        const xcb_cursor_t cursor = xcb_cursor_load_cursor(ctx, "left_ptr");
        xcb_change_window_attributes(m_conn, m_root, XCB_CW_CURSOR, &cursor);
        xcb_cursor_context_free(ctx);
    }
    xcb_set_selection_owner(m_conn, m_check, atom("WM_S0"), XCB_CURRENT_TIME);
    xcb_flush(m_conn);
    m_notifier = new QSocketNotifier(xcb_get_file_descriptor(m_conn), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Xwayland::readEvents);
    readEvents();
}

void Xwayland::readEvents()
{
    if (!m_conn)
        return;
    while (xcb_generic_event_t *e = xcb_poll_for_event(m_conn)) {
        handleEvent(e);
        free(e);
    }
    if (xcb_connection_has_error(m_conn)) {
        delete m_notifier;
        m_notifier = nullptr;
        for (XWindow *w : m_windows)
            m_server->removeWindow(w);
        m_windows.clear();
        m_known.clear();
        xcb_disconnect(m_conn);
        m_conn = nullptr;
        return;
    }
    xcb_flush(m_conn);
}

void Xwayland::handleEvent(xcb_generic_event_t *e)
{
    switch (e->response_type & 0x7f) {
    case XCB_CREATE_NOTIFY: {
        auto *ev = reinterpret_cast<xcb_create_notify_event_t *>(e);
        if (ev->parent != m_root || ev->window == m_check)
            break;
        Unmanaged u;
        u.window = ev->window;
        u.overrideRedirect = ev->override_redirect;
        u.geometry = QRect(ev->x, ev->y, ev->width, ev->height);
        m_known.insert(ev->window, u);
        const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE;
        xcb_change_window_attributes(m_conn, ev->window, XCB_CW_EVENT_MASK, &mask);
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        auto *ev = reinterpret_cast<xcb_destroy_notify_event_t *>(e);
        unmap(ev->window);
        m_known.remove(ev->window);
        break;
    }
    case XCB_MAP_REQUEST: {
        auto *ev = reinterpret_cast<xcb_map_request_event_t *>(e);
        // its state, then mapped: Xwayland gives it a surface (WL_SURFACE_ID)
        const uint32_t state[2] = {1, XCB_NONE};   // NormalState
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, ev->window, atom("WM_STATE"), atom("WM_STATE"), 32, 2, state);
        xcb_map_window(m_conn, ev->window);
        break;
    }
    case XCB_MAP_NOTIFY: {
        auto *ev = reinterpret_cast<xcb_map_notify_event_t *>(e);
        auto it = m_known.find(ev->window);
        if (it != m_known.end()) {
            it->mapped = true;
            it->overrideRedirect = ev->override_redirect;
            tryAssociate(ev->window);
        }
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        auto *ev = reinterpret_cast<xcb_unmap_notify_event_t *>(e);
        unmap(ev->window);
        auto it = m_known.find(ev->window);
        if (it != m_known.end()) {
            it->mapped = false;
            it->surfaceId = 0;
        }
        break;
    }
    case XCB_CONFIGURE_REQUEST: {
        auto *ev = reinterpret_cast<xcb_configure_request_event_t *>(e);
        XWindow *w = find(ev->window);
        QRect g = w ? w->xGeometry() : m_known.value(ev->window).geometry;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X)
            g.moveLeft(ev->x);
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y)
            g.moveTop(ev->y);
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            g.setWidth(ev->width);
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            g.setHeight(ev->height);
        if (w) {
            // its size as it asks; its place the server's (the content under the title bar)
            w->setGeometryFromX(QRect(w->contentGeometry().topLeft(), g.size()));
            w->configure(g.size());
        } else {
            m_known[ev->window].geometry = g;
            const uint32_t v[] = {uint32_t(g.x()), uint32_t(g.y()), uint32_t(g.width()), uint32_t(g.height())};
            xcb_configure_window(m_conn, ev->window,
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
        }
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        auto *ev = reinterpret_cast<xcb_configure_notify_event_t *>(e);
        auto it = m_known.find(ev->window);
        if (it != m_known.end()) {
            it->geometry = QRect(ev->x, ev->y, ev->width, ev->height);
            if (it->item)
                it->item->setPosition(QPointF(ev->x, ev->y));
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        auto *ev = reinterpret_cast<xcb_property_notify_event_t *>(e);
        if (XWindow *w = find(ev->window))
            w->readProperty(ev->atom);
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        auto *ev = reinterpret_cast<xcb_client_message_event_t *>(e);
        if (ev->type == atom("WL_SURFACE_ID")) {
            associate(ev->window, ev->data.data32[0]);
        } else if (ev->type == atom("_NET_ACTIVE_WINDOW")) {
            if (XWindow *w = find(ev->window))
                m_server->activate(w);
        } else if (ev->type == atom("_NET_CLOSE_WINDOW")) {
            if (XWindow *w = find(ev->window))
                w->sendClose();
        } else if (ev->type == atom("WM_CHANGE_STATE") && ev->data.data32[0] == 3) {
            if (XWindow *w = find(ev->window))
                m_server->minimize(w);
        } else if (ev->type == atom("_NET_WM_MOVERESIZE")) {
            if (XWindow *w = find(ev->window)) {
                const uint32_t dir = ev->data.data32[2];
                static const Qt::Edges edges[] = {Qt::TopEdge | Qt::LeftEdge, Qt::TopEdge, Qt::TopEdge | Qt::RightEdge, Qt::RightEdge,
                                                  Qt::BottomEdge | Qt::RightEdge, Qt::BottomEdge, Qt::BottomEdge | Qt::LeftEdge, Qt::LeftEdge};
                if (dir == 8)
                    m_server->startMove(w);
                else if (dir < 8)
                    m_server->startResize(w, edges[dir]);
            }
        } else if (ev->type == atom("_NET_WM_STATE")) {
            if (XWindow *w = find(ev->window)) {
                const uint32_t action = ev->data.data32[0];
                for (int i = 1; i <= 2; ++i) {
                    const uint32_t s = ev->data.data32[i];
                    if (s == atom("_NET_WM_STATE_FULLSCREEN"))
                        w->setFullscreen(action == 1 || (action == 2 && !w->fullscreen()));
                    else if ((s == atom("_NET_WM_STATE_MAXIMIZED_VERT") || s == atom("_NET_WM_STATE_MAXIMIZED_HORZ")) && i == 1) {
                        const bool on = action == 1 || (action == 2 && !w->zoomed());
                        if (on != w->zoomed())
                            w->toggleZoom();
                    }
                }
            }
        }
        break;
    }
    default: break;
    }
}

void Xwayland::associate(xcb_window_t w, uint32_t surfaceId)
{
    auto it = m_known.find(w);
    if (it == m_known.end())
        return;
    it->surfaceId = surfaceId;
    tryAssociate(w);
}

void Xwayland::surfaceCreated(QWaylandSurface *s)
{
    if (!m_client || s->client() != m_client)
        return;
    // the window whose WL_SURFACE_ID came first
    const auto keys = m_known.keys();
    for (xcb_window_t w : keys)
        if (m_known[w].surfaceId && !find(w) && !m_known[w].item)
            QMetaObject::invokeMethod(this, [this, w] { tryAssociate(w); }, Qt::QueuedConnection);
}

void Xwayland::tryAssociate(xcb_window_t w)
{
    auto it = m_known.find(w);
    if (it == m_known.end() || !it->surfaceId || !it->mapped || find(w) || it->item || !m_client)
        return;
    wl_resource *r = wl_client_get_object(m_client->client(), it->surfaceId);
    QWaylandSurface *s = r ? QWaylandSurface::fromResource(r) : nullptr;
    if (!s)
        return;   // not created yet: surfaceCreated tries again
    if (it->overrideRedirect)
        mapOverride(w, s);
    else
        mapManaged(w, s);
}

void Xwayland::mapManaged(xcb_window_t xw, QWaylandSurface *s)
{
    auto *w = new XWindow(m_server, this, xw, s);
    m_windows.insert(xw, w);
    const QRect g = m_known.value(xw).geometry;
    w->setGeometryFromX(g);
    auto show = [this, w, g] {
        // a dialog or a window that says where it goes keeps its place; the others cascade
        w->relayout();
        QPoint at = w->parentWindow() || (g.x() > 0 || g.y() > 0) ? g.topLeft() - QPoint(0, w->titleHeight())
                                                                  : m_server->placeWindow(w, w->frameGeometry().size());
        at.setY(qMax(at.y(), m_server->workArea().top()));
        w->frameItem()->setPosition(at);
        w->moved();
        m_server->addWindow(w);
        updateLists();
    };
    if (s->hasContent())
        show();
    else {
        auto *once = new QMetaObject::Connection;
        *once = connect(s, &QWaylandSurface::hasContentChanged, this, [this, s, show, once, xw] {
            if (!s->hasContent() || !find(xw))
                return;
            disconnect(*once);
            delete once;
            show();
        });
    }
}

void Xwayland::mapOverride(xcb_window_t xw, QWaylandSurface *s)
{
    // a menu, a tooltip: where it says, over the windows, without the keyboard
    auto it = m_known.find(xw);
    QWaylandQuickItem *item = m_server->createSurfaceItem(s, m_server->level(Server::Popups));
    item->setFocusOnClick(false);
    item->setPosition(it->geometry.topLeft());
    it->item = item;
}

void Xwayland::unmap(xcb_window_t xw)
{
    auto it = m_known.find(xw);
    if (it != m_known.end() && it->item) {
        delete it->item;
        it->item = nullptr;
    }
    if (XWindow *w = m_windows.take(xw)) {
        m_server->removeWindow(w);
        updateLists();
    }
}

void Xwayland::updateLists()
{
    if (!m_conn)
        return;
    QList<uint32_t> list;
    for (Window *w : m_server->windows())
        if (w->kind() == Window::X11)
            list << static_cast<XWindow *>(w)->xid();
    for (const char *name : {"_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING"})
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom(name), XCB_ATOM_WINDOW, 32, uint32_t(list.size()), list.constData());
    xcb_flush(m_conn);
}

void Xwayland::setActive(xcb_window_t w)
{
    if (!m_conn)
        return;
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW, 32, 1, &w);
    if (!w)
        xcb_set_input_focus(m_conn, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(m_conn);
}
