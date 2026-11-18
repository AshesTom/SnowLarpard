// SPDX-License-Identifier: LGPL-2.1-or-later
// WindowServer on Wayland: the session's compositor. Its scene (Qt Quick) has the Mac's window
// levels: the desktop picture, the layer shell's background and bottom layers (the Finder's
// desktop), the applications' windows (each in its frame: shadow, 10.6's title bar), the top layer
// (the Dock, the menu bar), the menus (popups), the overlay layer, then the effects (the genie).
// It does what the X11 WindowServer does: focus and stacking, Command (Super) + W, M, H, Q and Tab,
// the symbolic hot keys, minimizing into the Dock, zooming, full screen; and an Xwayland for the X11
// programs (xwayland.cpp).
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <functional>
#include <QWaylandClient>
#include <QWaylandSurface>

#include "window.h"
#include "xdgshell.h"

class QWaylandCompositor;
class QWaylandQuickCompositor;
class QWaylandQuickItem;
class QWaylandSeat;
class QWaylandSurface;
class QWaylandOutput;
class QWaylandClient;
class QKeyEvent;
class Window;
class XdgShell;
class XdgToplevel;
class XdgPopup;
class LayerShell;
class LayerSurface;
class ShellProtocol;
class Effects;
class Xwayland;

class Server : public QObject {
    Q_OBJECT
public:
    enum Level { DesktopPicture, LayerBackground, LayerBottom, Windows, Floating, LayerTop, Popups, LayerOverlay, EffectsLevel, Levels };

    explicit Server(QObject *parent = nullptr);
    ~Server() override;
    // the socket's name (WAYLAND_DISPLAY), the window on the compositor or screen under it
    bool start(const QString &socketName, bool fullScreen, const QSize &size);
    QString socketName() const;

    QWaylandCompositor *compositor() const;
    QWaylandSeat *seat() const;
    QQuickWindow *window() const { return m_window; }
    QQuickItem *level(Level l) const { return m_levels[l]; }
    QQuickItem *layerItem(uint32_t layer) const;   // zwlr_layer_shell_v1.layer's
    QWaylandQuickItem *createSurfaceItem(QWaylandSurface *surface, QQuickItem *parent);
    Effects *effects() const { return m_effects; }
    ShellProtocol *shell() const { return m_shell; }
    Xwayland *xwayland() const { return m_xwayland; }

    QRect screen() const;
    QRect workArea() const { return m_workArea; }

    // ---- the layer shell's surfaces
    void addLayerSurface(LayerSurface *l);
    void removeLayerSurface(LayerSurface *l);
    const QList<LayerSurface *> &layerSurfaces() const { return m_layers; }
    LayerSurface *layerSurface(const QString &nameSpace) const;
    void arrangeLayers();

    // ---- the windows
    const QList<Window *> &windows() const { return m_windows; }   // bottom to top
    Window *window(uint32_t id) const;
    Window *active() const { return m_active; }
    Window *windowAt(const QPointF &p) const;
    void addWindow(Window *w);      // mapped: placed, framed, made key
    void removeWindow(Window *w);
    void activate(Window *w);       // forward, unminimized, key
    void raise(Window *w);
    void focus(Window *w);          // key (the keyboard), or nothing
    void minimize(Window *w, bool hide = false);
    void unminimize(Window *w);
    void hideApplication(Window *w);   // ⌘H: all its application's windows
    void quitApplication(Window *w);   // ⌘Q for an application that doesn't answer it itself
    void windowChanged(Window *w);
    // where a new window goes (WindowServer's cascade)
    QPoint placeWindow(Window *w, const QSize &frame);
    // interactive moves and resizes (the title bar's drag, xdg_toplevel.move and resize)
    void startMove(Window *w);
    void startResize(Window *w, Qt::Edges edges);
    // the window's frame as it is drawn now, without its shadow (async: the next frame's)
    void grab(Window *w, std::function<void(const QImage &)> done);
    // the screen, as it is drawn (async)
    void grabScreen(std::function<void(const QImage &)> done);
    // the Dock's settings: genie or scale, and how long
    bool minimizeWithScale(double *seconds) const;
    Qt::KeyboardModifiers modifiers() const { return m_modifiers; }

    // ---- the application in front without a window (sl_window_server_v1.set_front_app)
    void setFrontApp(QWaylandClient *client, uint32_t pid);
    uint32_t frontAppPid() const { return m_frontPid; }
    uint32_t frontAppSerial() const { return m_frontSerial; }
    // the key equivalents an application's menus answer (they reach it instead of WindowServer's)
    void setKeyEquivalents(QWaylandClient *client, const QList<uint32_t> &keysyms);

    bool eventFilter(QObject *o, QEvent *e) override;

signals:
    void windowsChanged();     // added, removed or restacked
    void activeChanged();
    void workAreaChanged();
    void frontAppChanged();

private:
    void toplevelCreated(XdgToplevel *t);
    void popupCreated(XdgPopup *p);
    void placePopup(XdgPopup *p);
    void dismissPopups();
    void handleCursor(QWaylandSurface *surface, int hotX, int hotY);
    void updateCursor();
    bool handleKey(QKeyEvent *e);
    bool handleMouse(QEvent *e);
    void paintDesktopPicture();
    void restack();

    QQuickWindow *m_window = nullptr;
    QWaylandQuickCompositor *m_compositor = nullptr;
    QWaylandOutput *m_output = nullptr;
    QQuickItem *m_root = nullptr;
    QQuickItem *m_levels[Levels] = {};
    XdgShell *m_xdgShell = nullptr;
    LayerShell *m_layerShell = nullptr;
    ShellProtocol *m_shell = nullptr;
    Effects *m_effects = nullptr;
    Xwayland *m_xwayland = nullptr;
    QList<LayerSurface *> m_layers;
    QList<Window *> m_windows;
    QPointer<Window> m_active;
    QRect m_workArea;
    QList<QPointer<XdgPopup>> m_grabs;   // the popups holding the grab, the topmost last
    int m_cascade = 0;
    uint32_t m_nextId = 1;

    enum Drag { NoDrag, Moving, Resizing } m_drag = NoDrag;
    QPointer<Window> m_dragWindow;
    QPointF m_dragStart;
    QRect m_dragGeometry;
    Qt::Edges m_dragEdges;
    Qt::KeyboardModifiers m_modifiers;

    QPointer<QWaylandSurface> m_cursorSurface;
    QPoint m_cursorHotspot;

    QPointer<QWaylandClient> m_frontClient;
    uint32_t m_frontPid = 0, m_frontSerial = 0;
    QHash<QWaylandClient *, QList<uint32_t>> m_keyEquivalents;
    friend class Window;
};
