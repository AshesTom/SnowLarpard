// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aqua.h"
#include "windowserver.h"

#include <xcb/xcb_cursor.h>

#include "artfile.h"
#include "assets.h"
#include "session.h"
#include "desktopsettings.h"
#include "x11.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <cmath>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <cmath>
#include <cstdlib>
#include <xcb/shape.h>
#include <xcb/xcb_keysyms.h>

using X11::atom;

using Frame::GripSize;
static constexpr uint32_t ICCCM_WithdrawnState = 0, ICCCM_NormalState = 1, ICCCM_IconicState = 3;

WindowServer::WindowServer() {}

bool WindowServer::start()
{
    m_conn = X11::conn();
    m_root = X11::root();
    if (!m_conn)
        return false;
    auto *setup = xcb_setup_roots_iterator(xcb_get_setup(m_conn)).data;
    m_depth = setup->root_depth;
    m_screen = QRect(0, 0, setup->width_in_pixels, setup->height_in_pixels);
    m_workArea = m_screen;

    // Become the window manager (keeping the events Qt already listens to on the root)
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    if (auto *attr = xcb_get_window_attributes_reply(m_conn, xcb_get_window_attributes(m_conn, m_root), nullptr)) {
        mask |= attr->your_event_mask;
        free(attr);
    }
    if (xcb_generic_error_t *err = xcb_request_check(m_conn, xcb_change_window_attributes_checked(m_conn, m_root, XCB_CW_EVENT_MASK, &mask))) {
        free(err);
        return false;
    }
    QCoreApplication::instance()->installNativeEventFilter(this);
    // the arrow over the desktop and the frames (which inherit the root's): the cursor theme's
    // (XCURSOR_THEME, the session's Snow Leopard cursors from CoreGraphics and HIServices)
    // the theme in the X resources (Xcursor.theme, Xcursor.size), where libxcb-cursor and the
    // applications' toolkits look, as a session's xrdb would put it
    if (const char *theme = std::getenv("XCURSOR_THEME")) {
        const char *size = std::getenv("XCURSOR_SIZE");
        QByteArray resources;
        for (const QByteArray &line : X11::bytes(m_root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING).split('\n'))
            if (!line.isEmpty() && !line.startsWith("Xcursor.theme:") && !line.startsWith("Xcursor.size:"))
                resources += line + '\n';
        resources += QByteArray("Xcursor.theme:\t") + theme + "\nXcursor.size:\t" + (size ? size : "24") + "\n";
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 8,
                            uint32_t(resources.size()), resources.constData());
        xcb_flush(m_conn);
    }
    xcb_cursor_context_t *cursors = nullptr;
    if (xcb_cursor_context_new(m_conn, setup, &cursors) >= 0) {
        const uint32_t arrow = xcb_cursor_load_cursor(cursors, "default");
        if (arrow != XCB_CURSOR_NONE)
            xcb_change_window_attributes(m_conn, m_root, XCB_CW_CURSOR, &arrow);
        xcb_cursor_context_free(cursors);
    }

    // The compositor: the windows' shadows. A frame's is the key window's or another window's, a
    // panel's the other style's; none in full screen. Without it (no Composite, or another
    // compositor) the windows are drawn by the server as before.
    m_compositor.start(m_conn, m_root, [this](xcb_window_t w) {
        Client *c = find(w);
        if (!c || c->frame != w)
            return -1;
        if (c->fullscreen)
            return int(Compositor::NoShadow);
        if (c->floating)
            return int(Compositor::PanelShadow);
        return int(c == m_active ? Compositor::KeyShadow : Compositor::WindowShadow);
    });

    // the desktop picture at once, where the session would otherwise be black until the Finder's
    // desktop is up (as the Mac's login shows the picture before the menu bar and the Dock)
    paintDesktopPicture();

    // EWMH: who we are, what we do
    m_check = xcb_generate_id(m_conn);
    xcb_create_window(m_conn, XCB_COPY_FROM_PARENT, m_check, m_root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, 0, nullptr);
    for (xcb_window_t w : {m_root, m_check})
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, w, atom("_NET_SUPPORTING_WM_CHECK"), XCB_ATOM_WINDOW, 32, 1, &m_check);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_check, atom("_NET_WM_NAME"), atom("UTF8_STRING"), 8, 12, "WindowServer");
    const char *supported[] = {"_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING",
                               "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_NET_WM_NAME", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN",
                               "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
                               "_NET_WM_STATE_ABOVE", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP",
                               "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_STRUT", "_NET_WM_STRUT_PARTIAL",
                               "_NET_WORKAREA", "_NET_FRAME_EXTENTS", "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
                               "_NET_DESKTOP_GEOMETRY", "_NET_WM_DESKTOP", "_NET_DESKTOP_LAYOUT", "_NET_WM_STATE_STICKY"};
    QVector<uint32_t> atoms;
    for (const char *s : supported)
        atoms.append(atom(s));
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_SUPPORTED"), XCB_ATOM_ATOM, 32, atoms.size(), atoms.constData());
    const uint32_t one = 1, zero = 0;
    const uint32_t geometry[2] = {uint32_t(m_screen.width()), uint32_t(m_screen.height())};
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_NUMBER_OF_DESKTOPS"), XCB_ATOM_CARDINAL, 32, 1, &one);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_CURRENT_DESKTOP"), XCB_ATOM_CARDINAL, 32, 1, &zero);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_DESKTOP_GEOMETRY"), XCB_ATOM_CARDINAL, 32, 2, geometry);

    m_gc = xcb_generate_id(m_conn);
    xcb_create_gc(m_conn, m_gc, m_root, 0, nullptr);
    m_keySymbols = xcb_key_symbols_alloc(m_conn);
    grabKeys();
    // Spaces: the Dock's preferences, read again when they change (com.apple.dock's notification)
    m_slideTimer.setInterval(16);
    QObject::connect(&m_slideTimer, &QTimer::timeout, this, [this] { stepSlide(); });
    m_bezelTimer.setInterval(16);
    QObject::connect(&m_bezelTimer, &QTimer::timeout, this, [this] { stepBezel(); });
    readSpacesPrefs();
    publishSpaces();
    auto *prefsTimer = new QTimer(this);
    QObject::connect(prefsTimer, &QTimer::timeout, this, [this] {
        static QDateTime dock, global;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/";
        const QDateTime d = QFileInfo(dir + "dock.conf").lastModified(), g = QFileInfo(dir + "appearance.conf").lastModified();
        if (d != dock || g != global) {
            dock = d;
            global = g;
            readSpacesPrefs();
        }
    });
    prefsTimer->start(1000);

    // The windows that are already there
    if (auto *tree = xcb_query_tree_reply(m_conn, xcb_query_tree(m_conn, m_root), nullptr)) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
            auto *attr = xcb_get_window_attributes_reply(m_conn, xcb_get_window_attributes(m_conn, children[i]), nullptr);
            bool viewable = attr && !attr->override_redirect && attr->map_state == XCB_MAP_STATE_VIEWABLE;
            free(attr);
            if (viewable && children[i] != m_check)
                manage(children[i], true);
        }
        free(tree);
    }
    updateWorkArea();
    updateClientLists();
    setActiveProperty(0);
    xcb_flush(m_conn);
    return true;
}

WindowServer::Client *WindowServer::find(xcb_window_t w) { return m_byWindow.value(w); }

// ---- managing

static bool hasType(xcb_window_t w, std::initializer_list<const char *> types)
{
    for (uint32_t t : X11::cardinals(w, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM))
        for (const char *name : types)
            if (t == atom(name))
                return true;
    return false;
}

