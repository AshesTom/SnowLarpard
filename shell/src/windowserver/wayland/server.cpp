// SPDX-License-Identifier: LGPL-2.1-or-later
#include "server.h"

#include "desktopsettings.h"
#include "effects.h"
#include "imageitem.h"
#include "layershell.h"
#include "session.h"
#include "shellprotocol.h"
#include "window.h"
#include "xdgshell.h"
#include "xwayland.h"

#include <QGuiApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QKeySequence>
#include <QMouseEvent>
#include <QQuickItemGrabResult>
#include <QSettings>
#include <QStandardPaths>
#include <QWaylandClient>
#include <QWaylandKeymap>
#include <QWaylandQuickCompositor>
#include <QWaylandQuickItem>
#include <QWaylandQuickOutput>
#include <QWaylandQuickXdgOutputV1>
#include <QWaylandSeat>
#include <QWaylandSurface>
#include <QWaylandViewporter>
#include <QWaylandXdgOutputManagerV1>
#include <algorithm>

namespace {
// keysyms of what Qt::Key names, for the key equivalents (lower case letters and digits are their
// Latin-1 code; the rest as X11 has them)
uint32_t keysymOf(const QKeyEvent *e)
{
    const int k = e->key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z)
        return uint32_t(k - Qt::Key_A + 'a');
    if (k >= 0x20 && k <= 0xff)
        return uint32_t(k);
    if (e->nativeVirtualKey())
        return e->nativeVirtualKey();
    return 0;
}
} // namespace

Server::Server(QObject *parent) : QObject(parent) {}

Server::~Server()
{
    delete m_xwayland;
    qDeleteAll(m_windows);
    m_windows.clear();
    delete m_window;
}

QWaylandCompositor *Server::compositor() const { return m_compositor; }
QWaylandSeat *Server::seat() const { return m_compositor ? m_compositor->defaultSeat() : nullptr; }
QString Server::socketName() const { return m_compositor ? QString::fromUtf8(m_compositor->socketName()) : QString(); }
QRect Server::screen() const { return QRect(QPoint(0, 0), m_window ? m_window->size() : QSize()); }

bool Server::start(const QString &socketName, bool fullScreen, const QSize &size)
{
    m_window = new QQuickWindow;
    m_window->setTitle(QStringLiteral("WindowServer"));
    m_window->setColor(Qt::black);
    m_window->resize(size.isValid() ? size : QSize(1280, 800));

    m_compositor = new QWaylandQuickCompositor(this);
    m_compositor->setSocketName(socketName.toUtf8());
    m_output = new QWaylandQuickOutput(m_compositor, m_window);
    m_output->setSizeFollowsWindow(true);
    m_output->setManufacturer(QStringLiteral("Apple"));
    m_output->setModel(QStringLiteral("Snow Leopard"));
    auto *xdgOutputs = new QWaylandXdgOutputManagerV1(m_compositor);
    auto *xdgOutput = new QWaylandXdgOutputV1(m_output, xdgOutputs);
    xdgOutput->setName(QStringLiteral("SL-1"));
    xdgOutput->setDescription(QStringLiteral("WindowServer"));
    m_xdgOutput = xdgOutput;
    new QWaylandViewporter(m_compositor);
    m_compositor->create();
    if (!m_compositor->isCreated())
        return false;

    // the keyboard: the session's layout (snowleopard-session reads it from Plasma's kxkbrc or localectl)
    if (QWaylandKeymap *k = seat()->keymap()) {
        k->setLayout(qEnvironmentVariable("XKB_DEFAULT_LAYOUT"));
        k->setVariant(qEnvironmentVariable("XKB_DEFAULT_VARIANT"));
        k->setModel(qEnvironmentVariable("XKB_DEFAULT_MODEL"));
        k->setOptions(qEnvironmentVariable("XKB_DEFAULT_OPTIONS"));
    }

    // the levels, bottom to top
    m_root = m_window->contentItem();
    for (int l = 0; l < Levels; ++l) {
        m_levels[l] = new QQuickItem(m_root);
        m_levels[l]->setZ(l);
    }
    auto resize = [this] {
        m_xdgOutput->setLogicalPosition(QPoint(0, 0));
        m_xdgOutput->setLogicalSize(m_window->size());
        for (QQuickItem *l : m_levels)
            l->setSize(m_window->size());
        paintDesktopPicture();
        arrangeLayers();
    };
    connect(m_window, &QWindow::widthChanged, this, resize);
    connect(m_window, &QWindow::heightChanged, this, resize);

    m_xdgShell = new XdgShell(this);
    connect(m_xdgShell, &XdgShell::toplevelCreated, this, &Server::toplevelCreated);
    connect(m_xdgShell, &XdgShell::popupCreated, this, &Server::popupCreated);
    new XdgDecorationManager(m_compositor->display());
    m_layerShell = new LayerShell(this);
    m_shell = new ShellProtocol(this);
    new AppMenuManager(this, m_shell);
    m_effects = new Effects(this);

    connect(seat(), &QWaylandSeat::cursorSurfaceRequested, this,
            [this](QWaylandSurface *s, int x, int y, QWaylandClient *) { handleCursor(s, x, y); });
    m_window->installEventFilter(this);

    resize();
    if (fullScreen)
        m_window->showFullScreen();
    else
        m_window->show();
    resize();
    // the X11 programs' server, when one asks for it (xwayland.cpp)
    m_xwayland = new Xwayland(this);
    return true;
}

