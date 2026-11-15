// SPDX-License-Identifier: LGPL-2.1-or-later
// WindowServer's compositor: every window of the display is drawn off screen (Composite) and put
// together here (Render), bottom to top, each over its shadow. The shadows are 10.6's (AppKit's
// __NSSetWindowShadow, docs/reverse-status.md): the key window's, another window's, a menu's.
// Being the display's compositing manager (_NET_WM_CM_S0) also makes the Dock, its stacks and the
// menus really translucent.
#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QVector>
#include <functional>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

class Compositor : public QObject {
public:
    enum Shadow { NoShadow, KeyShadow, WindowShadow, PanelShadow };
    // A top-level window's shadow for the window manager's own frames; -1 for the others (theirs is
    // by their window type: a menu's, or none)
    using ShadowFor = std::function<int(xcb_window_t)>;

    // false when the display can't composite or another compositor has it
    bool start(xcb_connection_t *conn, xcb_window_t root, ShadowFor shadowFor);
    // every event of the display goes through here; none is kept
    void handleEvent(xcb_generic_event_t *ev);
    // everything again (the key window changed)
    void damageAll();

private:
    struct Win {
        xcb_window_t id = 0;
        QRect geometry;                 // in root coordinates, inside its border
        bool mapped = false, inputOnly = false, alpha = false, destroyed = false;
        int menu = -1;                  // by its window type; -1: not read yet
        xcb_render_picture_t picture = 0;
        uint32_t damage = 0;
        xcb_xfixes_region_t shape = 0;  // its outline, in its own coordinates; 0: to be read
        xcb_render_picture_t shadow = 0;
        int shadowKind = NoShadow;
        QRect shadowRect;               // the shadow's place around the window, in the window's coordinates
    };

    bool findFormats(xcb_visualid_t rootVisual);
    Win *add(xcb_window_t id);
    void release(Win *w);
    void readTree();
    int shadowKind(Win *w);
    void makeShadow(Win *w, int kind);
    void dropShadow(Win *w);
    void damageRect(QRect r);
    void damageWindow(const Win *w);
    void schedule();
    void paint();

    xcb_connection_t *m_conn = nullptr;
    xcb_window_t m_root = 0, m_overlay = 0, m_owner = 0;
    QRect m_screen;
    ShadowFor m_shadowFor;
    int m_damageEvent = -1, m_shapeEvent = -1;
    xcb_render_pictformat_t m_a8 = 0, m_rootFormat = 0;
    QHash<xcb_visualid_t, xcb_render_pictformat_t> m_visualFormats;
    QHash<xcb_render_pictformat_t, bool> m_formatAlpha;
    xcb_render_picture_t m_overlayPicture = 0, m_buffer = 0, m_black = 0;
    xcb_xfixes_region_t m_damage = 0, m_scratch = 0;
    QHash<xcb_window_t, Win *> m_windows;
    QVector<Win *> m_order;   // bottom to top
    bool m_treeDirty = true, m_all = true;
    QTimer m_timer;
};
