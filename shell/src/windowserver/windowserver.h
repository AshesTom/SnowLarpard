// SPDX-License-Identifier: LGPL-2.1-or-later
// WindowServer: the session's window manager. Each application window gets 10.6's title bar (the
// gradient measured on 10.6, CoreUI's close, minimize and zoom buttons from ArtFile.bin); the Dock,
// the menu bar and the Finder's desktop stay as they are, in their layers. EWMH for the shell's
// programs: the client list, the active window, the struts' work area, minimizing, zooming, full
// screen. Command (Super) + W, M, H, Q and Tab as on the Mac; ⇧⌘3 and ⇧⌘4 (with or without ⌃), the
// screen shots' symbolic hot keys, for SystemUIServer.
#pragma once

#include "compositor.h"
#include "frame.h"

#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QVector>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

class WindowServer : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    static constexpr int TitleHeight = Frame::TitleHeight;
    static constexpr int UtilityTitleHeight = Frame::UtilityTitleHeight;

    WindowServer();
    // false when another window manager has the display
    bool start();
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;

private:
    enum Button { None = Frame::None, Close = Frame::Close, Minimize = Frame::Minimize, Zoom = Frame::Zoom, ToolbarButton = Frame::ToolbarButton };
    struct Client {
        xcb_window_t window = 0, frame = 0, grip = 0;
        QRect geometry;          // the client's, in root coordinates (the title bar is above it)
        QString title;
        bool decorated = true;
        bool buttons = true;     // the close, minimize and zoom buttons (Motif hints' functions)   // false: the application draws its own (Motif hints, no decorations)
        int unifiedToolbar = 0;  // a toolbar continuing the title bar (_SNOWLEOPARD_UNIFIED_TOOLBAR): its height
        bool hasToolbar = false; // its second value: the window has a toolbar, shown or not (the toolbar button)
        QImage proxyIcon;        // the represented file's icon before the title (_SNOWLEOPARD_PROXY_ICON)
        bool documentEdited = false;  // the close button's dot (_SNOWLEOPARD_DOCUMENT_EDITED)
        bool roundedBottom = false;   // a textured window's bottom corners (_SNOWLEOPARD_ROUNDED_BOTTOM)
        bool outlined = false;   // a frame's outline though undecorated (the property set: iTunes' own frame)
        bool minimized = false, fullscreen = false, zoomed = false;
        bool hidden = false;     // minimized by Hide (⌘H): no tile in the Dock, no genie
        bool utility = false;    // a utility window's small title bar (_NET_WM_WINDOW_TYPE_UTILITY)
        bool floating = false;   // a panel kept over the windows (_NET_WM_STATE_ABOVE, a utility window)
        double opacity = 1;      // _NET_WM_WINDOW_OPACITY (a translucent Stickies note), given to the compositor
        QVector<uint32_t> keptStates;   // its own states WindowServer keeps (above, skip taskbar, sticky)
        QRect restore;           // before zooming or full screen
        bool deleteWindow = false, takeFocus = false, acceptsInput = true;
        int ignoreUnmaps = 0;
        QSize minimum, maximum;   // WM_NORMAL_HINTS' PMinSize, PMaxSize (empty: none)
        xcb_window_t transientFor = 0;
        int hover = None, pressed = None;
        bool overButtons = false;
        int titleHeight() const { return decorated && !fullscreen ? (utility ? UtilityTitleHeight : TitleHeight) : 0; }
        int space = 0;            // its space, from 0 (CGSGetWindowWorkspace)
        bool allSpaces = false;   // on every space (_NET_WM_STATE_STICKY, _NET_WM_DESKTOP 0xFFFFFFFF)
    };

    void manage(xcb_window_t w, bool existing);
    void unmanage(Client *c, bool destroyed);
    Client *find(xcb_window_t w);
    void readProperties(Client *c);
    void readSizeHints(Client *c);   // WM_NORMAL_HINTS' smallest and largest sizes
    void applyShape(Client *c);
    void place(Client *c, bool hasPosition);
    void applyGeometry(Client *c);
    void sendConfigureNotify(Client *c);
    void paintTitle(Client *c);
    Frame::TitleBar titleBar(const Client *c) const;
    int buttonAt(const Client *c, QPoint p) const;
    QRect buttonRect(const Client *c, int button) const;

    void focus(Client *c);
    void raise(Client *c);
    void restackLayers();
    void minimize(Client *c, bool hide = false);
    void unminimize(Client *c);
    void toggleZoom(Client *c);
    void setFullscreen(Client *c, bool on);
    void close(Client *c);
    void setWmState(Client *c, uint32_t state);
    QRect frameRect(const Client *c) const;
    QRect iconGeometry(const Client *c) const;
    Compositor::Effect minimizeEffect(double *seconds) const;
    void publishPreview(Client *c, QImage image);
    void publishCapture(Client *c, bool shadow, xcb_window_t requestor);
    void setNetState(Client *c);

    void updateClientLists();
    void updateWorkArea();
    void setActiveProperty(xcb_window_t w);
    void grabKeys();
    void paintDesktopPicture();
    void handleKey(xcb_key_press_event_t *e);
    xcb_window_t frontAppKeyboard();   // the window of the application in front without one, when it has the keyboard

    // ---- Spaces (spaces.cpp): the window server's workspaces as the Dock drives them on 10.6
    void readSpacesPrefs();
    int spaceCount() const { return m_rows * m_cols; }
    bool onCurrentSpace(const Client *c) const { return c->allSpaces || c->space == m_space; }
    void publishSpaces();
    void setClientSpace(Client *c, int space);
    void switchSpace(int target, Client *carried = nullptr);
    void switchSpaceBy(int direction);       // the Dock's 1 left, 2 right, 3 up, 4 down
    bool activateSpaceFor(Client *c);        // a window of another space activated: true when switched to it
    bool spacesKey(xcb_keysym_t sym, uint16_t state);
    void grabSpacesKeys(bool grab);
    void stepSlide();
    void finishSlide();
    void showBezel(int from, int to);
    void stepBezel();
    QImage bezelPanel() const;
    QRect bezelCell(int space) const;

    void buttonPress(xcb_button_press_event_t *e);
    void buttonRelease(xcb_button_release_event_t *e);
    void motion(xcb_motion_notify_event_t *e);
    void clientMessage(xcb_client_message_event_t *e);
    void configureRequest(xcb_configure_request_event_t *e);

    xcb_connection_t *m_conn = nullptr;
    xcb_window_t m_root = 0, m_check = 0;
    xcb_gcontext_t m_gc = 0;
    uint8_t m_depth = 24;
    QRect m_screen, m_workArea;
    QVector<Client *> m_clients;               // bottom to top
    QHash<xcb_window_t, Client *> m_byWindow;  // client, frame and grip windows
    QVector<xcb_window_t> m_docks, m_desktops; // unmanaged layers
    Client *m_active = nullptr;
    xcb_key_symbols_t *m_keySymbols = nullptr;

    // a move or a resize in progress
    enum Drag { NoDrag, Moving, Resizing, ButtonDown } m_drag = NoDrag;
    Client *m_dragClient = nullptr;
    QPoint m_dragStart;
    QRect m_dragGeometry;
    QElapsedTimer m_lastTitleClick;
    xcb_window_t m_lastTitleWindow = 0;
    int m_cascade = 0;
    Compositor m_compositor;   // the windows put together, with their shadows

    // Spaces
    bool m_spacesEnabled = false, m_wrapArrows = true, m_swooshOff = false, m_switchOnActivate = true;
    int m_rows = 1, m_cols = 1, m_space = 0;
    uint16_t m_switchModifier = XCB_MOD_MASK_CONTROL, m_directModifier = XCB_MOD_MASK_CONTROL;
    bool m_spacesKeysGrabbed = false;
    struct Slide {
        int from = -1, to = -1;
        QPoint delta;              // the old space's windows go by it, the new one's come from minus it
        QVector<Client *> out, in;
        Client *carried = nullptr;
        QElapsedTimer clock;
        bool running = false;
    } m_slide;
    QTimer m_slideTimer;
    struct Bezel {
        int from = 0, to = 0;      // the indicator's cells
        QElapsedTimer clock;       // since it showed; the indicator moves, then it fades
        double hideAt = -1;        // when it starts fading, in seconds from clock
        bool shown = false;
    } m_bezel;
    QTimer m_bezelTimer;
    QImage m_bezelPanel;
};