QQuickItem *Server::layerItem(uint32_t layer) const
{
    switch (layer) {
    case LayerSurface::Background: return m_levels[LayerBackground];
    case LayerSurface::Bottom: return m_levels[LayerBottom];
    case LayerSurface::Overlay: return m_levels[LayerOverlay];
    default: return m_levels[LayerTop];
    }
}

QWaylandQuickItem *Server::createSurfaceItem(QWaylandSurface *surface, QQuickItem *parent)
{
    auto *item = new QWaylandQuickItem;
    item->setParentItem(parent);
    item->setOutput(m_output);
    item->setSurface(surface);
    item->setFocusOnClick(true);
    return item;
}

void Server::paintDesktopPicture()
{
    // what shows until the Finder's desktop covers it (WindowServer's root background on X11)
    auto *item = static_cast<ImageItem *>(m_levels[DesktopPicture]->childItems().value(0));
    if (!item) {
        item = new ImageItem(m_levels[DesktopPicture]);
        item->setSmooth(true);
    }
    const QSize s = m_window->size();
    if (s.isEmpty() || item->size() == QSizeF(s))
        return;
    item->setImage(Desktop::render(Desktop::load(), s).convertToFormat(QImage::Format_RGB32));
    item->setSize(s);
}

// ---- the layers

void Server::addLayerSurface(LayerSurface *l) { m_layers.append(l); }

void Server::removeLayerSurface(LayerSurface *l)
{
    m_layers.removeAll(l);
    arrangeLayers();
}

LayerSurface *Server::layerSurface(const QString &nameSpace) const
{
    for (LayerSurface *l : m_layers)
        if (l->nameSpace() == nameSpace)
            return l;
    return nullptr;
}

void Server::arrangeLayers()
{
    const QRect s = screen();
    QRect usable = s;
    // those with an exclusive zone first (the menu bar, the Dock), then the others in what is left
    for (bool exclusive : {true, false})
        for (LayerSurface *l : std::as_const(m_layers))
            if ((l->state().exclusiveZone > 0) == exclusive)
                l->arrange(s, usable);
    // the work area: the zones of those shown
    QRect area = s;
    for (LayerSurface *l : std::as_const(m_layers))
        if (l->mapped())
            l->takeZone(s, area);
    if (area != m_workArea) {
        m_workArea = area;
        emit workAreaChanged();
    }
}

// ---- the windows

Window *Server::window(uint32_t id) const
{
    for (Window *w : m_windows)
        if (w->id() == id)
            return w;
    return nullptr;
}

Window *Server::windowAt(const QPointF &p) const
{
    for (int i = int(m_windows.size()) - 1; i >= 0; --i) {
        Window *w = m_windows[i];
        if (!w->minimized() && w->frameGeometry().contains(p.toPoint()))
            return w;
    }
    return nullptr;
}

