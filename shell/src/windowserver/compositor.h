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
#include <QPolygon>
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
    // A top-level window drawn at this opacity (-[NSWindow setAlphaValue:]), 1 for opaque
    void setOpacity(xcb_window_t frame, double opacity);
    // What shows where no window is (the root's background pixmap: the desktop picture, until the
    // Finder's desktop covers it), instead of black; 0 for black
    void setBackground(xcb_pixmap_t pixmap);

    // Minimizing, as 10.6's Dock has the window server do it (WAWindow genie:direction:genie: and
    // setSystemFrame:from:, docs/reverse-status.md): the window's picture kept when it goes, drawn
    // warped into its Dock tile and back out of it.
    enum Effect { Genie, Scale };
    // the frame as it is now, kept under `key` (the client window) until dropped; its pixels
    // (premultiplied ARGB), for the Dock's tile
    QImage snapshot(xcb_window_t key, xcb_window_t frame);
    // A window's picture as CGSHWCaptureWindowList gives screencapture: its frame inside its
    // outline, over its shadow when `shadow` (the shadow's room around it, transparent); *offset: where
    // the frame's top left is in the picture
    QImage capture(xcb_window_t frame, bool shadow, QPoint *offset = nullptr);
    void dropSnapshot(xcb_window_t key);
    // `window` is the frame's place, `target` the tile's. Minimizing with waitForTarget, the clock
    // starts when the Dock gives the new tile's place (retarget), or after 150 ms
    void animate(xcb_window_t key, xcb_window_t frame, const QRect &window, const QRect &target, Effect effect, bool minimizing,
                 double seconds, bool waitForTarget);
    void retarget(xcb_window_t key, const QRect &target);

    // Spaces: a window drawn moved by `offset` (the Dock's switch slides the windows of the two spaces
    // with CGS window transforms, the windows themselves not moved); clearOffsets() puts them all back
    void setOffset(xcb_window_t frame, QPoint offset);
    void clearOffsets();
    bool hasOffsets() const { return !m_offsets.isEmpty(); }
    // a picture over everything at `at` (Spaces' bezel, WVBezelLayer's window at the overlay level),
    // at `opacity`; a null image takes it away
    void setOverlay(const QImage &image, QPoint at, double opacity);

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
        // The Quartz magic mirror it asks for (the Dock's floor, _SNOWLEOPARD_MAGIC_MIRROR): its
        // direction (CGSSetMagicMirror's: 1 bottom, 2 top, 3 left, 4 right; 0 none, -1 not read yet)
        // and the quadrangle, in the window's coordinates
        int mirror = -1;
        QPolygon mirrorQuad;
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
    // The magic mirror (CoreGraphics' Server/Windows/MagicMirror.c): under the window that asked for
    // it, what is drawn beside its quadrangle, turned over that side's edge
    void readMirror(Win *w);
    QRect mirrorTarget(const Win *w) const;   // where it is drawn, in root coordinates
    QRect mirrorSource(const Win *w) const;   // what it shows
    void damageMirror(const Win *w);
    void drawMirror(const Win *w);

    xcb_connection_t *m_conn = nullptr;
    xcb_window_t m_root = 0, m_overlay = 0, m_owner = 0;
    QRect m_screen;
    ShadowFor m_shadowFor;
    int m_damageEvent = -1, m_shapeEvent = -1;
    xcb_render_pictformat_t m_a8 = 0, m_rootFormat = 0;
    QHash<xcb_visualid_t, xcb_render_pictformat_t> m_visualFormats;
    QHash<xcb_render_pictformat_t, bool> m_formatAlpha;
    xcb_render_picture_t m_overlayPicture = 0, m_buffer = 0, m_black = 0, m_background = 0;
    uint8_t m_depth = 24;
    xcb_atom_t m_mirrorAtom = 0;
    xcb_xfixes_region_t m_damage = 0, m_scratch = 0;
    QHash<xcb_window_t, Win *> m_windows;
    QVector<Win *> m_order;   // bottom to top
    bool m_treeDirty = true, m_all = true;
    QTimer m_timer;
    xcb_render_pictformat_t m_argb32 = 0;
    QHash<xcb_window_t, Snapshot> m_snapshots;
    QHash<xcb_window_t, double> m_opacity;            // the windows not opaque
    QHash<int, xcb_render_picture_t> m_opacityMasks;  // a solid alpha for each, in 1/255
    QVector<Animation> m_animations;
    QTimer m_animTimer;
    xcb_render_picture_t opacityMask(double opacity);   // a repeated 1 x 1 A8 picture, none for opaque
    QHash<xcb_window_t, QPoint> m_offsets;
    xcb_render_picture_t m_overlayImage = 0;
    QRect m_overlayRect;
    double m_overlayOpacity = 1;
    qint64 m_overlayKey = 0;
};
