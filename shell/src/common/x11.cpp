// SPDX-License-Identifier: LGPL-2.1-or-later
#include "x11.h"

#include <xcb/shape.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QHash>
#include <QWidget>
#include <QtGui/qguiapplication_platform.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace X11 {

xcb_connection_t *conn()
{
    static xcb_connection_t *c = [] {
        auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
        return x11 ? x11->connection() : nullptr;
    }();
    return c;
}

xcb_window_t root()
{
    static xcb_window_t r = conn() ? xcb_setup_roots_iterator(xcb_get_setup(conn())).data->root : 0;
    return r;
}

xcb_atom_t atom(const char *name)
{
    static QHash<QByteArray, xcb_atom_t> cache;
    auto it = cache.constFind(name);
    if (it != cache.constEnd())
        return *it;
    xcb_atom_t a = XCB_ATOM_NONE;
    auto cookie = xcb_intern_atom(conn(), 0, uint16_t(strlen(name)), name);
    if (auto *reply = xcb_intern_atom_reply(conn(), cookie, nullptr)) {
        a = reply->atom;
        free(reply);
    }
    cache.insert(name, a);
    return a;
}

static xcb_get_property_reply_t *getProperty(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, uint32_t longs = 1 << 20)
{
    if (!conn() || !w)
        return nullptr;
    auto cookie = xcb_get_property(conn(), 0, w, prop, type, 0, longs);
    return xcb_get_property_reply(conn(), cookie, nullptr);
}

QVector<uint32_t> cardinals(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
    QVector<uint32_t> out;
    if (auto *reply = getProperty(w, prop, type)) {
        if (reply->format == 32) {
            auto *data = static_cast<uint32_t *>(xcb_get_property_value(reply));
            int n = xcb_get_property_value_length(reply) / 4;
            out = QVector<uint32_t>(data, data + n);
        }
        free(reply);
    }
    return out;
}

QByteArray bytes(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
    QByteArray out;
    if (auto *reply = getProperty(w, prop, type)) {
        out = QByteArray(static_cast<const char *>(xcb_get_property_value(reply)), xcb_get_property_value_length(reply));
        free(reply);
    }
    return out;
}

bool hasWindowManager()
{
    auto check = cardinals(root(), atom("_NET_SUPPORTING_WM_CHECK"), XCB_ATOM_WINDOW);
    return !check.isEmpty() && check[0];
}

void shapeToContent(QWidget *widget, const QImage &content, const QRegion &keep)
{
    // the window's SHAPE set here, not Qt's mask: Qt would clip the updates to it, and a Dock shaped
    // before its icons came would never be drawn again
    const QImage img = content.format() == QImage::Format_ARGB32_Premultiplied ? content : content.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const qreal dpr = img.devicePixelRatio();
    QRegion shape;
    QVector<QRect> rects;
    for (int y = 0; y < img.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width();) {
            if (qAlpha(line[x]) < 26) {
                ++x;
                continue;
            }
            const int start = x;
            while (x < img.width() && qAlpha(line[x]) >= 26)
                ++x;
            // in the window's pixels (X's): the device pixels
            rects.append(QRect(start, y, x - start, 1));
        }
    }
    shape.setRects(rects.constData(), int(rects.size()));
    for (const QRect &r : keep)
        shape += QRect(int(r.x() * dpr), int(r.y() * dpr), int(std::ceil(r.width() * dpr)), int(std::ceil(r.height() * dpr)));
    if (shape.isEmpty())
        shape = QRegion(0, 0, 1, 1);   // nothing: one pixel (no rectangle at all would mean no shape)
    const QVariant last = widget->property("_snowleopard_shape");
    if (last.isValid() && last.value<QRegion>() == shape)
        return;
    widget->setProperty("_snowleopard_shape", QVariant::fromValue(shape));
    QVector<xcb_rectangle_t> xr;
    for (const QRect &r : shape)
        xr.append({int16_t(r.x()), int16_t(r.y()), uint16_t(r.width()), uint16_t(r.height())});
    xcb_shape_rectangles(conn(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED, xcb_window_t(widget->winId()), 0, 0,
                         uint32_t(xr.size()), xr.constData());
    xcb_flush(conn());
}

bool compositing()
{
    if (!conn())
        return false;
    auto cookie = xcb_get_selection_owner(conn(), atom("_NET_WM_CM_S0"));
    bool owned = false;
    if (auto *reply = xcb_get_selection_owner_reply(conn(), cookie, nullptr)) {
        owned = reply->owner != XCB_NONE;
        free(reply);
    }
    return owned;
}