void Server::toplevelCreated(XdgToplevel *t)
{
    QWaylandSurface *s = t->xdgSurface()->surface();
    QPointer<XdgToplevel> toplevel(t);
    connect(s, &QWaylandSurface::hasContentChanged, t, [this, toplevel, s] {
        if (!toplevel)
            return;
        if (s->hasContent() && !toplevel->window) {
            auto *w = new XdgWindow(this, toplevel);
            addWindow(w);
        } else if (!s->hasContent() && toplevel->window) {
            Window *w = toplevel->window;
            toplevel->window = nullptr;
            removeWindow(w);
        }
    });
    connect(t, &QObject::destroyed, this, [this, t] {
        for (Window *w : m_windows)
            if (w->kind() == Window::Xdg && static_cast<XdgWindow *>(w)->toplevel() == nullptr) {
                removeWindow(w);
                break;
            }
        Q_UNUSED(t)
    });
}

QPoint Server::placeWindow(Window *w, const QSize &frame)
{
    const QRect area = m_workArea;
    QRect g(QPoint(), frame);
    if (Window *parent = w->parentWindow()) {
        // dialogs: centred on their window, a little down (sheets are for later)
        const QRect pf = parent->frameGeometry();
        g.moveCenter(pf.center());
        g.moveTop(pf.top() + parent->titleHeight() + 20);
    } else {
        // a cascade from the top left of the work area
        const int step = 22;
        g.moveTopLeft(QPoint(area.left() + 40 + m_cascade * step, area.top() + 20 + m_cascade * step));
        m_cascade = (m_cascade + 1) % 10;
    }
    if (g.right() > area.right())
        g.moveRight(qMax(area.left() + g.width() - 1, area.right()));
    if (g.left() < area.left())
        g.moveLeft(area.left());
    if (g.bottom() > area.bottom())
        g.moveBottom(qMax(area.top() + g.height() - 1, area.bottom()));
    if (g.top() < area.top())
        g.moveTop(area.top());
    return g.topLeft();
}

void Server::addWindow(Window *w)
{
    if (w->kind() == Window::Xdg) {
        const auto menu = m_shell->menu(w->surface());
        if (!menu.first.isEmpty())
            static_cast<XdgWindow *>(w)->setMenu(menu.first, menu.second);
    }
    w->relayout();
    if (w->kind() == Window::Xdg)
        w->setPosition(placeWindow(w, w->frameGeometry().size()));
    w->mapped();
    connect(w, &Window::changed, this, [this, w] { windowChanged(w); });
    m_windows.append(w);
    activate(w);
    emit windowsChanged();
}

void Server::removeWindow(Window *w)
{
    if (!m_windows.contains(w))
        return;
    m_windows.removeAll(w);
    m_shell->windowRemoved(w);
    if (m_dragWindow == w)
        m_drag = NoDrag;
    const bool wasActive = m_active == w;
    if (wasActive)
        m_active = nullptr;
    w->deleteLater();
    if (wasActive) {
        // the window under it, of the same application first (its document window under a dialog)
        Window *next = nullptr;
        for (int i = int(m_windows.size()) - 1; i >= 0 && !next; --i)
            if (!m_windows[i]->minimized() && m_windows[i]->pid() == w->pid())
                next = m_windows[i];
        for (int i = int(m_windows.size()) - 1; i >= 0 && !next; --i)
            if (!m_windows[i]->minimized())
                next = m_windows[i];
        focus(next);
    }
    emit windowsChanged();
}

void Server::windowChanged(Window *w) { m_shell->windowChanged(w); }

void Server::restack()
{
    // the windows bottom to top; floating panels over them, full screen windows over the Dock
    int z = 0;
    for (Window *w : std::as_const(m_windows)) {
        if (!w->frameItem())
            continue;
        QQuickItem *level = m_levels[w->fullscreen() && w == m_active ? LayerOverlay : (w->above() || w->utility()) ? Floating : Windows];
        if (w->frameItem()->parentItem() != level)
            w->frameItem()->setParentItem(level);
        w->frameItem()->setZ(z++);
    }
}

void Server::raise(Window *w)
{
    if (!m_windows.contains(w))
        return;
    m_windows.removeAll(w);
    m_windows.append(w);
    // its dialogs over it
    const QList<Window *> all = m_windows;
    for (Window *d : all)
        if (d->parentWindow() == w && !d->minimized()) {
            m_windows.removeAll(d);
            m_windows.append(d);
        }
    restack();
    emit windowsChanged();
}

