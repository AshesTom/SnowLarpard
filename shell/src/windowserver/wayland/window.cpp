// SPDX-License-Identifier: LGPL-2.1-or-later
#include "window.h"

#include "server.h"
#include "shellprotocol.h"
#include "xdgshell.h"

#include <QCursor>
#include <QMouseEvent>
#include <QWaylandClient>
#include <QWaylandQuickItem>
#include <QWaylandSurface>

// ---- the title bar

TitleItem::TitleItem(Window *window, QQuickItem *parent) : ImageItem(parent), m_window(window)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
}

void TitleItem::mousePressEvent(QMouseEvent *e)
{
    m_pressAt = e->scenePosition();
    m_frameAt = m_window->position();
    m_dragging = false;
    const int b = Frame::buttonAt(m_window->titleBar(), e->position().toPoint());
    m_window->setTitleBarState(b, b != Frame::None && b != Frame::ToolbarButton);
    if (b == Frame::None)
        m_dragging = true;
}

void TitleItem::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging) {
        QPoint to = m_frameAt + (e->scenePosition() - m_pressAt).toPoint();
        // never under the menu bar
        to.setY(qMax(to.y(), m_window->server()->workArea().top()));
        m_window->setPosition(to);
        return;
    }
    const Frame::TitleBar t = m_window->titleBar();
    const int over = Frame::buttonAt(t, e->position().toPoint());
    if (t.pressed != Frame::None && over != t.pressed)
        m_window->setTitleBarState(Frame::None, over != Frame::None);
}

void TitleItem::mouseReleaseEvent(QMouseEvent *e)
{
    const Frame::TitleBar t = m_window->titleBar();
    const int b = Frame::buttonAt(t, e->position().toPoint());
    m_window->setTitleBarState(Frame::None, b != Frame::None && b != Frame::ToolbarButton);
    if (!m_dragging && b != Frame::None && b == t.pressed)
        m_window->titleButtonClicked(b);
    m_dragging = false;
}

void TitleItem::mouseDoubleClickEvent(QMouseEvent *e)
{
    // AppleMiniaturizeOnDoubleClick: 10.6's default
    if (Frame::buttonAt(m_window->titleBar(), e->position().toPoint()) == Frame::None)
        m_window->server()->minimize(m_window);
}

void TitleItem::hoverMoveEvent(QHoverEvent *e)
{
    const Frame::TitleBar t = m_window->titleBar();
    // the three show their glyphs together while the pointer is over them
    const bool over = Frame::buttonRect(t, Frame::Close).united(Frame::buttonRect(t, t.buttons ? Frame::Zoom : Frame::Close)).contains(e->position().toPoint());
    if (over != t.overButtons)
        m_window->setTitleBarState(t.pressed, over);
}

void TitleItem::hoverLeaveEvent(QHoverEvent *)
{
    if (m_window->titleBar().overButtons)
        m_window->setTitleBarState(Frame::None, false);
}

// ---- the size box

GripItem::GripItem(Window *window, QQuickItem *parent) : QQuickItem(parent), m_window(window)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::SizeFDiagCursor);
}

void GripItem::mousePressEvent(QMouseEvent *e)
{
    m_pressAt = e->scenePosition();
    m_startSize = m_window->contentGeometry().size();
    m_window->server()->activate(m_window);
}

void GripItem::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint d = (e->scenePosition() - m_pressAt).toPoint();
    const QSize min = m_window->minimumSize().expandedTo(QSize(Frame::MinimumWidth, 40));
    m_window->configure(QSize(qMax(min.width(), m_startSize.width() + d.x()), qMax(min.height(), m_startSize.height() + d.y())));
}

void GripItem::mouseReleaseEvent(QMouseEvent *) {}

// ---- the frame

FrameItem::FrameItem(Window *window, QQuickItem *parent) : QQuickItem(parent), m_window(window)
{
    setFiltersChildMouseEvents(true);
}

bool FrameItem::childMouseEventFilter(QQuickItem *, QEvent *e)
{
    if (e->type() == QEvent::MouseButtonPress && !m_window->active())
        m_window->server()->activate(m_window);
    return false;
}

// ---- the window

Window::Window(Server *server, QWaylandSurface *surface) : QObject(server), m_server(server), m_surface(surface), m_id(server->m_nextId++) {}

Window::~Window()
{
    delete m_frame;
}

uint32_t Window::pid() const
{
    return m_surface && m_surface->client() ? uint32_t(m_surface->client()->processId()) : 0;
}

QRect Window::windowGeometry() const
{
    return QRect(QPoint(0, 0), m_surface ? m_surface->destinationSize() : QSize());
}

void Window::createItems()
{
    m_frame = new FrameItem(this, m_server->level(Server::Windows));
    m_frame->setVisible(false);
    m_shadow = new ImageItem(m_frame);
    m_surfaceItem = m_server->createSurfaceItem(m_surface, m_frame);
    m_title = new TitleItem(this, m_frame);
    m_grip = new GripItem(this, m_frame);
    m_grip->setZ(2);
    connect(m_surface, &QWaylandSurface::destinationSizeChanged, this, &Window::relayout);
}