void WindowServer::manage(xcb_window_t w, bool existing)
{
    if (find(w) || m_docks.contains(w) || m_desktops.contains(w))
        return;
    const uint32_t watch = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    // The shell's layers: not framed, not in the client list
    if (hasType(w, {"_NET_WM_WINDOW_TYPE_DESKTOP"})) {
        xcb_change_window_attributes(m_conn, w, XCB_CW_EVENT_MASK, &watch);
        m_desktops.append(w);
        xcb_map_window(m_conn, w);
        restackLayers();
        return;
    }
    if (hasType(w, {"_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_NOTIFICATION",
                    "_NET_WM_WINDOW_TYPE_TOOLTIP", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_POPUP_MENU"})) {
        xcb_change_window_attributes(m_conn, w, XCB_CW_EVENT_MASK, &watch);
        m_docks.append(w);
        xcb_map_window(m_conn, w);
        updateWorkArea();
        restackLayers();
        return;
    }

    auto *geo = xcb_get_geometry_reply(m_conn, xcb_get_geometry(m_conn, w), nullptr);
    if (!geo)
        return;
    auto *c = new Client;
    c->window = w;
    c->geometry = QRect(geo->x, geo->y, geo->width, geo->height);
    free(geo);
    readProperties(c);

    // WM_NORMAL_HINTS: a position given by the user or the program is kept
    QByteArray hints = X11::bytes(w, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS);
    bool hasPosition = false;
    if (hints.size() >= 4 * 7) {
        const auto *h = reinterpret_cast<const uint32_t *>(hints.constData());
        hasPosition = (h[0] & 5) != 0;   // USPosition (1) | PPosition (4)
    }
    readSizeHints(c);
    if (!existing)
        place(c, hasPosition && !c->geometry.topLeft().isNull());
    else
        c->geometry.moveTop(qMax(c->geometry.top(), m_workArea.top() + c->titleHeight()));

    // The frame: the title bar, and the client under it
    c->frame = xcb_generate_id(m_conn);
    const uint32_t frameMask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const uint32_t frameValues[] = {0xEEEEEE, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                                                  XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                                  XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_EXPOSURE |
                                                  XCB_EVENT_MASK_LEAVE_WINDOW};
    const int th = c->titleHeight();
    xcb_create_window(m_conn, m_depth, c->frame, m_root, c->geometry.x(), c->geometry.y() - th, c->geometry.width(),
                      c->geometry.height() + th, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, frameMask, frameValues);
    // the size box: an invisible corner over the client's, where dragging resizes
    c->grip = xcb_generate_id(m_conn);
    xcb_font_t cursorFont = xcb_generate_id(m_conn);
    xcb_open_font(m_conn, cursorFont, 6, "cursor");
    xcb_cursor_t corner = xcb_generate_id(m_conn);
    xcb_create_glyph_cursor(m_conn, corner, cursorFont, cursorFont, 14, 15, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF);   // bottom_right_corner
    xcb_close_font(m_conn, cursorFont);
    const uint32_t gripValues[] = {XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION, corner};
    xcb_create_window(m_conn, 0, c->grip, c->frame, c->geometry.width() - GripSize, th + c->geometry.height() - GripSize,
                      GripSize, GripSize, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK | XCB_CW_CURSOR,
                      gripValues);

    xcb_change_window_attributes(m_conn, w, XCB_CW_EVENT_MASK, &watch);
    xcb_change_save_set(m_conn, XCB_SET_MODE_INSERT, w);
    const uint32_t noBorder = 0;
    xcb_configure_window(m_conn, w, XCB_CONFIG_WINDOW_BORDER_WIDTH, &noBorder);
    if (existing)
        c->ignoreUnmaps++;   // reparenting a mapped window unmaps it once
    xcb_reparent_window(m_conn, w, c->frame, 0, th);
    // click to focus: the first click is the window manager's, then replayed to the application
    xcb_grab_button(m_conn, 1, w, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
                    XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);

    m_clients.append(c);
    m_byWindow.insert(w, c);
    m_byWindow.insert(c->frame, c);
    m_byWindow.insert(c->grip, c);

    const uint32_t extents[4] = {0, 0, uint32_t(th), 0};
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, w, atom("_NET_FRAME_EXTENTS"), XCB_ATOM_CARDINAL, 32, 4, extents);
    // its space: the one it had (a window kept from before), its parent's, else the current one
    // (CGS puts a new window on the active workspace); on all of them when sticky
    c->space = m_space;
    for (Client *p : std::as_const(m_clients))
        if (c->transientFor && p->window == c->transientFor)
            c->space = p->space, c->allSpaces = p->allSpaces;
    if (existing) {
        const QVector<uint32_t> d = X11::cardinals(w, atom("_NET_WM_DESKTOP"), XCB_ATOM_CARDINAL);
        if (!d.isEmpty())
            d[0] == 0xFFFFFFFF ? void(c->allSpaces = true) : void(c->space = qBound(0, int(d[0]), spaceCount() - 1));
    }
    if (X11::cardinals(w, atom("_NET_WM_STATE"), XCB_ATOM_ATOM).contains(atom("_NET_WM_STATE_STICKY")))
        c->allSpaces = true;
    const uint32_t desktop = c->allSpaces ? 0xFFFFFFFF : uint32_t(c->space);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, w, atom("_NET_WM_DESKTOP"), XCB_ATOM_CARDINAL, 32, 1, &desktop);

    // full screen asked for before mapping; a panel's own states (loginwindow's About This Mac)
    for (uint32_t s : X11::cardinals(w, atom("_NET_WM_STATE"), XCB_ATOM_ATOM)) {
        if (s == atom("_NET_WM_STATE_FULLSCREEN"))
            c->fullscreen = true, c->restore = c->geometry;
        if (s == atom("_NET_WM_STATE_ABOVE") || s == atom("_NET_WM_STATE_SKIP_TASKBAR") || s == atom("_NET_WM_STATE_STICKY"))
            c->keptStates << s;
        if (s == atom("_NET_WM_STATE_ABOVE"))
            c->floating = true;
    }
    if (hasType(w, {"_NET_WM_WINDOW_TYPE_UTILITY"}))
        c->floating = true;
    if (c->fullscreen) {
        c->fullscreen = false;
        setFullscreen(c, true);
    }

    applyGeometry(c);
    m_compositor.setOpacity(c->frame, c->opacity);
    xcb_map_window(m_conn, w);
    xcb_map_window(m_conn, c->grip);
    if (onCurrentSpace(c))
        xcb_map_window(m_conn, c->frame);
    setWmState(c, ICCCM_NormalState);
    paintTitle(c);
    raise(c);
    if (onCurrentSpace(c))
        focus(c);
    updateClientLists();
}

void WindowServer::paintDesktopPicture()
{
    if (m_screen.isEmpty())
        return;
    QImage img = Desktop::render(Desktop::load(), m_screen.size()).convertToFormat(QImage::Format_RGB32);
    if (img.size() != m_screen.size())
        return;
    const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
    xcb_create_pixmap(m_conn, m_depth, pixmap, m_root, uint16_t(img.width()), uint16_t(img.height()));
    const xcb_gcontext_t gc = xcb_generate_id(m_conn);
    xcb_create_gc(m_conn, gc, pixmap, 0, nullptr);
    const uint32_t maxBytes = xcb_get_maximum_request_length(m_conn) * 4 - 64;
    const int rowsPerRequest = qMax(1, int(maxBytes / uint32_t(img.bytesPerLine())));
    for (int y = 0; y < img.height(); y += rowsPerRequest) {
        const int rows = qMin(rowsPerRequest, img.height() - y);
        xcb_put_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, uint16_t(img.width()), uint16_t(rows), 0, int16_t(y), 0,
                      m_depth, uint32_t(img.bytesPerLine() * rows), img.constScanLine(y));
    }
    xcb_free_gc(m_conn, gc);
    // the root's background (without the compositor, or before it draws), and the compositor's
    xcb_change_window_attributes(m_conn, m_root, XCB_CW_BACK_PIXMAP, &pixmap);
    xcb_clear_area(m_conn, 0, m_root, 0, 0, 0, 0);
    m_compositor.setBackground(pixmap);
    xcb_free_pixmap(m_conn, pixmap);   // the root and the picture keep it
    xcb_flush(m_conn);
}