void Server::focus(Window *w)
{
    if (m_active == w) {
        if (w && w->surface())
            seat()->setKeyboardFocus(w->surface());
        return;
    }
    Window *old = m_active;
    m_active = w;
    if (old && m_windows.contains(old))
        old->setActive(false);
    if (w) {
        w->setActive(true);
        if (w->surface())
            seat()->setKeyboardFocus(w->surface());
        // a window brought forward: the application without windows in front no more
        if (m_frontPid && w->pid() != m_frontPid)
            setFrontApp(m_frontClient, 0);
    } else {
        seat()->setKeyboardFocus(nullptr);
    }
    restack();
    emit activeChanged();
}

void Server::activate(Window *w)
{
    if (!w)
        return;
    if (w->minimized())
        unminimize(w);
    raise(w);
    focus(w);
}

bool Server::minimizeWithScale(double *seconds) const
{
    // the Dock's settings (dock.conf, com.apple.dock's keys): mineffect genie or scale. 10.6's Dock
    // takes genie-speed (0.5 s) for the genie, 0.25 s for the scale, ten times longer with Shift.
    const QSettings dock(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/dock.conf",
                         QSettings::IniFormat);
    const bool scale = dock.value("mineffect", "genie").toString() == "scale";
    *seconds = scale ? 0.25 : qBound(0.05, dock.value("genie-speed", 0.5).toDouble(), 5.0);
    if (m_modifiers & Qt::ShiftModifier)
        *seconds *= 10;
    return scale;
}

void Server::minimize(Window *w, bool hide)
{
    if (!w || w->minimized())
        return;
    const QRect frame = w->frameGeometry();
    QPointer<Window> win(w);
    auto done = [this, win, hide, frame](const QImage &picture) {
        if (!win)
            return;
        win->setSnapshot(picture);
        win->setMinimized(true, hide);
        if (m_active == win) {
            Window *next = nullptr;
            for (int i = int(m_windows.size()) - 1; i >= 0 && !next; --i)
                if (!m_windows[i]->minimized())
                    next = m_windows[i];
            m_active = nullptr;
            win->setActive(false);
            focus(next);
            if (!next)
                emit activeChanged();
        }
        emit windowsChanged();
        if (hide)
            return;
        // into the tile the Dock makes for it: the animation waits for the Dock to say where
        double seconds = 0.5;
        const bool scale = minimizeWithScale(&seconds);
        QRect tile = win->iconGeometry();
        tile = tile.width() > 8 && tile.height() > 8 ? tile.adjusted(4, 4, -4, -4)
                                                     : QRect(screen().center().x() - 24, screen().bottom() - 56, 48, 48);
        m_effects->animate(win, picture, frame, tile, true, scale, seconds, true);
    };
    if (hide)
        done({});
    else
        grab(w, done);
}

void Server::unminimize(Window *w)
{
    if (!w || !w->minimized())
        return;
    const bool hidden = w->hidden();
    if (!hidden && !w->snapshot().isNull()) {
        // out of its tile, then the window itself
        double seconds = 0.5;
        const bool scale = minimizeWithScale(&seconds);
        QRect tile = w->iconGeometry();
        tile = tile.width() > 8 && tile.height() > 8 ? tile.adjusted(4, 4, -4, -4)
                                                     : QRect(screen().center().x() - 24, screen().bottom() - 56, 48, 48);
        QPointer<Window> win(w);
        w->frameItem()->setOpacity(0);
        m_effects->animate(w, w->snapshot(), w->frameGeometry(), tile, false, scale, seconds, false, [win] {
            if (win && win->frameItem())
                win->frameItem()->setOpacity(1);
        });
    }
    w->setSnapshot({});
    w->setMinimized(false);
    emit windowsChanged();
}

void Server::hideApplication(Window *w)
{
    const uint32_t pid = w->pid();
    const QString app = w->appId();
    for (Window *o : QList<Window *>(m_windows))
        if (pid ? o->pid() == pid : o->appId() == app)
            minimize(o, true);
}

void Server::quitApplication(Window *w)
{
    const uint32_t pid = w->pid();
    for (Window *o : QList<Window *>(m_windows))
        if (o->pid() == pid)
            o->sendClose();
}