QPoint Window::position() const { return m_frame ? m_frame->position().toPoint() : QPoint(); }

void Window::setPosition(const QPoint &p)
{
    if (!m_frame || m_frame->position().toPoint() == p)
        return;
    m_frame->setPosition(p);
    moved();
    emit changed();
}

QRect Window::frameGeometry() const
{
    const QRect g = windowGeometry();
    return QRect(position(), QSize(g.width(), g.height() + titleHeight()));
}

QRect Window::contentGeometry() const
{
    const QRect g = windowGeometry();
    return QRect(position() + QPoint(0, titleHeight()), g.size());
}

uint32_t Window::flags() const
{
    uint32_t f = 0;
    if (m_minimized && !m_hidden)
        f |= ShellProtocol::Minimized;
    if (m_hidden)
        f |= ShellProtocol::Hidden;
    if (utility())
        f |= ShellProtocol::Utility;
    if (parentWindow())
        f |= ShellProtocol::Dialog;
    if (skipTaskbar())
        f |= ShellProtocol::SkipTaskbar;
    if (m_fullscreen)
        f |= ShellProtocol::Fullscreen;
    if (m_zoomed)
        f |= ShellProtocol::Zoomed;
    if (kind() == X11)
        f |= ShellProtocol::X11Window;
    if (m_above)
        f |= ShellProtocol::Above;
    return f;
}

void Window::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    activated(active);
    updateTitle();
    updateShadow();
}

Frame::TitleBar Window::titleBar() const
{
    Frame::TitleBar t;
    t.width = windowGeometry().width();
    t.title = title();
    t.active = m_active;
    t.utility = utility();
    t.pressed = m_pressed;
    t.overButtons = m_overButtons;
    return t;
}

void Window::setTitleBarState(int pressed, bool overButtons)
{
    if (pressed == m_pressed && overButtons == m_overButtons)
        return;
    m_pressed = pressed;
    m_overButtons = overButtons;
    updateTitle();
}

void Window::titleButtonClicked(int button)
{
    switch (button) {
    case Frame::Close: sendClose(); break;
    case Frame::Minimize: m_server->minimize(this); break;
    case Frame::Zoom: toggleZoom(); break;
    default: break;
    }
}