void WindowServer::unmanage(Client *c, bool destroyed)
{
    if (!destroyed) {
        xcb_ungrab_button(m_conn, XCB_BUTTON_INDEX_ANY, c->window, XCB_MOD_MASK_ANY);
        xcb_reparent_window(m_conn, c->window, m_root, c->geometry.x(), c->geometry.y());
        xcb_change_save_set(m_conn, XCB_SET_MODE_DELETE, c->window);
        setWmState(c, ICCCM_WithdrawnState);
    }
    m_compositor.dropSnapshot(c->window);
    xcb_destroy_window(m_conn, c->frame);   // and the grip with it
    m_byWindow.remove(c->window);
    m_byWindow.remove(c->frame);
    m_byWindow.remove(c->grip);
    m_clients.removeAll(c);
    m_slide.in.removeAll(c);
    m_slide.out.removeAll(c);
    if (m_slide.carried == c)
        m_slide.carried = nullptr;
    if (m_dragClient == c)
        m_drag = NoDrag, m_dragClient = nullptr;
    bool wasActive = m_active == c;
    if (wasActive)
        m_active = nullptr;
    delete c;
    if (wasActive) {
        // the topmost window left takes the focus, as the Mac gives it to the application's next window
        for (int i = m_clients.size() - 1; i >= 0; --i)
            if (!m_clients[i]->minimized && onCurrentSpace(m_clients[i])) {
                focus(m_clients[i]);
                break;
            }
        if (!m_active)
            setActiveProperty(0);
    }
    updateClientLists();
}

void WindowServer::readSizeHints(Client *c)
{
    const QByteArray hints = X11::bytes(c->window, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS);
    c->minimum = c->maximum = QSize();
    if (hints.size() < 4 * 9)
        return;
    const auto *h = reinterpret_cast<const uint32_t *>(hints.constData());
    if (h[0] & 16)   // PMinSize
        c->minimum = QSize(int(h[5]), int(h[6]));
    if (h[0] & 32)   // PMaxSize
        c->maximum = QSize(int(h[7]), int(h[8]));
}

void WindowServer::readProperties(Client *c)
{
    QByteArray name = X11::bytes(c->window, atom("_NET_WM_NAME"), atom("UTF8_STRING"));
    c->title = name.isEmpty() ? QString::fromLocal8Bit(X11::bytes(c->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING))
                              : QString::fromUtf8(name);
    c->deleteWindow = c->takeFocus = false;
    for (uint32_t p : X11::cardinals(c->window, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM)) {
        if (p == atom("WM_DELETE_WINDOW"))
            c->deleteWindow = true;
        if (p == atom("WM_TAKE_FOCUS"))
            c->takeFocus = true;
    }
    QVector<uint32_t> wmHints = X11::cardinals(c->window, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS);
    c->acceptsInput = wmHints.size() < 2 || !(wmHints[0] & 1) || wmHints[1];
    // _MOTIF_WM_HINTS: flags, functions, decorations; decorations 0 means the program draws its own
    QVector<uint32_t> motif = X11::cardinals(c->window, atom("_MOTIF_WM_HINTS"), atom("_MOTIF_WM_HINTS"));
    c->decorated = !(motif.size() >= 3 && (motif[0] & 2) && motif[2] == 0);
    // functions without MWM_FUNC_ALL (1), MWM_FUNC_MINIMIZE (8), MWM_FUNC_MAXIMIZE (16) nor
    // MWM_FUNC_CLOSE (32): a titled window without buttons (AppKit hides the three then)
    c->buttons = !(motif.size() >= 2 && (motif[0] & 1) && !(motif[1] & (1 | 8 | 16 | 32)));
    c->utility = hasType(c->window, {"_NET_WM_WINDOW_TYPE_UTILITY"});
    QVector<uint32_t> unified = X11::cardinals(c->window, atom("_SNOWLEOPARD_UNIFIED_TOOLBAR"), XCB_ATOM_CARDINAL);
    c->unifiedToolbar = unified.isEmpty() ? 0 : int(unified[0]);
    c->hasToolbar = unified.size() > 1 ? unified[1] != 0 : c->unifiedToolbar > 0;
    // width, height, then ARGB rows (straight alpha), as _NET_WM_ICON
    QVector<uint32_t> proxy = X11::cardinals(c->window, atom("_SNOWLEOPARD_PROXY_ICON"), XCB_ATOM_CARDINAL);
    c->proxyIcon = QImage();
    if (proxy.size() > 2 && proxy.size() >= 2 + qsizetype(proxy[0]) * qsizetype(proxy[1])) {
        QImage icon(int(proxy[0]), int(proxy[1]), QImage::Format_ARGB32);
        for (int y = 0; y < icon.height(); ++y)
            for (int x = 0; x < icon.width(); ++x)
                icon.setPixel(x, y, proxy[2 + y * icon.width() + x]);
        c->proxyIcon = icon;
    }
    QVector<uint32_t> edited = X11::cardinals(c->window, atom("_SNOWLEOPARD_DOCUMENT_EDITED"), XCB_ATOM_CARDINAL);
    c->documentEdited = !edited.isEmpty() && edited[0];
    QVector<uint32_t> rounded = X11::cardinals(c->window, atom("_SNOWLEOPARD_ROUNDED_BOTTOM"), XCB_ATOM_CARDINAL);
    c->roundedBottom = !rounded.isEmpty() && rounded[0];
    c->outlined = !rounded.isEmpty();
    QVector<uint32_t> opacity = X11::cardinals(c->window, atom("_NET_WM_WINDOW_OPACITY"), XCB_ATOM_CARDINAL);
    c->opacity = opacity.isEmpty() ? 1.0 : opacity[0] / double(0xffffffffu);
    QVector<uint32_t> transient = X11::cardinals(c->window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW);
    c->transientFor = transient.isEmpty() ? 0 : transient[0];
}

void WindowServer::place(Client *c, bool hasPosition)
{
    QRect &g = c->geometry;
    const int th = c->titleHeight();
    if (Client *parent = find(c->transientFor)) {
        // dialogs: centred on their window, a little down (sheets are for later)
        g.moveCenter(parent->geometry.center());
        g.moveTop(parent->geometry.top() + 20);
    } else if (!hasPosition) {
        // a cascade from the top left of the work area
        const int step = 22;
        g.moveTopLeft(QPoint(m_workArea.left() + 40 + m_cascade * step, m_workArea.top() + th + 20 + m_cascade * step));
        m_cascade = (m_cascade + 1) % 10;
    }
    // inside the work area, the title bar under the menu bar
    if (g.right() > m_workArea.right())
        g.moveRight(qMax(m_workArea.left() + g.width() - 1, m_workArea.right()));
    if (g.left() < m_workArea.left())
        g.moveLeft(m_workArea.left());
    if (g.bottom() > m_workArea.bottom())   // a tall window (a document's) up, its title bar kept in
        g.moveBottom(qMax(m_workArea.top() + th + g.height() - 1, m_workArea.bottom()));
    if (g.top() - th < m_workArea.top())
        g.moveTop(m_workArea.top() + th);
}