void Server::grab(Window *w, std::function<void(const QImage &)> done)
{
    if (!w || !w->frameItem() || !w->frameItem()->isVisible() || w->frameItem()->size().isEmpty()) {
        done({});
        return;
    }
    QSharedPointer<QQuickItemGrabResult> result = w->frameItem()->grabToImage();
    if (!result) {
        done({});
        return;
    }
    connect(result.data(), &QQuickItemGrabResult::ready, this, [result, done] { done(result->image().convertToFormat(QImage::Format_ARGB32_Premultiplied)); });
}

void Server::grabScreen(std::function<void(const QImage &)> done)
{
    // the scene drawn again for it, as it is on the screen
    done(m_window->grabWindow().convertToFormat(QImage::Format_RGB32));
}

// ---- the application in front without a window

void Server::setFrontApp(QWaylandClient *client, uint32_t pid)
{
    if (!pid) {
        if (m_frontClient && client != m_frontClient)
            return;   // someone else's
        m_frontClient = nullptr;
        m_frontPid = 0;
    } else {
        m_frontClient = client;
        m_frontPid = pid;
        // in front: the key window loses the keyboard to it (the menu bar's application)
        if (m_active && m_active->pid() != pid) {
            Window *old = m_active;
            m_active = nullptr;
            old->setActive(false);
            seat()->setKeyboardFocus(nullptr);
            emit activeChanged();
        }
    }
    ++m_frontSerial;
    emit frontAppChanged();
}

void Server::setKeyEquivalents(QWaylandClient *client, const QList<uint32_t> &keysyms)
{
    if (keysyms.isEmpty())
        m_keyEquivalents.remove(client);
    else
        m_keyEquivalents[client] = keysyms;
}

// ---- popups

void Server::popupCreated(XdgPopup *p)
{
    QPointer<XdgSurface> xs(p->xdgSurface());
    QPointer<XdgPopup> popup(p);
    auto reposition = [this, popup] {
        if (popup)
            placePopup(popup);
    };
    connect(p, &XdgPopup::repositioned, this, reposition);
    connect(p, &XdgPopup::parentAttached, this, reposition);
    connect(xs->surface(), &QWaylandSurface::hasContentChanged, p, [this, popup, xs] {
        // the xdg_surface may be gone before the popup's last commit (a menu closing)
        if (!popup || !xs || !xs->surface())
            return;
        if (xs->surface()->hasContent()) {
            if (!xs->item) {
                xs->item = createSurfaceItem(xs->surface(), m_levels[Popups]);
                xs->item->setFocusOnClick(false);
            }
            placePopup(popup);
            xs->item->setVisible(true);
            if (popup->grabbed())
                seat()->setKeyboardFocus(xs->surface());
        } else if (xs->item) {
            xs->item->setVisible(false);
        }
    });
    connect(p, &XdgPopup::grabRequested, this, [this, popup] {
        if (!popup)
            return;
        m_grabs.append(popup);
        if (popup->xdgSurface()->surface() && popup->xdgSurface()->surface()->hasContent())
            seat()->setKeyboardFocus(popup->xdgSurface()->surface());
    });
    connect(p, &QObject::destroyed, this, [this] {
        // the grab goes back to the popup under it, or the window
        m_grabs.removeAll(nullptr);
        if (!m_grabs.isEmpty() && m_grabs.last() && m_grabs.last()->xdgSurface()->surface())
            seat()->setKeyboardFocus(m_grabs.last()->xdgSurface()->surface());
        else if (m_active && m_active->surface())
            seat()->setKeyboardFocus(m_active->surface());
        else
            seat()->setKeyboardFocus(nullptr);
    });
    connect(xs, &XdgSurface::surfaceGone, this, [](XdgSurface *gone) { delete gone->item; });
}

void Server::placePopup(XdgPopup *p)
{
    // the parent's window geometry on the screen
    QPoint origin;
    if (XdgSurface *parent = p->parentXdgSurface()) {
        if (!parent->item)
            return;
        origin = parent->item->mapToScene(QPointF(0, 0)).toPoint() + parent->windowGeometry().topLeft();
    } else if (LayerSurface *layer = p->parentLayer()) {
        origin = layer->geometry().topLeft();
    } else {
        return;
    }
    const QRect bounds = screen().translated(-origin);
    const QRect place = p->positioner().place(bounds);
    p->setGeometry(place);
    if (QWaylandQuickItem *item = p->xdgSurface()->item) {
        const QRect g = p->xdgSurface()->windowGeometry();
        item->setPosition(origin + place.topLeft() - g.topLeft());
    }
}

