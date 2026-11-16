// SPDX-License-Identifier: LGPL-2.1-or-later
// WindowServer's compositor: every window of the display is drawn off screen (Composite) and put
// together here (Render), bottom to top, each over its shadow. The shadows are 10.6's (AppKit's
// __NSSetWindowShadow, docs/reverse-status.md): the key window's, another window's, a menu's.
// Being the display's compositing manager (_NET_WM_CM_S0) also makes the Dock, its stacks and the
// menus really translucent.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
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
    bool running() const { return m_overlay != 0; }

    // Minimizing, as 10.6's Dock has the window server do it (WAWindow genie:direction:genie: and
    // setSystemFrame:from:, docs/reverse-status.md): the window's picture kept when it goes, drawn
    // warped into its Dock tile and back out of it.
    enum Effect { Genie, Scale };
    // the frame as it is now, kept under `key` (the client window) until dropped; its pixels
    // (premultiplied ARGB), for the Dock's tile
    QImage snapshot(xcb_window_t key, xcb_window_t frame);
    void dropSnapshot(xcb_window_t key);
    // `window` is the frame's place, `target` the tile's. Minimizing with waitForTarget, the clock
    // starts when the Dock gives the new tile's place (retarget), or after 150 ms
    void animate(xcb_window_t key, xcb_window_t frame, const QRect &window, const QRect &target, Effect effect, bool minimizing,
                 double seconds, bool waitForTarget);
    void retarget(xcb_window_t key, const QRect &target);

private:
    struct Snapshot {
        xcb_render_picture_t picture = 0;
        QSize size;
    };
    struct Animation {
        xcb_window_t key = 0, frame = 0;
        QRect window, target, drawn;   // drawn: where the last frame went, to be drawn over
        Effect effect = Genie;
        bool minimizing = true, started = false;
        double seconds = 0.5;
        QElapsedTimer clock;           // since the animation started, or since it waits for its target
    };
    void stepAnimations();
    QRect drawAnimation(const Animation &a, const Snapshot &s);

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
    xcb_render_pictformat_t m_argb32 = 0;
    QHash<xcb_window_t, Snapshot> m_snapshots;
    QVector<Animation> m_animations;
    QTimer m_animTimer;
};
