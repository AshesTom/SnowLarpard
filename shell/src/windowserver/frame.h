// SPDX-License-Identifier: LGPL-2.1-or-later
// What both window servers draw the same way, the X11 one (WindowServer) and the Wayland one
// (WindowServer-wayland): 10.6's title bar and its buttons, the windows' shadows (AppKit's
// __NSSetWindowShadow, CGXWindowShadowGet), and the genie's mesh (the Dock's).
#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>
#include <vector>

namespace Frame {

constexpr int TitleHeight = 22;
// a utility window's (NSUtilityWindowMask: loginwindow's About This Mac): a 10.6 screenshot's rows,
// 242, 237 down to 217 over 14, a 166 line, a 217 row
constexpr int UtilityTitleHeight = 17;
constexpr int GripSize = 15;   // the size box's corner, where dragging resizes
constexpr int MinimumWidth = 3 * 21 + 40;   // the three buttons and room for a title

enum Button { None = -1, Close, Minimize, Zoom, ToolbarButton };

// A title bar as it is to be drawn
struct TitleBar {
    int width = 0;
    QString title;
    bool active = false;
    bool utility = false;
    bool buttons = true;          // the close, minimize and zoom buttons (only the close one without)
    bool hasToolbar = false;      // the toolbar button at the right
    int unifiedToolbar = 0;       // a toolbar continuing the title bar: its height
    QImage proxyIcon;             // the represented file's icon before the title
    bool documentEdited = false;  // the close button's dot
    int pressed = None;
    bool overButtons = false;     // the pointer over the three: their glyphs shown
    int height() const { return utility ? UtilityTitleHeight : TitleHeight; }
};

QRect buttonRect(const TitleBar &t, int button);
int buttonAt(const TitleBar &t, QPoint p);
QImage paintTitleBar(const TitleBar &t);
// The unified gradient (measured on 10.6's System Preferences): row 0 of the title bar to the
// toolbar's end
QColor unifiedToolbarColor(int row, int totalHeight, bool active);

// ---- shadows

enum Shadow { NoShadow, KeyShadow, WindowShadow, PanelShadow };
// the room any shadow takes around its window (the key window's: a tent of 34 and 15 under)
constexpr int ShadowReach = 52;

// The shadow of a window as an alpha mask (Format_Alpha8), from the window's outline (0 to 255,
// `width` x `height`): *origin is where the mask's top left is from the window's
QImage shadowMask(const std::vector<uint8_t> &outline, int width, int height, int kind, QPoint *origin);
// the outline of a plain window (its top corners rounded, 4, and its bottom ones with roundedBottom)
std::vector<uint8_t> windowOutline(int width, int height, bool roundedBottom);
// the outline from a picture's alpha (a menu, a panel with round corners)
std::vector<uint8_t> alphaOutline(const QImage &image);

// ---- the genie

// A row of the genie's mesh: where it is on the screen, how far its left and right edges have come
// in, and which of the window's rows is there
struct GenieRow {
    double y, left, right, local;
};
// The genie at p (0: the window, 1: in its tile), the window's frame and the tile on the screen
// (a tile above the window: the same, upside down)
QVector<GenieRow> genieRows(const QRectF &window, const QRectF &tile, double p);

} // namespace Frame