void Server::dismissPopups()
{
    const auto grabs = m_grabs;
    m_grabs.clear();
    for (int i = int(grabs.size()) - 1; i >= 0; --i)
        if (grabs[i])
            grabs[i]->dismiss();
}

// ---- interactive moves and resizes

void Server::startMove(Window *w)
{
    m_drag = Moving;
    m_dragWindow = w;
    m_dragStart = m_window->mapFromGlobal(QCursor::pos());
    m_dragGeometry = w->frameGeometry();
}

void Server::startResize(Window *w, Qt::Edges edges)
{
    m_drag = Resizing;
    m_dragWindow = w;
    m_dragStart = m_window->mapFromGlobal(QCursor::pos());
    m_dragGeometry = w->contentGeometry();
    m_dragEdges = edges;
}

bool Server::handleMouse(QEvent *e)
{
    auto *me = static_cast<QMouseEvent *>(e);
    if (e->type() == QEvent::MouseButtonPress && !m_grabs.isEmpty()) {
        // a click outside the menus closes them (and goes nowhere else)
        bool inside = false;
        for (const auto &g : std::as_const(m_grabs))
            if (g && g->xdgSurface()->item) {
                QQuickItem *item = g->xdgSurface()->item;
                if (QRectF(item->mapToScene(QPointF(0, 0)), item->size()).contains(me->scenePosition()))
                    inside = true;
            }
        if (!inside) {
            dismissPopups();
            return true;
        }
    }
    if (m_drag == NoDrag || !m_dragWindow)
        return false;
    if (e->type() == QEvent::MouseButtonRelease) {
        m_drag = NoDrag;
        return true;
    }
    if (e->type() != QEvent::MouseMove)
        return false;
    const QPoint d = (me->scenePosition() - m_dragStart).toPoint();
    if (m_drag == Moving) {
        QPoint to = m_dragGeometry.topLeft() + d;
        to.setY(qMax(to.y(), m_workArea.top()));
        m_dragWindow->setPosition(to);
    } else {
        QRect g = m_dragGeometry;
        if (m_dragEdges & Qt::LeftEdge)
            g.setLeft(g.left() + d.x());
        if (m_dragEdges & Qt::RightEdge)
            g.setRight(g.right() + d.x());
        if (m_dragEdges & Qt::TopEdge)
            g.setTop(g.top() + d.y());
        if (m_dragEdges & Qt::BottomEdge)
            g.setBottom(g.bottom() + d.y());
        const QSize min = m_dragWindow->minimumSize().expandedTo(QSize(40, 20));
        g.setSize(g.size().expandedTo(min));
        m_dragWindow->configure(g.size());
        if (m_dragEdges & (Qt::LeftEdge | Qt::TopEdge))
            m_dragWindow->setPosition(QPoint(m_dragEdges & Qt::LeftEdge ? g.left() : m_dragWindow->position().x(),
                                             m_dragEdges & Qt::TopEdge ? g.top() - m_dragWindow->titleHeight() : m_dragWindow->position().y()));
    }
    return true;
}

// ---- the cursor

void Server::handleCursor(QWaylandSurface *surface, int hotX, int hotY)
{
    if (m_cursorSurface)
        disconnect(m_cursorSurface, nullptr, this, nullptr);
    m_cursorSurface = surface;
    m_cursorHotspot = QPoint(hotX, hotY);
    if (surface)
        connect(surface, &QWaylandSurface::redraw, this, &Server::updateCursor);
    updateCursor();
}