void Window::updateTitle()
{
    if (!m_title)
        return;
    const int th = titleHeight();
    m_title->setVisible(th > 0);
    if (!th)
        return;
    QImage image = Frame::paintTitleBar(titleBar()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // its top corners rounded (CoreUI's window path), as WindowServer shapes its frames
    const std::vector<uint8_t> outline = Frame::windowOutline(image.width(), image.height(), false);
    for (int y = 0; y < qMin(4, image.height()); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (!outline[size_t(y) * size_t(image.width()) + size_t(x)])
                line[x] = 0;
    }
    m_title->setImage(image);
    m_title->setSize(image.size());
}

QImage Window::shadowImage(QPoint *origin) const
{
    if (!m_shadow || m_shadow->image().isNull())
        return {};
    if (origin)
        *origin = m_shadow->position().toPoint();
    return m_shadow->image();
}

void Window::updateShadow()
{
    if (!m_shadow)
        return;
    // an application drawing its own frame draws its shadow too (its surface larger than its
    // window geometry: GTK's, AquaKit's); full screen windows have none
    const QRect g = windowGeometry();
    const QSize surfaceSize = m_surface ? m_surface->destinationSize() : QSize();
    const bool own = !decorated() && (g.size() != surfaceSize);
    const int kind = (own || m_fullscreen || g.isEmpty()) ? Frame::NoShadow : utility() ? Frame::PanelShadow
                                                                                       : m_active ? Frame::KeyShadow
                                                                                                  : Frame::WindowShadow;
    const QSize size = frameGeometry().size();
    if (kind == m_shadowKind && size == m_shadowSize)
        return;
    m_shadowKind = kind;
    m_shadowSize = size;
    if (kind == Frame::NoShadow) {
        m_shadow->setImage({});
        m_shadow->setVisible(false);
        return;
    }
    QPoint origin;
    const QImage mask = Frame::shadowMask(Frame::windowOutline(size.width(), size.height(), false), size.width(), size.height(), kind, &origin);
    // black through the mask's alpha
    QImage shadow(mask.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *a = mask.constScanLine(y);
        auto *line = reinterpret_cast<QRgb *>(shadow.scanLine(y));
        for (int x = 0; x < mask.width(); ++x)
            line[x] = qRgba(0, 0, 0, a[x]);
    }
    m_shadow->setImage(shadow);
    m_shadow->setPosition(origin);
    m_shadow->setSize(shadow.size());
    m_shadow->setVisible(true);
}

void Window::relayout()
{
    if (!m_frame || !m_surfaceItem)
        return;
    const QRect g = windowGeometry();
    const int th = titleHeight();
    m_frame->setSize(QSize(g.width(), g.height() + th));
    m_surfaceItem->setPosition(QPoint(-g.x(), th - g.y()));
    m_title->setPosition(QPoint(0, 0));
    if (m_grip) {
        // a window WindowServer frames has its size box (10.6's are drawn by the application; here
        // the corner takes the drag)
        m_grip->setVisible(decorated() && !m_fullscreen);
        m_grip->setPosition(QPoint(g.width() - Frame::GripSize, th + g.height() - Frame::GripSize));
        m_grip->setSize(QSize(Frame::GripSize, Frame::GripSize));
    }
    updateTitle();
    updateShadow();
    emit changed();
}

void Window::mapped()
{
    relayout();
    m_frame->setVisible(!m_minimized);
}

void Window::setMinimized(bool minimized, bool hidden)
{
    m_minimized = minimized;
    m_hidden = minimized && hidden;
    if (m_frame)
        m_frame->setVisible(!minimized);
    emit changed();
}

void Window::toggleZoom()
{
    // 10.6's zoom goes between the application's size and the largest: here, the work area's
    const QRect area = m_server->workArea();
    if (m_zoomed) {
        m_zoomed = false;
        setPosition(m_restore.topLeft() - QPoint(0, titleHeight()));
        configure(m_restore.size());
    } else {
        m_restore = contentGeometry();
        m_zoomed = true;
        setPosition(area.topLeft());
        configure(QSize(area.width(), area.height() - titleHeight()));
    }
    emit changed();
}

void Window::setFullscreen(bool on)
{
    if (on == m_fullscreen)
        return;
    if (on) {
        if (!m_zoomed)
            m_restore = contentGeometry();
        m_fullscreen = true;
        setPosition(m_server->screen().topLeft());
        configure(m_server->screen().size());
        m_server->raise(this);
    } else {
        m_fullscreen = false;
        setPosition(m_restore.topLeft() - QPoint(0, titleHeight()));
        configure(m_restore.size());
    }
    relayout();
}

// ---- an xdg toplevel's

XdgWindow::XdgWindow(Server *server, XdgToplevel *toplevel)
    : Window(server, toplevel->xdgSurface()->surface()), m_toplevel(toplevel)
{
    toplevel->window = this;
    createItems();
    toplevel->xdgSurface()->item = m_surfaceItem;
    connect(toplevel, &XdgToplevel::titleChanged, this, [this] {
        updateTitle();
        emit changed();
    });
    connect(toplevel, &XdgToplevel::appIdChanged, this, &Window::changed);
    connect(toplevel, &XdgToplevel::parentChanged, this, &Window::changed);
    connect(toplevel, &XdgToplevel::decorationChanged, this, &Window::relayout);
    connect(toplevel->xdgSurface(), &XdgSurface::windowGeometryChanged, this, &Window::relayout);
    connect(toplevel, &XdgToplevel::moveRequested, this, [this] { m_server->startMove(this); });
    connect(toplevel, &XdgToplevel::resizeRequested, this, [this](Qt::Edges e) { m_server->startResize(this, e); });
    connect(toplevel, &XdgToplevel::minimizeRequested, this, [this] { m_server->minimize(this); });
    connect(toplevel, &XdgToplevel::maximizeRequested, this, [this](bool on) {
        if (on != zoomed())
            toggleZoom();
        else
            configure(m_requested);
    });
    connect(toplevel, &XdgToplevel::fullscreenRequested, this, [this](bool on) { setFullscreen(on); });
}

QString XdgWindow::title() const { return m_toplevel ? m_toplevel->title() : QString(); }
QString XdgWindow::appId() const { return m_toplevel ? m_toplevel->appId() : QString(); }

Window *XdgWindow::parentWindow() const
{
    return m_toplevel && m_toplevel->parentToplevel() ? m_toplevel->parentToplevel()->window : nullptr;
}

QRect XdgWindow::windowGeometry() const
{
    return m_toplevel ? m_toplevel->xdgSurface()->windowGeometry() : QRect();
}

bool XdgWindow::decorated() const { return m_toplevel && m_toplevel->serverSideDecoration(); }

QSize XdgWindow::minimumSize() const { return m_toplevel ? m_toplevel->minSize() : QSize(); }

void XdgWindow::setMenu(const QString &service, const QString &path)
{
    m_menuService = service;
    m_menuPath = path;
    emit changed();
}

void XdgWindow::configure(const QSize &content)
{
    m_requested = content;
    if (m_toplevel)
        m_toplevel->setConfigure(content, active(), zoomed(), fullscreen(), false);
}

void XdgWindow::sendClose()
{
    if (m_toplevel)
        m_toplevel->sendClose();
}

void XdgWindow::activated(bool) { configure(m_requested); }
