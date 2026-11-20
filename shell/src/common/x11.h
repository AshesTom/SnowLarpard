// SPDX-License-Identifier: LGPL-2.1-or-later
// The window server's side of the shell: the clients' windows (EWMH when a window manager runs, the
// root's children when none does), the active one, and the Dock's and the menu bar's own window
// types. On Wayland the same calls go to the Wayland WindowServer (sl_window_server_v1, through
// WindowServerClient: its window numbers stand for the X windows) and the shell's own windows are
// layer shell surfaces (LayerShellQt).
#pragma once

#include <QPolygon>

#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QVector>
#include <xcb/xcb.h>

class QWidget;

namespace X11 {

xcb_connection_t *conn();
xcb_window_t root();
xcb_atom_t atom(const char *name);

// A property's 32-bit items (CARDINAL, WINDOW, ATOM...).
QVector<uint32_t> cardinals(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type);
QByteArray bytes(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type);

bool hasWindowManager();
bool compositing();
// the Wayland session's WindowServer is there (the calls below go to it)
bool wayland();

// The application windows, bottom to top when known.
QVector<xcb_window_t> clientWindows();
// all of them, those out of the Dock too (_NET_CLIENT_LIST: loginwindow's panels)
QVector<xcb_window_t> allClientWindows();
// a panel or a toolbox (a utility or toolbar window): not a Foreground application's window alone
bool isPanel(xcb_window_t w);
// The application in front without a window (_SNOWLEOPARD_FRONT_APP, set_front_app): its pid, 0 for
// none; *serial changes with each request
uint32_t frontApp(uint32_t *serial = nullptr);
// this application in front without a window (the Finder's desktop clicked)
void setFrontApp(bool front);
void flush();
xcb_window_t activeWindow();

QString wmClass(xcb_window_t w);      // the class part ("firefox")
QString wmInstance(xcb_window_t w);   // the instance part
QString title(xcb_window_t w);
QImage icon(xcb_window_t w, int size);
uint32_t pid(xcb_window_t w);
bool minimized(xcb_window_t w);

void activate(xcb_window_t w);
void close(xcb_window_t w);
void minimize(xcb_window_t w);
void hide(xcb_window_t w);            // Hide (⌘H): gone without going into the Dock
bool hidden(xcb_window_t w);          // hidden, not minimized (_SNOWLEOPARD_HIDDEN)
QImage preview(xcb_window_t w);       // a minimized window's picture (_SNOWLEOPARD_WINDOW_PREVIEW)
void setIconGeometry(xcb_window_t w, const QRect &r);   // _NET_WM_ICON_GEOMETRY: where the Dock shows it

// The shell's own windows: the menu bar and the Dock are docks that keep their edge (struts), the
// Finder's desktop is the desktop.
void setDock(QWidget *widget, int strutTop, int strutBottom);
// Without a compositor nothing is see-through: a translucent window (the Dock, its stacks) is shaped
// to what it draws (its pixels a tenth opaque or more), with `keep` (where it must still take the
// pointer); its mask is only set again when it changes
void shapeToContent(QWidget *widget, const QImage &content, const QRegion &keep = {});
void setDesktop(QWidget *widget);
void setMenu(QWidget *widget);
// Quartz's magic mirror (CGSSetMagicMirror, as the Dock asks for it under its glass floor): the
// window server draws under `widget`, inside `quad` (the window's pixels), what is beside it turned
// over: direction 1 bottom, 2 top, 3 left, 4 right; 0 or an empty quad turns it off
// (_SNOWLEOPARD_MAGIC_MIRROR, set only when it changes)
void setMagicMirror(QWidget *widget, int direction, const QPolygon &quad);
// A window whose toolbar continues its title bar (10.6's unified windows): the toolbar's height, for
// WindowServer, which draws the title bar's part of the one gradient (unifiedToolbarColor gives it).
void setUnifiedToolbar(QWidget *widget, int height);
// The unified gradient (measured on 10.6's System Preferences): row 0 of the title bar to the toolbar's end
QColor unifiedToolbarColor(int row, int totalHeight, bool active);

// Tells when the list of windows, the active one or their stacking changed (coalesced).
class Watcher : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit Watcher(QObject *parent = nullptr);
    ~Watcher() override;
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;
signals:
    void changed();
private:
    QTimer m_coalesce;
};

} // namespace X11