void WindowServer::applyGeometry(Client *c)
{
    const int th = c->titleHeight();
    const QRect &g = c->geometry;
    const uint32_t frame[] = {uint32_t(g.x()), uint32_t(g.y() - th), uint32_t(qMax(1, g.width())), uint32_t(qMax(1, g.height() + th))};
    xcb_configure_window(m_conn, c->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, frame);
    const uint32_t client[] = {0, uint32_t(th), uint32_t(qMax(1, g.width())), uint32_t(qMax(1, g.height()))};
    xcb_configure_window(m_conn, c->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, client);
    const uint32_t grip[] = {uint32_t(qMax(0, g.width() - GripSize)), uint32_t(qMax(0, th + g.height() - GripSize))};
    xcb_configure_window(m_conn, c->grip, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, grip);
    const uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(m_conn, c->grip, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    const uint32_t extents[4] = {0, 0, uint32_t(th), 0};
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, c->window, atom("_NET_FRAME_EXTENTS"), XCB_ATOM_CARDINAL, 32, 4, extents);
    applyShape(c);
    sendConfigureNotify(c);
}

// The frame's outline (CoreUI's CreateWindowPath): its top corners rounded, radius 4, its bottom ones
// too for a textured window; whole pixels, those whose centre is in the corner's circle. An undecorated
// window drawing its own frame (iTunes') has it when it says which bottom corners it has. The shadows
// (AppKit's __NSSetWindowShadow) are the compositor's (compositor.cpp); AquaKit draws them on Wayland
void WindowServer::applyShape(Client *c)
{
    const QRect &g = c->geometry;
    const int w = qMax(1, g.width()), h = qMax(1, g.height() + c->titleHeight());
    QVector<xcb_rectangle_t> rects;
    if ((!c->decorated && !c->outlined) || c->fullscreen) {
        rects.append({0, 0, uint16_t(w), uint16_t(h)});
    } else {
        constexpr double r = 4;
        int insets[int(r)];
        for (int y = 0; y < int(r); ++y) {
            const double dy = r - (y + 0.5);
            insets[y] = int(std::ceil(r - std::sqrt(r * r - dy * dy) - 0.5));
        }
        const int bottom = c->roundedBottom ? int(r) : 0;
        for (int y = 0; y < int(r); ++y)
            rects.append({int16_t(insets[y]), int16_t(y), uint16_t(qMax(1, w - 2 * insets[y])), 1});
        rects.append({0, int16_t(r), uint16_t(w), uint16_t(qMax(1, h - int(r) - bottom))});
        for (int y = 0; y < bottom; ++y)
            rects.append({int16_t(insets[y]), int16_t(h - 1 - y), uint16_t(qMax(1, w - 2 * insets[y])), 1});
    }
    xcb_shape_rectangles(m_conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED, c->frame, 0, 0,
                         uint32_t(rects.size()), rects.constData());
}

void WindowServer::sendConfigureNotify(Client *c)
{
    // ICCCM 4.1.5: where the client is on the screen, though its parent is our frame
    xcb_configure_notify_event_t ev = {};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = ev.window = c->window;
    ev.x = int16_t(c->geometry.x());
    ev.y = int16_t(c->geometry.y());
    ev.width = uint16_t(c->geometry.width());
    ev.height = uint16_t(c->geometry.height());
    xcb_send_event(m_conn, 0, c->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<const char *>(&ev));
}

// ---- the title bar (frame.cpp's, as the Wayland server draws it)

Frame::TitleBar WindowServer::titleBar(const Client *c) const
{
    Frame::TitleBar t;
    t.width = c->geometry.width();
    t.title = c->title;
    t.active = c == m_active;
    t.utility = c->utility;
    t.buttons = c->buttons;
    t.hasToolbar = c->hasToolbar;
    t.unifiedToolbar = c->unifiedToolbar;
    t.proxyIcon = c->proxyIcon;
    t.documentEdited = c->documentEdited;
    t.pressed = c->pressed;
    t.overButtons = c->overButtons;
    return t;
}

QRect WindowServer::buttonRect(const Client *c, int b) const { return Frame::buttonRect(titleBar(c), b); }

int WindowServer::buttonAt(const Client *c, QPoint p) const
{
    if (!c->titleHeight())
        return None;
    return Frame::buttonAt(titleBar(c), p);
}

void WindowServer::paintTitle(Client *c)
{
    const int th = c->titleHeight();
    const int w = c->geometry.width();
    if (!th || w <= 0)
        return;
    const QImage img = Frame::paintTitleBar(titleBar(c));
    // to the frame, a band of rows at a time within the request's limit
    const uint32_t maxBytes = xcb_get_maximum_request_length(m_conn) * 4 - 64;
    const int rowsPerRequest = qMax(1, int(maxBytes / uint32_t(img.bytesPerLine())));
    for (int y = 0; y < th; y += rowsPerRequest) {
        int rows = qMin(rowsPerRequest, th - y);
        xcb_put_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, c->frame, m_gc, uint16_t(w), uint16_t(rows), 0, int16_t(y), 0,
                      m_depth, uint32_t(img.bytesPerLine() * rows), img.constScanLine(y));
    }
    xcb_flush(m_conn);
}

// ---- focus and stacking

void WindowServer::setActiveProperty(xcb_window_t w)
{
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW, 32, 1, &w);
}

