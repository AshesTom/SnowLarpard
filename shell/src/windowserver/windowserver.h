// SPDX-License-Identifier: LGPL-2.1-or-later
// WindowServer: the session's window manager. Each application window gets 10.6's title bar (the
// gradient measured on 10.6, CoreUI's close, minimize and zoom buttons from ArtFile.bin); the Dock,
// the menu bar and the Finder's desktop stay as they are, in their layers. EWMH for the shell's
// programs: the client list, the active window, the struts' work area, minimizing, zooming, full
// screen. Command (Super) + W, M, H, Q and Tab as on the Mac.
#pragma once

#include "compositor.h"

#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QVector>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

class WindowServer : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    static constexpr int TitleHeight = 22;
    // a utility window's (NSUtilityWindowMask, _NET_WM_WINDOW_TYPE_UTILITY: loginwindow's About This
    // Mac): a 10.6 screenshot's rows, 242, 237 down to 217 over 14, a 166 line, a 217 row
    static constexpr int UtilityTitleHeight = 17;

    WindowServer();
    // false when another window manager has the display
    bool start();
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;

private:
    enum Button { None = -1, Close, Minimize, Zoom, ToolbarButton };
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
        bool minimized = false, fullscreen = false, zoomed = false;
        bool hidden = false;     // minimized by Hide (⌘H): no tile in the Dock, no genie
        bool utility = false;    // a utility window's small title bar (_NET_WM_WINDOW_TYPE_UTILITY)
        bool floating = false;   // a panel kept over the windows (_NET_WM_STATE_ABOVE, a utility window)
        double opacity = 1;      // _NET_WM_WINDOW_OPACITY (a translucent Stickies note), given to the compositor
        QVector<uint32_t> keptStates;   // its own states WindowServer keeps (above, skip taskbar, sticky)
        QRect restore;           // before zooming or full screen
        bool deleteWindow = false, takeFocus = false, acceptsInput = true;
        int ignoreUnmaps = 0;
        QSize minimum;
        xcb_window_t transientFor = 0;
        int hover = None, pressed = None;
        bool overButtons = false;
        int titleHeight() const { return decorated && !fullscreen ? (utility ? UtilityTitleHeight : TitleHeight) : 0; }
    };

    void manage(xcb_window_t w, bool existing);
    void unmanage(Client *c, bool destroyed);
    Client *find(xcb_window_t w);
    void readProperties(Client *c);
    void applyShape(Client *c);
    void place(Client *c, bool hasPosition);
    void applyGeometry(Client *c);
    void sendConfigureNotify(Client *c);
    void paintTitle(Client *c);
    QImage buttonImage(const Client *c, int button) const;
    int buttonAt(const Client *c, QPoint p) const;
    static QRect buttonRect(const Client *c, int button);

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
    void setNetState(Client *c);

    void updateClientLists();
    void updateWorkArea();
    void setActiveProperty(xcb_window_t w);
    void grabKeys();
    void paintDesktopPicture();
    void handleKey(xcb_key_press_event_t *e);

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
};