void Server::updateCursor()
{
    // the cursor of the surface under the pointer, which asked for it: its picture, as the window's
    QWaylandView *view = seat()->mouseFocus();
    auto *item = view ? qobject_cast<QQuickItem *>(view->renderObject()) : nullptr;
    if (!item)
        return;
    if (!m_cursorSurface || !m_cursorSurface->hasContent()) {
        item->setCursor(Qt::BlankCursor);
        return;
    }
    // shared memory cursors (wl_cursor's): their picture; the others the theme's arrow
    QWaylandSurface *s = m_cursorSurface;
    QImage image;
    for (QWaylandView *v : s->views())
        if (v->currentBuffer().hasBuffer() && v->currentBuffer().isSharedMemory()) {
            image = v->currentBuffer().image();
            break;
        }
    if (image.isNull()) {
        // the surface has no view of ours: read it through a view made for it
        static QWaylandView cursorView;
        cursorView.setSurface(s);
        cursorView.advance();
        if (cursorView.currentBuffer().isSharedMemory())
            image = cursorView.currentBuffer().image();
    }
    if (image.isNull()) {
        item->setCursor(Qt::ArrowCursor);
        return;
    }
    const int scale = qMax(1, s->bufferScale());
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(scale);
    item->setCursor(QCursor(pixmap, m_cursorHotspot.x(), m_cursorHotspot.y()));
    s->frameStarted();
    s->sendFrameCallbacks();
}

// ---- keys

bool Server::handleKey(QKeyEvent *e)
{
    m_modifiers = e->modifiers();
    static QSet<quint32> swallowed;   // presses WindowServer took: their releases too
    if (e->type() == QEvent::KeyRelease) {
        if (swallowed.remove(e->nativeScanCode()))
            return true;
        seat()->sendFullKeyEvent(e);
        return true;
    }
    const Qt::KeyboardModifiers m = e->modifiers();
    const bool command = m & Qt::MetaModifier;
    const int key = e->key();
    auto take = [&] {
        swallowed.insert(e->nativeScanCode());
        return true;
    };
    if (command) {
        // the symbolic hot keys (CGSSetSymbolicHotKey): 28 ⇧⌘3, 29 ⌃⇧⌘3, 30 ⇧⌘4, 31 ⌃⇧⌘4
        if ((m & Qt::ShiftModifier) && (key == Qt::Key_3 || key == Qt::Key_4 || key == Qt::Key_NumberSign || key == Qt::Key_Dollar)) {
            const int hotKey = (key == Qt::Key_3 || key == Qt::Key_NumberSign ? 28 : 30) + ((m & Qt::ControlModifier) ? 1 : 0);
            if (m_shell->hotKey(hotKey))
                return take();
        }
        // ⇧⌘Q Log Out, ⌥⇧⌘Q at once (the Apple menu's: loginwindow's hot keys on the Mac)
        if (key == Qt::Key_Q && (m & Qt::ShiftModifier)) {
            Session::logOut(!(m & Qt::AltModifier));
            return take();
        }
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
            // the next window down, brought to the front
            for (int i = int(m_windows.size()) - 2; i >= 0; --i)
                if (!m_windows[i]->minimized()) {
                    activate(m_windows[i]);
                    break;
                }
            return take();
        }
    }
    // an application in front without a window has the keyboard: ⌘Q, ⌘H... are its menus'
    if (!m_active && m_frontPid && m_grabs.isEmpty()) {
        uint32_t mods = 0;
        if (m & Qt::ShiftModifier)
            mods |= 1;
        if (m & Qt::ControlModifier)
            mods |= 4;
        if (m & Qt::AltModifier)
            mods |= 8;
        if (m & Qt::MetaModifier)
            mods |= 64;
        if (m_shell->keyForFrontApp(keysymOf(e), e->text(), mods))
            return take();
    }
    if (command && m_active && m_grabs.isEmpty()) {
        // a key the application's own menus answer: its
        QWaylandClient *client = m_active->surface() ? m_active->surface()->client() : nullptr;
        const bool theirs = client && m_keyEquivalents.value(client).contains(keysymOf(e));
        if (!theirs) {
            switch (key) {
            case Qt::Key_W: m_active->sendClose(); return take();
            case Qt::Key_M: minimize(m_active); return take();
            case Qt::Key_H: hideApplication(m_active); return take();
            case Qt::Key_Q: quitApplication(m_active); return take();
            default: break;
            }
        }
    }
    seat()->sendFullKeyEvent(e);
    return true;
}