void WindowServer::focus(Client *c)
{
    Client *old = m_active;
    m_active = c;
    if (c) {
        if (c->acceptsInput)
            xcb_set_input_focus(m_conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->window, XCB_CURRENT_TIME);
        if (c->takeFocus) {
            xcb_client_message_event_t ev = {};
            ev.response_type = XCB_CLIENT_MESSAGE;
            ev.format = 32;
            ev.window = c->window;
            ev.type = atom("WM_PROTOCOLS");
            ev.data.data32[0] = atom("WM_TAKE_FOCUS");
            ev.data.data32[1] = XCB_CURRENT_TIME;
            xcb_send_event(m_conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
        }
    }
    setActiveProperty(c ? c->window : 0);
    if (old && old != c && m_clients.contains(old))
        paintTitle(old);
    if (c)
        paintTitle(c);
    m_compositor.damageAll();   // the key window's shadow is deeper
    xcb_flush(m_conn);
}

void WindowServer::raise(Client *c)
{
    const uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(m_conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    // its dialogs over it
    for (Client *t : std::as_const(m_clients))
        if (t->transientFor == c->window && !t->minimized)
            xcb_configure_window(m_conn, t->frame, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    m_clients.removeAll(c);
    m_clients.append(c);
    restackLayers();
    updateClientLists();
}

void WindowServer::restackLayers()
{
    // the desktop under everything, the Dock and the menu bar over everything (full screen windows aside)
    const uint32_t below = XCB_STACK_MODE_BELOW, above = XCB_STACK_MODE_ABOVE;
    for (xcb_window_t d : std::as_const(m_desktops))
        xcb_configure_window(m_conn, d, XCB_CONFIG_WINDOW_STACK_MODE, &below);
    bool fullscreenOnTop = m_active && m_active->fullscreen;
    // floating panels over the windows, under the Dock and the menu bar
    for (Client *f : std::as_const(m_clients))
        if (f->floating && !f->minimized)
            xcb_configure_window(m_conn, f->frame, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    if (!fullscreenOnTop)
        for (xcb_window_t d : std::as_const(m_docks))
            xcb_configure_window(m_conn, d, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    xcb_flush(m_conn);
}

void WindowServer::setWmState(Client *c, uint32_t state)
{
    const uint32_t data[2] = {state, XCB_NONE};
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, c->window, atom("WM_STATE"), atom("WM_STATE"), 32, 2, data);
}

void WindowServer::setNetState(Client *c)
{
    QVector<uint32_t> states = c->keptStates;
    if (c->minimized)
        states << atom("_NET_WM_STATE_HIDDEN");
    if (c->fullscreen)
        states << atom("_NET_WM_STATE_FULLSCREEN");
    if (c->zoomed)
        states << atom("_NET_WM_STATE_MAXIMIZED_VERT") << atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, c->window, atom("_NET_WM_STATE"), XCB_ATOM_ATOM, 32, states.size(), states.constData());
}

QRect WindowServer::frameRect(const Client *c) const
{
    const int th = c->titleHeight();
    return QRect(c->geometry.x(), c->geometry.y() - th, c->geometry.width(), c->geometry.height() + th);
}

// Where the Dock shows the window (_NET_WM_ICON_GEOMETRY, which the Dock keeps on every window: its
// minimized tile, or its application's), inset by 4 as 10.6's Dock does; the Dock's middle otherwise
QRect WindowServer::iconGeometry(const Client *c) const
{
    const QVector<uint32_t> g = X11::cardinals(c->window, atom("_NET_WM_ICON_GEOMETRY"), XCB_ATOM_CARDINAL);
    if (g.size() >= 4 && g[2] > 8 && g[3] > 8)
        return QRect(int(g[0]), int(g[1]), int(g[2]), int(g[3])).adjusted(4, 4, -4, -4);
    return QRect(m_screen.center().x() - 24, m_screen.bottom() - 56, 48, 48);
}

// The Dock's settings (dock.conf, com.apple.dock's keys): mineffect genie or scale. 10.6's Dock
// takes genie-speed (0.5 s) for the genie, 0.25 s for the scale, ten times longer with Shift.
Compositor::Effect WindowServer::minimizeEffect(double *seconds) const
{
    const QSettings dock(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/dock.conf",
                         QSettings::IniFormat);
    const bool scale = dock.value("mineffect", "genie").toString() == "scale";
    *seconds = scale ? 0.25 : qBound(0.05, dock.value("genie-speed", 0.5).toDouble(), 5.0);
    if (auto *r = xcb_query_pointer_reply(m_conn, xcb_query_pointer(m_conn, m_root), nullptr)) {
        if (r->mask & XCB_MOD_MASK_SHIFT)
            *seconds *= 10;
        free(r);
    }
    return scale ? Compositor::Scale : Compositor::Genie;
}

// The window's picture for its tile in the Dock (_SNOWLEOPARD_WINDOW_PREVIEW: width, height, ARGB
// pixels, as _NET_WM_ICON), 256 pixels at most each way
void WindowServer::publishPreview(Client *c, QImage image)
{
    if (image.isNull()) {
        // nothing composites: what the server shows of the frame
        const QRect f = frameRect(c);
        if (auto *r = xcb_get_image_reply(m_conn, xcb_get_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, c->frame, 0, 0, uint16_t(f.width()),
                                                                uint16_t(f.height()), ~0u),
                                          nullptr)) {
            if (r->depth >= 24 && xcb_get_image_data_length(r) >= f.width() * f.height() * 4)
                image = QImage(xcb_get_image_data(r), f.width(), f.height(), f.width() * 4,
                               r->depth == 32 ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32).copy();
            free(r);
        }
    }
    if (image.isNull())
        return;
    if (image.width() > 256 || image.height() > 256)
        image = image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_ARGB32);
    QVector<uint32_t> data{uint32_t(image.width()), uint32_t(image.height())};
    data.reserve(2 + image.width() * image.height());
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const uint32_t *>(image.constScanLine(y));
        data.append(QVector<uint32_t>(line, line + image.width()));
    }
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, c->window, atom("_SNOWLEOPARD_WINDOW_PREVIEW"), XCB_ATOM_CARDINAL, 32,
                        uint32_t(data.size()), data.constData());
}

void WindowServer::publishCapture(Client *c, bool shadow, xcb_window_t requestor)
{
    if (!requestor)
        return;
    QPoint offset;
    const QImage image = m_compositor.capture(c->frame, shadow, &offset).convertToFormat(QImage::Format_ARGB32);
    // width, height, the frame's place in it, then the pixels; appended in pieces (a request's longest)
    const uint32_t head[4] = {uint32_t(image.width()), uint32_t(image.height()), uint32_t(offset.x()), uint32_t(offset.y())};
    const xcb_atom_t prop = atom("_SNOWLEOPARD_WINDOW_CAPTURE");
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, requestor, prop, XCB_ATOM_CARDINAL, 32, 4, head);
    const int rowsAtOnce = qMax(1, 65536 / qMax(1, image.width()));
    for (int y = 0; y < image.height(); y += rowsAtOnce) {
        const int n = qMin(rowsAtOnce, image.height() - y);
        xcb_change_property(m_conn, XCB_PROP_MODE_APPEND, requestor, prop, XCB_ATOM_CARDINAL, 32, uint32_t(image.width() * n),
                            image.constScanLine(y));
    }
    // the end: a last empty append the requestor waits for (_SNOWLEOPARD_WINDOW_CAPTURE_DONE)
    const uint32_t done = 1;
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, requestor, atom("_SNOWLEOPARD_WINDOW_CAPTURE_DONE"), XCB_ATOM_CARDINAL, 32, 1,
                        &done);
}

void WindowServer::minimize(Client *c, bool hide)
{
    if (c->minimized)
        return;
    c->minimized = true;
    c->hidden = hide;
    if (hide) {   // hidden (⌘H): gone at once, no tile in the Dock
        const uint32_t one = 1;
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, c->window, atom("_SNOWLEOPARD_HIDDEN"), XCB_ATOM_CARDINAL, 32, 1, &one);
    } else {      // its picture first, while it's shown: for the Dock's tile and the genie
        publishPreview(c, m_compositor.snapshot(c->window, c->frame));
    }
    xcb_unmap_window(m_conn, c->frame);   // the client stays mapped inside: no UnmapNotify from it
    setWmState(c, ICCCM_IconicState);
    setNetState(c);
    if (m_active == c) {
        m_active = nullptr;
        for (int i = m_clients.size() - 1; i >= 0; --i)
            if (!m_clients[i]->minimized && onCurrentSpace(m_clients[i])) {
                focus(m_clients[i]);
                break;
            }
        if (!m_active)
            setActiveProperty(0);
    }
    updateClientLists();
    if (hide)
        return;
    // into the tile the Dock makes for it: the animation waits for the Dock to say where
    double seconds = 0.5;
    const Compositor::Effect effect = minimizeEffect(&seconds);
    m_compositor.animate(c->window, c->frame, frameRect(c), iconGeometry(c), effect, true, seconds, true);
}

void WindowServer::unminimize(Client *c)
{
    if (!c->minimized)
        return;
    c->minimized = false;
    if (c->hidden) {
        c->hidden = false;
        xcb_delete_property(m_conn, c->window, atom("_SNOWLEOPARD_HIDDEN"));
    } else {   // out of its tile
        double seconds = 0.5;
        const Compositor::Effect effect = minimizeEffect(&seconds);
        m_compositor.animate(c->window, c->frame, frameRect(c), iconGeometry(c), effect, false, seconds, false);
    }
    if (onCurrentSpace(c))
        xcb_map_window(m_conn, c->frame);
    setWmState(c, ICCCM_NormalState);
    setNetState(c);
    xcb_delete_property(m_conn, c->window, atom("_SNOWLEOPARD_WINDOW_PREVIEW"));
    updateClientLists();   // the Dock takes its tile away
}

void WindowServer::toggleZoom(Client *c)
{
    // 10.6's zoom goes between the application's size and the largest: here, the work area's
    if (c->zoomed) {
        c->geometry = c->restore;
        c->zoomed = false;
    } else {
        c->restore = c->geometry;
        c->geometry = QRect(m_workArea.left(), m_workArea.top() + c->titleHeight(), m_workArea.width(),
                            m_workArea.height() - c->titleHeight());
        c->zoomed = true;
    }
    applyGeometry(c);
    paintTitle(c);
    setNetState(c);
}

void WindowServer::setFullscreen(Client *c, bool on)
{
    if (c->fullscreen == on)
        return;
    if (on) {
        if (!c->zoomed)
            c->restore = c->geometry;
        c->fullscreen = true;
        c->geometry = m_screen;
    } else {
        c->fullscreen = false;
        c->geometry = c->restore;
    }
    applyGeometry(c);
    paintTitle(c);
    setNetState(c);
    raise(c);
}