static bool isOwnOrSpecial(xcb_window_t w)
{
    // The shell's own windows and the ones that are not applications' (docks, desktops, menus)
    static const xcb_atom_t skip[] = {atom("_NET_WM_WINDOW_TYPE_DOCK"), atom("_NET_WM_WINDOW_TYPE_DESKTOP"),
                                      atom("_NET_WM_WINDOW_TYPE_MENU"), atom("_NET_WM_WINDOW_TYPE_POPUP_MENU"),
                                      atom("_NET_WM_WINDOW_TYPE_DROPDOWN_MENU"), atom("_NET_WM_WINDOW_TYPE_TOOLTIP"),
                                      atom("_NET_WM_WINDOW_TYPE_NOTIFICATION"), atom("_NET_WM_WINDOW_TYPE_SPLASH")};
    for (uint32_t type : cardinals(w, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM))
        for (xcb_atom_t s : skip)
            if (type == s)
                return true;
    for (uint32_t state : cardinals(w, atom("_NET_WM_STATE"), XCB_ATOM_ATOM))
        if (state == atom("_NET_WM_STATE_SKIP_TASKBAR"))
            return true;
    return false;
}

QVector<xcb_window_t> clientWindows()
{
    QVector<xcb_window_t> out;
    if (!conn())
        return out;
    if (hasWindowManager()) {
        auto list = cardinals(root(), atom("_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW);
        if (list.isEmpty())
            list = cardinals(root(), atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
        for (uint32_t w : list)
            if (!isOwnOrSpecial(w))
                out.append(w);
        return out;
    }
    // No window manager: the clients are the root's mapped children that have a WM_CLASS.
    auto cookie = xcb_query_tree(conn(), root());
    auto *tree = xcb_query_tree_reply(conn(), cookie, nullptr);
    if (!tree)
        return out;
    xcb_window_t *children = xcb_query_tree_children(tree);
    int n = xcb_query_tree_children_length(tree);
    for (int i = 0; i < n; ++i) {
        xcb_window_t w = children[i];
        auto attr = xcb_get_window_attributes_reply(conn(), xcb_get_window_attributes(conn(), w), nullptr);
        bool viewable = attr && attr->map_state == XCB_MAP_STATE_VIEWABLE && !attr->override_redirect;
        free(attr);
        if (viewable && !wmClass(w).isEmpty() && !isOwnOrSpecial(w))
            out.append(w);
    }
    free(tree);
    return out;
}

xcb_window_t activeWindow()
{
    if (hasWindowManager()) {
        auto active = cardinals(root(), atom("_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW);
        return active.isEmpty() ? 0 : active[0];
    }
    auto clients = clientWindows();   // bottom to top: the topmost stands for the active one
    return clients.isEmpty() ? 0 : clients.last();
}

static QPair<QString, QString> classHint(xcb_window_t w)
{
    QByteArray raw = bytes(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
    QList<QByteArray> parts = raw.split('\0');
    return {QString::fromLocal8Bit(parts.value(0)), QString::fromLocal8Bit(parts.value(1))};
}

QString wmClass(xcb_window_t w) { return classHint(w).second; }
QString wmInstance(xcb_window_t w) { return classHint(w).first; }

QString title(xcb_window_t w)
{
    QByteArray name = bytes(w, atom("_NET_WM_NAME"), atom("UTF8_STRING"));
    if (name.isEmpty())
        return QString::fromLocal8Bit(bytes(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING));
    return QString::fromUtf8(name);
}

QImage icon(xcb_window_t w, int size)
{
    // _NET_WM_ICON: width, height, then ARGB pixels, for each size; the smallest one at least `size`
    QVector<uint32_t> data = cardinals(w, atom("_NET_WM_ICON"), XCB_ATOM_CARDINAL);
    int best = -1, bestW = 0, bestH = 0;
    for (int i = 0; i + 2 <= data.size();) {
        int iw = int(data[i]), ih = int(data[i + 1]);
        if (iw <= 0 || ih <= 0 || i + 2 + iw * ih > data.size())
            break;
        bool better = best < 0 || (bestW < size ? iw > bestW : (iw >= size && iw < bestW));
        if (better)
            best = i, bestW = iw, bestH = ih;
        i += 2 + iw * ih;
    }
    if (best < 0)
        return {};
    QImage img(bestW, bestH, QImage::Format_ARGB32);
    for (int y = 0; y < bestH; ++y)
        memcpy(img.scanLine(y), data.constData() + best + 2 + y * bestW, size_t(bestW) * 4);
    return img;
}

uint32_t pid(xcb_window_t w)
{
    auto p = cardinals(w, atom("_NET_WM_PID"), XCB_ATOM_CARDINAL);
    return p.isEmpty() ? 0 : p[0];
}

bool minimized(xcb_window_t w)
{
    for (uint32_t state : cardinals(w, atom("_NET_WM_STATE"), XCB_ATOM_ATOM))
        if (state == atom("_NET_WM_STATE_HIDDEN"))
            return true;
    auto wmState = cardinals(w, atom("WM_STATE"), atom("WM_STATE"));
    return !wmState.isEmpty() && wmState[0] == 3;   // IconicState
}

static void clientMessage(xcb_window_t w, xcb_atom_t type, std::initializer_list<uint32_t> data)
{
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = w;
    ev.type = type;
    int i = 0;
    for (uint32_t d : data)
        ev.data.data32[i++] = d;
    xcb_send_event(conn(), 0, root(), XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&ev));
}

void activate(xcb_window_t w)
{
    if (!conn() || !w)
        return;
    if (hasWindowManager()) {
        clientMessage(w, atom("_NET_ACTIVE_WINDOW"), {2, XCB_CURRENT_TIME, 0});   // 2: from a pager
    } else {
        xcb_map_window(conn(), w);
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn(), w, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        xcb_set_input_focus(conn(), XCB_INPUT_FOCUS_POINTER_ROOT, w, XCB_CURRENT_TIME);
    }
    xcb_flush(conn());
}

void close(xcb_window_t w)
{
    if (!conn() || !w)
        return;
    if (hasWindowManager()) {
        clientMessage(w, atom("_NET_CLOSE_WINDOW"), {XCB_CURRENT_TIME, 2});
    } else {
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = w;
        ev.type = atom("WM_PROTOCOLS");
        ev.data.data32[0] = atom("WM_DELETE_WINDOW");
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(conn(), 0, w, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
    }
    xcb_flush(conn());
}

void minimize(xcb_window_t w)
{
    if (!conn() || !w)
        return;
    if (hasWindowManager())
        clientMessage(w, atom("WM_CHANGE_STATE"), {3});   // IconicState
    else
        xcb_unmap_window(conn(), w);
    xcb_flush(conn());
}

void hide(xcb_window_t w)
{
    if (!conn() || !w)
        return;
    if (hasWindowManager())
        clientMessage(w, atom("_SNOWLEOPARD_HIDE"), {1});
    else
        xcb_unmap_window(conn(), w);
    xcb_flush(conn());
}

bool hidden(xcb_window_t w)
{
    const auto h = cardinals(w, atom("_SNOWLEOPARD_HIDDEN"), XCB_ATOM_CARDINAL);
    return !h.isEmpty() && h[0];
}

QImage preview(xcb_window_t w)
{
    const QVector<uint32_t> data = cardinals(w, atom("_SNOWLEOPARD_WINDOW_PREVIEW"), XCB_ATOM_CARDINAL);
    if (data.size() < 2)
        return {};
    const int iw = int(data[0]), ih = int(data[1]);
    if (iw <= 0 || ih <= 0 || data.size() < 2 + iw * ih)
        return {};
    QImage img(iw, ih, QImage::Format_ARGB32);
    for (int y = 0; y < ih; ++y)
        memcpy(img.scanLine(y), data.constData() + 2 + y * iw, size_t(iw) * 4);
    return img;
}

void setIconGeometry(xcb_window_t w, const QRect &r)
{
    if (!conn() || !w)
        return;
    const uint32_t g[4] = {uint32_t(r.x()), uint32_t(r.y()), uint32_t(r.width()), uint32_t(r.height())};
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom("_NET_WM_ICON_GEOMETRY"), XCB_ATOM_CARDINAL, 32, 4, g);
}

static void setAtoms(xcb_window_t w, const char *prop, std::initializer_list<const char *> names)
{
    QVector<uint32_t> atoms;
    for (const char *n : names)
        atoms.append(atom(n));
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom(prop), XCB_ATOM_ATOM, 32, uint32_t(atoms.size()),
                        atoms.constData());
}

void setDock(QWidget *widget, int strutTop, int strutBottom)
{
    if (!conn())
        return;
    xcb_window_t w = xcb_window_t(widget->winId());
    setAtoms(w, "_NET_WM_WINDOW_TYPE", {"_NET_WM_WINDOW_TYPE_DOCK"});
    setAtoms(w, "_NET_WM_STATE", {"_NET_WM_STATE_ABOVE", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR",
                                  "_NET_WM_STATE_SKIP_PAGER"});
    const uint32_t all = 0xFFFFFFFF;
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom("_NET_WM_DESKTOP"), XCB_ATOM_CARDINAL, 32, 1, &all);
    QRect g = widget->geometry();
    // left, right, top, bottom, then the start and end of each
    uint32_t strut[12] = {0, 0, uint32_t(strutTop), uint32_t(strutBottom), 0, 0, 0, 0,
                          uint32_t(g.left()), uint32_t(g.right()), uint32_t(g.left()), uint32_t(g.right())};
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom("_NET_WM_STRUT_PARTIAL"), XCB_ATOM_CARDINAL, 32, 12, strut);
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom("_NET_WM_STRUT"), XCB_ATOM_CARDINAL, 32, 4, strut);
    xcb_flush(conn());
}

void setDesktop(QWidget *widget)
{
    if (!conn())
        return;
    xcb_window_t w = xcb_window_t(widget->winId());
    setAtoms(w, "_NET_WM_WINDOW_TYPE", {"_NET_WM_WINDOW_TYPE_DESKTOP"});
    setAtoms(w, "_NET_WM_STATE", {"_NET_WM_STATE_BELOW", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR",
                                  "_NET_WM_STATE_SKIP_PAGER"});
    if (!hasWindowManager()) {
        const uint32_t below = XCB_STACK_MODE_BELOW;
        xcb_configure_window(conn(), w, XCB_CONFIG_WINDOW_STACK_MODE, &below);
    }
    xcb_flush(conn());
}

void setMagicMirror(QWidget *widget, int direction, const QPolygon &quad)
{
    if (!conn())
        return;
    const xcb_window_t w = xcb_window_t(widget->winId());
    static QHash<xcb_window_t, QVector<uint32_t>> set;
    QVector<uint32_t> value;
    if (direction > 0 && quad.size() == 4) {
        value << uint32_t(direction);
        for (const QPoint &p : quad)
            value << uint32_t(p.x()) << uint32_t(p.y());
    }
    if (set.value(w) == value && set.contains(w))
        return;
    set.insert(w, value);
    if (value.isEmpty())
        xcb_delete_property(conn(), w, atom("_SNOWLEOPARD_MAGIC_MIRROR"));
    else
        xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom("_SNOWLEOPARD_MAGIC_MIRROR"), XCB_ATOM_CARDINAL, 32,
                            uint32_t(value.size()), value.constData());
    xcb_flush(conn());
}