void Server::testInput(const QString &line)
{
    const QStringList w = line.simplified().split(' ');
    if (w.isEmpty())
        return;
    static QPointF at;
    auto mouse = [this](QEvent::Type type, QPointF p, Qt::MouseButtons buttons) {
        QMouseEvent e(type, p, p, m_window->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, m_modifiers);
        QCoreApplication::sendEvent(m_window, &e);
    };
    if (w[0] == "key" && w.size() >= 2) {
        Qt::KeyboardModifiers mods;
        const QStringList m = w.value(2).split('+', Qt::SkipEmptyParts);
        if (m.contains("meta"))
            mods |= Qt::MetaModifier;
        if (m.contains("shift"))
            mods |= Qt::ShiftModifier;
        if (m.contains("control"))
            mods |= Qt::ControlModifier;
        if (m.contains("alt"))
            mods |= Qt::AltModifier;
        const QKeySequence seq(w[1]);
        const int key = seq.isEmpty() ? 0 : seq[0].key();
        const QString text = w[1].size() == 1 ? w[1].toLower() : QString();
        QKeyEvent press(QEvent::KeyPress, key, mods, 0, 0, 0, text);
        QKeyEvent release(QEvent::KeyRelease, key, mods, 0, 0, 0, text);
        QCoreApplication::sendEvent(m_window, &press);
        QCoreApplication::sendEvent(m_window, &release);
        return;
    }
    if (w.size() < 3)
        return;
    const QPointF p(w[1].toDouble(), w[2].toDouble());
    if (w[0] == "move") {
        mouse(QEvent::MouseMove, p, QGuiApplication::mouseButtons());
    } else if (w[0] == "press") {
        mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
    } else if (w[0] == "release") {
        mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
    } else if (w[0] == "click" || w[0] == "doubleclick") {
        mouse(QEvent::MouseMove, p, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
        if (w[0] == "doubleclick") {
            mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
            mouse(QEvent::MouseButtonDblClick, p, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
        }
    }
    at = p;
}

void Server::testInput(const QString &line)
{
    const QStringList w = line.simplified().split(' ');
    if (w.isEmpty())
        return;
    auto mouse = [this](QEvent::Type type, QPointF p, Qt::MouseButtons buttons) {
        QMouseEvent e(type, p, p, m_window->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, m_modifiers);
        QCoreApplication::sendEvent(m_window, &e);
    };
    if (w[0] == "key" && w.size() >= 2) {
        Qt::KeyboardModifiers mods;
        const QStringList m = w.value(2).split('+', Qt::SkipEmptyParts);
        if (m.contains("meta"))
            mods |= Qt::MetaModifier;
        if (m.contains("shift"))
            mods |= Qt::ShiftModifier;
        if (m.contains("control"))
            mods |= Qt::ControlModifier;
        if (m.contains("alt"))
            mods |= Qt::AltModifier;
        const QKeySequence seq(w[1]);
        const int key = seq.isEmpty() ? 0 : seq[0].key();
        const QString text = w[1].size() == 1 ? w[1].toLower() : QString();
        QKeyEvent press(QEvent::KeyPress, key, mods, 0, 0, 0, text);
        QKeyEvent release(QEvent::KeyRelease, key, mods, 0, 0, 0, text);
        QCoreApplication::sendEvent(m_window, &press);
        QCoreApplication::sendEvent(m_window, &release);
        return;
    }
    if (w.size() < 3)
        return;
    const QPointF p(w[1].toDouble(), w[2].toDouble());
    if (w[0] == "move") {
        mouse(QEvent::MouseMove, p, Qt::NoButton);
    } else if (w[0] == "drag") {
        mouse(QEvent::MouseMove, p, Qt::LeftButton);
    } else if (w[0] == "press") {
        mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
    } else if (w[0] == "release") {
        mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
    } else if (w[0] == "click" || w[0] == "doubleclick") {
        mouse(QEvent::MouseMove, p, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
        if (w[0] == "doubleclick") {
            mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
            mouse(QEvent::MouseButtonDblClick, p, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
        }
    }
}

bool Server::eventFilter(QObject *o, QEvent *e)
{
    if (o != m_window)
        return false;
    switch (e->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease: return handleKey(static_cast<QKeyEvent *>(e));
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove: {
        m_modifiers = static_cast<QMouseEvent *>(e)->modifiers();
        const bool taken = handleMouse(e);
        if (!taken && e->type() == QEvent::MouseMove)
            QMetaObject::invokeMethod(this, &Server::updateCursor, Qt::QueuedConnection);
        return taken;
    }
    default: return false;
    }
}