void WindowServer::close(Client *c)
{
    if (c->deleteWindow) {
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = c->window;
        ev.type = atom("WM_PROTOCOLS");
        ev.data.data32[0] = atom("WM_DELETE_WINDOW");
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(m_conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
    } else {
        xcb_kill_client(m_conn, c->window);
    }
    xcb_flush(m_conn);
}

void WindowServer::updateClientLists()
{
    QVector<uint32_t> stacking, mapping;
    for (Client *c : std::as_const(m_clients))
        stacking.append(c->window);
    mapping = stacking;
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW, 32, mapping.size(), mapping.constData());
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW, 32, stacking.size(),
                        stacking.constData());
    xcb_flush(m_conn);
}

void WindowServer::updateWorkArea()
{
    int top = 0, bottom = 0, left = 0, right = 0;
    for (xcb_window_t d : std::as_const(m_docks)) {
        QVector<uint32_t> s = X11::cardinals(d, atom("_NET_WM_STRUT_PARTIAL"), XCB_ATOM_CARDINAL);
        if (s.size() < 4)
            s = X11::cardinals(d, atom("_NET_WM_STRUT"), XCB_ATOM_CARDINAL);
        if (s.size() < 4)
            continue;
        left = qMax(left, int(s[0])), right = qMax(right, int(s[1])), top = qMax(top, int(s[2])), bottom = qMax(bottom, int(s[3]));
    }
    m_workArea = m_screen.adjusted(left, top, -right, -bottom);
    const uint32_t area[4] = {uint32_t(m_workArea.x()), uint32_t(m_workArea.y()), uint32_t(m_workArea.width()), uint32_t(m_workArea.height())};
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_WORKAREA"), XCB_ATOM_CARDINAL, 32, 4, area);
}

// ---- Command (Super) keys, as on the Mac: W closes, M minimizes, H hides, Q quits, Tab goes round

static const xcb_keysym_t keyW = 0x77, keyM = 0x6d, keyH = 0x68, keyQ = 0x71, keyTab = 0xff09, key3 = 0x33, key4 = 0x34;

// The symbolic hot keys (CGSSetSymbolicHotKey) the window server keeps for whoever registered them:
// 28 ⇧⌘3, 29 ⌃⇧⌘3, 30 ⇧⌘4, 31 ⌃⇧⌘4, the screen shots' (SystemUIServer's ScreenCapture). A program
// registers by the root's _SNOWLEOPARD_SYMBOLIC_HOTKEYS (pairs: hot key, window); the key pressed, its
// window gets a _SNOWLEOPARD_HOTKEY message with the hot key's number, as the Mac's server sends the
// registering connection its hot key event
static int symbolicHotKey(xcb_keysym_t sym, uint16_t state)
{
    if (!(state & XCB_MOD_MASK_SHIFT) || (sym != key3 && sym != key4))
        return -1;
    return (sym == key3 ? 28 : 30) + ((state & XCB_MOD_MASK_CONTROL) ? 1 : 0);
}