void setMenu(QWidget *widget)
{
    if (!conn())
        return;
    setAtoms(xcb_window_t(widget->winId()), "_NET_WM_WINDOW_TYPE", {"_NET_WM_WINDOW_TYPE_DROPDOWN_MENU"});
    xcb_flush(conn());
}

void setUnifiedToolbar(QWidget *widget, int height)
{
    if (!conn())
        return;
    const uint32_t h = uint32_t(height);
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, xcb_window_t(widget->winId()), atom("_SNOWLEOPARD_UNIFIED_TOOLBAR"),
                        XCB_ATOM_CARDINAL, 32, 1, &h);
    xcb_flush(conn());
}

QColor unifiedToolbarColor(int row, int totalHeight, bool active)
{
    // measured: a 218 first row, then 207 down to 167 at the toolbar's last row (the 97 line under it is
    // the toolbar's); inactive: lighter, estimated
    if (row <= 0)
        return active ? QColor(218, 218, 218) : QColor(236, 236, 236);
    const qreal t = qBound(0.0, qreal(row - 1) / qMax(1, totalHeight - 2), 1.0);
    const int top = active ? 207 : 232, bottom = active ? 167 : 207;
    const int v = qRound(top + (bottom - top) * t);
    return QColor(v, v, v);
}

Watcher::Watcher(QObject *parent) : QObject(parent)
{
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(40);
    connect(&m_coalesce, &QTimer::timeout, this, &Watcher::changed);
    if (!conn())
        return;
    // Keep what the root already listens to (Qt selects some), and add ours.
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    if (auto *attr = xcb_get_window_attributes_reply(conn(), xcb_get_window_attributes(conn(), root()), nullptr)) {
        mask |= attr->your_event_mask;
        free(attr);
    }
    xcb_change_window_attributes(conn(), root(), XCB_CW_EVENT_MASK, &mask);
    xcb_flush(conn());
    QCoreApplication::instance()->installNativeEventFilter(this);
}

Watcher::~Watcher()
{
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
}

bool Watcher::nativeEventFilter(const QByteArray &type, void *message, qintptr *)
{
    if (type != "xcb_generic_event_t")
        return false;
    auto *ev = static_cast<xcb_generic_event_t *>(message);
    switch (ev->response_type & ~0x80) {
    case XCB_PROPERTY_NOTIFY: {
        auto *p = reinterpret_cast<xcb_property_notify_event_t *>(ev);
        if (p->window == root() && (p->atom == atom("_NET_ACTIVE_WINDOW") || p->atom == atom("_NET_CLIENT_LIST") ||
                                    p->atom == atom("_NET_CLIENT_LIST_STACKING") || p->atom == atom("_SNOWLEOPARD_FRONT_APP")))
            m_coalesce.start();
        break;
    }
    case XCB_MAP_NOTIFY:
    case XCB_UNMAP_NOTIFY:
    case XCB_DESTROY_NOTIFY:
    case XCB_CONFIGURE_NOTIFY:
        if (!hasWindowManager())
            m_coalesce.start();
        break;
    default:
        break;
    }
    return false;
}

} // namespace X11
