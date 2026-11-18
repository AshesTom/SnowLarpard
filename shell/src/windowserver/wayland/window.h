// SPDX-License-Identifier: LGPL-2.1-or-later
// An application window on the Wayland server: its frame in the scene (the shadow, 10.6's title bar
// when WindowServer draws it, the application's surface, the size box), and what the shell knows
// of it (sl_window_server_v1's window event). An xdg toplevel's (XdgWindow) or an X11 window's
// through Xwayland (XWindow, xwayland.cpp).
#pragma once

#include "frame.h"
#include "imageitem.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QWaylandQuickItem>
#include <QRect>

class QWaylandSurface;
class QWaylandQuickItem;
class Server;
class Window;
class XdgToplevel;

// The frame's title bar: drawn by frame.cpp, its buttons, a drag moves the window, a double click
// minimizes it (AppleMiniaturizeOnDoubleClick)
class TitleItem : public ImageItem {
public:
    explicit TitleItem(Window *window, QQuickItem *parent);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void hoverMoveEvent(QHoverEvent *e) override;
    void hoverLeaveEvent(QHoverEvent *e) override;

private:
    Window *m_window;
    QPointF m_pressAt;
    QPoint m_frameAt;
    bool m_dragging = false;
};

// The size box: dragging the window's bottom right corner resizes it
class GripItem : public QQuickItem {
public:
    explicit GripItem(Window *window, QQuickItem *parent);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    Window *m_window;
    QPointF m_pressAt;
    QSize m_startSize;
};

// The frame: a click anywhere in it brings the window forward (the click still goes to the
// application, as on the Mac with a click-through... 10.6 brings forward and delivers)
class FrameItem : public QQuickItem {
public:
    explicit FrameItem(Window *window, QQuickItem *parent);

protected:
    bool childMouseEventFilter(QQuickItem *item, QEvent *e) override;

private:
    Window *m_window;
};

class Window : public QObject {
    Q_OBJECT
public:
    enum Kind { Xdg, X11 };
    Window(Server *server, QWaylandSurface *surface);
    ~Window() override;

    virtual Kind kind() const = 0;
    uint32_t id() const { return m_id; }
    Server *server() const { return m_server; }
    QWaylandSurface *surface() const { return m_surface; }
    QWaylandQuickItem *surfaceItem() const { return m_surfaceItem; }
    FrameItem *frameItem() const { return m_frame; }

    virtual QString title() const = 0;
    virtual QString appId() const = 0;
    virtual uint32_t pid() const;
    virtual Window *parentWindow() const { return nullptr; }
    // the application's rectangle in its surface (xdg_surface's window geometry; the surface's)
    virtual QRect windowGeometry() const;
    virtual bool decorated() const = 0;    // WindowServer draws its title bar
    virtual bool utility() const { return false; }
    virtual bool skipTaskbar() const { return false; }
    virtual QSize minimumSize() const { return {}; }
    virtual QString menuService() const { return {}; }
    virtual QString menuPath() const { return {}; }

    // asks the application for this size of its own rectangle (0 x 0: its choice), these states
    virtual void configure(const QSize &content) = 0;
    virtual void sendClose() = 0;
    // the frame moved: an X11 window is told where it is
    virtual void moved() {}
    virtual void activated(bool) {}

    int titleHeight() const { return decorated() && !m_fullscreen ? (utility() ? Frame::UtilityTitleHeight : Frame::TitleHeight) : 0; }
    // the frame's place on the screen (its title bar's top left) and the whole frame
    QPoint position() const;
    void setPosition(const QPoint &p);
    QRect frameGeometry() const;
    QRect contentGeometry() const;   // the application's rectangle on the screen

    bool active() const { return m_active; }
    void setActive(bool active);
    bool minimized() const { return m_minimized; }
    bool hidden() const { return m_hidden; }
    bool zoomed() const { return m_zoomed; }
    bool fullscreen() const { return m_fullscreen; }
    bool above() const { return m_above; }
    void setAbove(bool above) { m_above = above; }
    uint32_t flags() const;
    QRect iconGeometry() const { return m_iconGeometry; }
    void setIconGeometry(const QRect &r) { m_iconGeometry = r; }
    QImage snapshot() const { return m_snapshot; }   // a minimized window's picture
    void setSnapshot(const QImage &image) { m_snapshot = image; }

    void setMinimized(bool minimized, bool hidden = false);
    void toggleZoom();
    void setFullscreen(bool on);
    // the title bar's pointer state
    Frame::TitleBar titleBar() const;
    void setTitleBarState(int pressed, bool overButtons);
    void titleButtonClicked(int button);

    // the frame laid out again (the application's size, title bar, shadow)
    void relayout();
    void updateTitle();
    void updateShadow();
    void mapped();

    // its shadow's picture and where it is from the frame (for capture_window)
    QImage shadowImage(QPoint *origin) const;

signals:
    void changed();   // what the shell knows of it

protected:
    void createItems();
    Server *m_server;
    QWaylandSurface *m_surface;
    uint32_t m_id;
    QPointer<FrameItem> m_frame;
    QPointer<ImageItem> m_shadow;
    QPointer<TitleItem> m_title;
    QPointer<GripItem> m_grip;
    QPointer<QWaylandQuickItem> m_surfaceItem;
    bool m_active = false, m_minimized = false, m_hidden = false, m_zoomed = false, m_fullscreen = false, m_above = false;
    QRect m_restore;   // before zooming or full screen (the content's)
    QRect m_iconGeometry;
    QImage m_snapshot;
    QSize m_shadowSize;
    int m_shadowKind = -1;
    int m_pressed = Frame::None;
    bool m_overButtons = false;
};

class XdgWindow : public Window {
    Q_OBJECT
public:
    XdgWindow(Server *server, XdgToplevel *toplevel);
    Kind kind() const override { return Xdg; }
    XdgToplevel *toplevel() const { return m_toplevel; }
    QString title() const override;
    QString appId() const override;
    Window *parentWindow() const override;
    QRect windowGeometry() const override;
    bool decorated() const override;
    bool skipTaskbar() const override { return false; }
    QSize minimumSize() const override;
    QString menuService() const override { return m_menuService; }
    QString menuPath() const override { return m_menuPath; }
    void setMenu(const QString &service, const QString &path);
    void configure(const QSize &content) override;
    void sendClose() override;
    void activated(bool) override;

private:
    QPointer<XdgToplevel> m_toplevel;
    QString m_menuService, m_menuPath;
    QSize m_requested;
};