void WindowServer::grabKeys()
{
    if (!m_keySymbols)
        return;
    for (xcb_keysym_t sym : {keyW, keyM, keyH, keyQ, keyTab, key3, key4}) {
        xcb_keycode_t *codes = xcb_key_symbols_get_keycode(m_keySymbols, sym);
        if (!codes)
            continue;
        for (xcb_keycode_t *k = codes; *k; ++k)
            // with and without Caps Lock and Num Lock
            for (uint16_t extra : {uint16_t(0), uint16_t(XCB_MOD_MASK_LOCK), uint16_t(XCB_MOD_MASK_2), uint16_t(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)})
            {
                if (sym == key3 || sym == key4) {   // the screen shots' hot keys only
                    for (uint16_t shot : {uint16_t(XCB_MOD_MASK_SHIFT), uint16_t(XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL)})
                        xcb_grab_key(m_conn, 1, m_root, uint16_t(XCB_MOD_MASK_4 | shot | extra), *k, XCB_GRAB_MODE_ASYNC,
                                     XCB_GRAB_MODE_ASYNC);
                    continue;
                }
                xcb_grab_key(m_conn, 1, m_root, uint16_t(XCB_MOD_MASK_4 | extra), *k, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
                // ⇧⌘Q Log Out, ⌥⇧⌘Q at once (the Apple menu's: loginwindow's hot keys on the Mac)
                if (sym == keyQ)
                    for (uint16_t log : {uint16_t(XCB_MOD_MASK_SHIFT), uint16_t(XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_1)})
                        xcb_grab_key(m_conn, 1, m_root, uint16_t(XCB_MOD_MASK_4 | log | extra), *k, XCB_GRAB_MODE_ASYNC,
                                     XCB_GRAB_MODE_ASYNC);
            }
        free(codes);
    }
}

xcb_window_t WindowServer::frontAppKeyboard()
{
    const QVector<uint32_t> front = X11::cardinals(m_root, atom("_SNOWLEOPARD_FRONT_APP"), XCB_ATOM_CARDINAL);
    if (front.size() < 3 || !front[2])
        return 0;
    xcb_window_t focused = 0;
    if (auto *r = xcb_get_input_focus_reply(m_conn, xcb_get_input_focus(m_conn), nullptr)) {
        focused = r->focus;
        free(r);
    }
    return focused == front[2] ? front[2] : 0;
}

void WindowServer::handleKey(xcb_key_press_event_t *e)
{
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(m_keySymbols, e->detail, 0);
    if (spacesKey(sym, e->state))
        return;
    if (const int hotKey = symbolicHotKey(sym, e->state); hotKey >= 0) {
        const QVector<uint32_t> owners = X11::cardinals(m_root, atom("_SNOWLEOPARD_SYMBOLIC_HOTKEYS"), XCB_ATOM_CARDINAL);
        for (int i = 0; i + 1 < owners.size(); i += 2)
            if (int(owners[i]) == hotKey) {
                xcb_client_message_event_t ev = {};
                ev.response_type = XCB_CLIENT_MESSAGE;
                ev.format = 32;
                ev.window = owners[i + 1];
                ev.type = atom("_SNOWLEOPARD_HOTKEY");
                ev.data.data32[0] = uint32_t(hotKey);
                ev.data.data32[1] = e->time;
                xcb_send_event(m_conn, 0, owners[i + 1], XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
                xcb_flush(m_conn);
                break;
            }
        return;
    }
    if (sym == keyQ && (e->state & XCB_MOD_MASK_SHIFT)) {
        Session::logOut(!(e->state & XCB_MOD_MASK_1));
        return;
    }
    Client *c = m_active;
    auto sameApplication = [](Client *a, Client *b) { return X11::wmClass(a->window) == X11::wmClass(b->window); };
    if (sym == keyTab) {
        // the next window down, brought to the front
        for (int i = m_clients.size() - 2; i >= 0; --i)
            if (!m_clients[i]->minimized) {
                Client *next = m_clients[i];
                activateSpaceFor(next);
                raise(next);
                focus(next);
                break;
            }
        return;
    }
    // an application in front without a window has the keyboard (its window off the screen, the root's
    // _SNOWLEOPARD_FRONT_APP's third value): ⌘Q, ⌘H... are its menus'
    if (const xcb_window_t keys = frontAppKeyboard()) {
        xcb_key_press_event_t k = *e;
        k.event = keys;
        k.child = XCB_NONE;
        k.same_screen = 1;
        xcb_send_event(m_conn, 0, keys, XCB_EVENT_MASK_KEY_PRESS, reinterpret_cast<const char *>(&k));
        xcb_flush(m_conn);
        return;
    }
    if (!c)
        return;
    // a key the application's own menus answer (_SNOWLEOPARD_KEY_EQUIVALENTS): given to its window
    if (X11::cardinals(c->window, atom("_SNOWLEOPARD_KEY_EQUIVALENTS"), XCB_ATOM_CARDINAL).contains(sym)) {
        xcb_key_press_event_t k = *e;
        k.event = c->window;
        k.child = XCB_NONE;
        k.same_screen = 1;
        xcb_send_event(m_conn, 0, c->window, XCB_EVENT_MASK_KEY_PRESS, reinterpret_cast<const char *>(&k));
        xcb_flush(m_conn);
        return;
    }
    if (sym == keyW)
        close(c);
    else if (sym == keyM)
        minimize(c);
    else if (sym == keyH) {
        for (Client *o : QVector<Client *>(m_clients))
            if (sameApplication(o, c))
                minimize(o, true);
    } else if (sym == keyQ) {
        for (Client *o : QVector<Client *>(m_clients))
            if (sameApplication(o, c))
                close(o);
    }
}

// ---- events

bool WindowServer::nativeEventFilter(const QByteArray &type, void *message, qintptr *)
{
    if (type != "xcb_generic_event_t")
        return false;
    auto *ev = static_cast<xcb_generic_event_t *>(message);
    m_compositor.handleEvent(ev);
    switch (ev->response_type & ~0x80) {
    case XCB_MAP_REQUEST: {
        auto *e = reinterpret_cast<xcb_map_request_event_t *>(ev);
        if (Client *c = find(e->window)) {
            unminimize(c);
            raise(c);
            focus(c);
        } else {
            manage(e->window, false);
        }
        return true;
    }
    case XCB_CONFIGURE_REQUEST:
        configureRequest(reinterpret_cast<xcb_configure_request_event_t *>(ev));
        return true;
    case XCB_UNMAP_NOTIFY: {
        auto *e = reinterpret_cast<xcb_unmap_notify_event_t *>(ev);
        if (Client *c = find(e->window); c && e->window == c->window) {
            if (c->ignoreUnmaps > 0)
                c->ignoreUnmaps--;
            else
                unmanage(c, false);
        } else if (m_docks.removeAll(e->window)) {
            updateWorkArea();
        } else {
            m_desktops.removeAll(e->window);
        }
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        auto *e = reinterpret_cast<xcb_destroy_notify_event_t *>(ev);
        if (Client *c = find(e->window); c && e->window == c->window)
            unmanage(c, true);
        else if (m_docks.removeAll(e->window))
            updateWorkArea();
        else
            m_desktops.removeAll(e->window);
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        auto *e = reinterpret_cast<xcb_property_notify_event_t *>(ev);
        // an application came in front without a window and took the keyboard: no window is key
        if (e->window == m_root && e->atom == atom("_SNOWLEOPARD_FRONT_APP") && m_active && frontAppKeyboard()) {
            Client *old = m_active;
            m_active = nullptr;
            setActiveProperty(0);
            paintTitle(old);
            m_compositor.damageAll();
            xcb_flush(m_conn);
        }
        if (Client *c = find(e->window); c && e->window == c->window) {
            if (e->atom == XCB_ATOM_WM_NAME || e->atom == atom("_NET_WM_NAME")) {
                readProperties(c);
                paintTitle(c);
            } else if (e->atom == atom("_SNOWLEOPARD_UNIFIED_TOOLBAR") || e->atom == atom("_SNOWLEOPARD_PROXY_ICON") ||
                       e->atom == atom("_SNOWLEOPARD_DOCUMENT_EDITED")) {
                readProperties(c);
                paintTitle(c);
            } else if (e->atom == atom("_SNOWLEOPARD_ROUNDED_BOTTOM")) {
                readProperties(c);
                applyShape(c);
            } else if (e->atom == XCB_ATOM_WM_NORMAL_HINTS) {
                readSizeHints(c);   // a window whose limits change (iTunes' mini player)
            } else if (e->atom == atom("WM_PROTOCOLS") || e->atom == XCB_ATOM_WM_HINTS) {
                readProperties(c);
            } else if (e->atom == atom("_NET_WM_WINDOW_OPACITY")) {
                readProperties(c);
                m_compositor.setOpacity(c->frame, c->opacity);
            } else if (e->atom == atom("_NET_WM_ICON_GEOMETRY") && c->minimized) {
                m_compositor.retarget(c->window, iconGeometry(c));   // the Dock made its tile
            }
        } else if (m_docks.contains(e->window) && (e->atom == atom("_NET_WM_STRUT_PARTIAL") || e->atom == atom("_NET_WM_STRUT"))) {
            updateWorkArea();
        }
        break;
    }
    case XCB_EXPOSE: {
        auto *e = reinterpret_cast<xcb_expose_event_t *>(ev);
        if (Client *c = find(e->window); c && e->window == c->frame && e->count == 0)
            paintTitle(c);
        break;
    }
    case XCB_BUTTON_PRESS:
        buttonPress(reinterpret_cast<xcb_button_press_event_t *>(ev));
        break;
    case XCB_BUTTON_RELEASE:
        buttonRelease(reinterpret_cast<xcb_button_release_event_t *>(ev));
        break;
    case XCB_MOTION_NOTIFY:
        motion(reinterpret_cast<xcb_motion_notify_event_t *>(ev));
        break;
    case XCB_LEAVE_NOTIFY: {
        auto *e = reinterpret_cast<xcb_leave_notify_event_t *>(ev);
        if (Client *c = find(e->event); c && e->event == c->frame && c->overButtons && m_drag == NoDrag) {
            c->overButtons = false;
            c->hover = None;
            paintTitle(c);
        }
        break;
    }
    case XCB_CLIENT_MESSAGE:
        clientMessage(reinterpret_cast<xcb_client_message_event_t *>(ev));
        break;
    case XCB_KEY_PRESS:
        handleKey(reinterpret_cast<xcb_key_press_event_t *>(ev));
        return true;
    case XCB_MAPPING_NOTIFY:
        if (m_keySymbols) {
            xcb_refresh_keyboard_mapping(m_keySymbols, reinterpret_cast<xcb_mapping_notify_event_t *>(ev));
            xcb_ungrab_key(m_conn, XCB_GRAB_ANY, m_root, XCB_MOD_MASK_ANY);
            grabKeys();
        }
        break;
    default:
        break;
    }
    return false;
}

void WindowServer::configureRequest(xcb_configure_request_event_t *e)
{
    Client *c = find(e->window);
    if (!c || e->window != c->window) {
        // not ours: as asked
        QVector<uint32_t> values;
        uint16_t mask = 0;
        auto add = [&](uint16_t bit, uint32_t v) {
            if (e->value_mask & bit)
                mask |= bit, values.append(v);
        };
        add(XCB_CONFIG_WINDOW_X, uint32_t(e->x));
        add(XCB_CONFIG_WINDOW_Y, uint32_t(e->y));
        add(XCB_CONFIG_WINDOW_WIDTH, e->width);
        add(XCB_CONFIG_WINDOW_HEIGHT, e->height);
        add(XCB_CONFIG_WINDOW_BORDER_WIDTH, e->border_width);
        add(XCB_CONFIG_WINDOW_SIBLING, e->sibling);
        add(XCB_CONFIG_WINDOW_STACK_MODE, e->stack_mode);
        xcb_configure_window(m_conn, e->window, mask, values.constData());
        if (m_docks.contains(e->window) || m_desktops.contains(e->window))
            restackLayers();
        return;
    }
    if (c->fullscreen) {
        sendConfigureNotify(c);
        return;
    }
    if (e->value_mask & XCB_CONFIG_WINDOW_X)
        c->geometry.moveLeft(e->x);
    if (e->value_mask & XCB_CONFIG_WINDOW_Y)
        c->geometry.moveTop(qMax(int(e->y), m_workArea.top() + c->titleHeight()));
    if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
        c->geometry.setWidth(e->width);
    if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
        c->geometry.setHeight(e->height);
    applyGeometry(c);
    if ((e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) && e->stack_mode == XCB_STACK_MODE_ABOVE)
        raise(c);
    paintTitle(c);
}

void WindowServer::buttonPress(xcb_button_press_event_t *e)
{
    Client *c = find(e->event);
    if (!c) {
        xcb_allow_events(m_conn, XCB_ALLOW_REPLAY_POINTER, e->time);
        return;
    }
    if (e->event == c->window) {
        // the click to focus: ours, then the application's
        if (m_active != c)
            focus(c);
        raise(c);
        xcb_allow_events(m_conn, XCB_ALLOW_REPLAY_POINTER, e->time);
        xcb_flush(m_conn);
        return;
    }
    if (e->detail != XCB_BUTTON_INDEX_1)
        return;
    if (m_active != c)
        focus(c);
    raise(c);
    m_dragClient = c;
    m_dragStart = QPoint(e->root_x, e->root_y);
    m_dragGeometry = c->geometry;
    if (e->event == c->grip) {
        m_drag = Resizing;
    } else if (int b = buttonAt(c, QPoint(e->event_x, e->event_y)); b != None) {
        m_drag = ButtonDown;
        c->pressed = c->hover = b;
        paintTitle(c);
    } else {
        // a second click on the title bar within the double-click time: minimize, as 10.6 does
        if (m_lastTitleWindow == c->frame && m_lastTitleClick.isValid() && m_lastTitleClick.elapsed() < 400) {
            m_lastTitleClick.invalidate();
            m_drag = NoDrag;
            minimize(c);
            return;
        }
        m_lastTitleClick.start();
        m_lastTitleWindow = c->frame;
        m_drag = Moving;
    }
}

void WindowServer::motion(xcb_motion_notify_event_t *e)
{
    if (m_drag == NoDrag) {
        // the buttons' glyphs while the pointer is over them
        Client *c = find(e->event);
        if (c && e->event == c->frame) {
            bool over = buttonRect(c, Close).united(buttonRect(c, Zoom)).contains(e->event_x, e->event_y);
            if (over != c->overButtons) {
                c->overButtons = over;
                paintTitle(c);
            }
        }
        return;
    }
    Client *c = m_dragClient;
    if (!c)
        return;
    QPoint delta = QPoint(e->root_x, e->root_y) - m_dragStart;
    if (m_drag == Moving) {
        c->geometry.moveTopLeft(m_dragGeometry.topLeft() + delta);
        if (c->geometry.top() - c->titleHeight() < m_workArea.top())   // never under the menu bar
            c->geometry.moveTop(m_workArea.top() + c->titleHeight());
        const uint32_t pos[] = {uint32_t(c->geometry.x()), uint32_t(c->geometry.y() - c->titleHeight())};
        xcb_configure_window(m_conn, c->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
        sendConfigureNotify(c);
        xcb_flush(m_conn);
    } else if (m_drag == Resizing) {
        QSize minimum = c->minimum.expandedTo(QSize(Frame::MinimumWidth, 40));
        QSize size(qMax(minimum.width(), m_dragGeometry.width() + delta.x()), qMax(minimum.height(), m_dragGeometry.height() + delta.y()));
        if (!c->maximum.isEmpty())
            size = size.boundedTo(c->maximum.expandedTo(minimum));
        c->geometry.setSize(size);
        c->zoomed = false;
        applyGeometry(c);
        paintTitle(c);
        xcb_flush(m_conn);
    } else if (m_drag == ButtonDown) {
        // pressed only while the pointer stays on the button it went down on (c->hover)
        QPoint local(e->root_x - c->geometry.x(), e->root_y - c->geometry.y() + c->titleHeight());
        int now = buttonAt(c, local) == c->hover ? c->hover : int(None);
        if (now != c->pressed) {
            c->pressed = now;
            paintTitle(c);
        }
    }
}

void WindowServer::buttonRelease(xcb_button_release_event_t *e)
{
    Client *c = m_dragClient;
    Drag drag = m_drag;
    m_drag = NoDrag;
    m_dragClient = nullptr;
    if (!c || drag != ButtonDown)
        return;
    int button = c->pressed;
    c->pressed = None;
    c->hover = None;
    paintTitle(c);
    Q_UNUSED(e);
    if (button == Close)
        close(c);
    else if (button == Minimize)
        minimize(c);
    else if (button == Zoom)
        toggleZoom(c);
    else if (button == ToolbarButton) {
        // the application shows or hides its toolbar ([NSWindow toggleToolbarShown:])
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = c->window;
        ev.type = atom("_SNOWLEOPARD_TOGGLE_TOOLBAR");
        xcb_send_event(m_conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
        xcb_flush(m_conn);
    }
}

void WindowServer::clientMessage(xcb_client_message_event_t *e)
{
    Client *c = find(e->window);
    if (e->type == atom("_NET_CURRENT_DESKTOP") && e->window == m_root) {
        // a pager's, the Spaces menu extra's (com.apple.switchSpaces)
        if (m_spacesEnabled)
            switchSpace(int(e->data.data32[0]));
        xcb_flush(m_conn);
        return;
    }
    if (e->type == atom("_NET_ACTIVE_WINDOW")) {
        if (c) {
            activateSpaceFor(c);
            unminimize(c);
            raise(c);
            focus(c);
        } else if (m_desktops.contains(e->window)) {
            // the desktop: the Finder becomes the active application
            xcb_set_input_focus(m_conn, XCB_INPUT_FOCUS_POINTER_ROOT, e->window, XCB_CURRENT_TIME);
            Client *old = m_active;
            m_active = nullptr;
            setActiveProperty(e->window);
            if (old)
                paintTitle(old);
        }
    } else if (!c) {
        return;
    } else if (e->type == atom("_NET_CLOSE_WINDOW")) {
        close(c);
    } else if (e->type == atom("_NET_WM_DESKTOP")) {
        // to another space, or on all of them
        c->allSpaces = e->data.data32[0] == 0xFFFFFFFF;
        setClientSpace(c, c->allSpaces ? c->space : int(e->data.data32[0]));
    } else if (e->type == atom("_SNOWLEOPARD_CAPTURE_WINDOW")) {
        // screencapture's window mode (CGSHWCaptureWindowList): data 0, 1 its shadow too; data 1, the
        // window that wants the picture, in its _SNOWLEOPARD_WINDOW_CAPTURE
        publishCapture(c, e->data.data32[0] & 1, e->data.data32[1]);
    } else if (e->type == atom("WM_CHANGE_STATE") && e->data.data32[0] == ICCCM_IconicState) {
        minimize(c);
    } else if (e->type == atom("_SNOWLEOPARD_HIDE")) {
        minimize(c, true);
    } else if (e->type == atom("_NET_WM_STATE")) {
        // data: action (0 remove, 1 add, 2 toggle), two properties
        const uint32_t action = e->data.data32[0];
        for (int i = 1; i <= 2; ++i) {
            const uint32_t prop = e->data.data32[i];
            if (prop == atom("_NET_WM_STATE_FULLSCREEN"))
                setFullscreen(c, action == 2 ? !c->fullscreen : action == 1);
            else if (prop == atom("_NET_WM_STATE_MAXIMIZED_VERT") && i == 1) {
                bool want = action == 2 ? !c->zoomed : action == 1;
                if (want != c->zoomed)
                    toggleZoom(c);
            } else if (prop == atom("_NET_WM_STATE_HIDDEN") && action == 1) {
                minimize(c);
            } else if (prop == atom("_NET_WM_STATE_ABOVE")) {
                // -[NSWindow setLevel:] to the floating level and back (a Stickies note's Floating Window)
                const bool want = action == 2 ? !c->floating : action == 1;
                c->floating = want;
                c->keptStates.removeAll(prop);
                if (want)
                    c->keptStates << prop;
                setNetState(c);
                if (want)
                    raise(c);
                else
                    restackLayers();
            }
        }
    }
    xcb_flush(m_conn);
}
