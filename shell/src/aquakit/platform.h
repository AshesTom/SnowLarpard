// SPDX-License-Identifier: LGPL-2.1-or-later
// AquaKit's window systems: Wayland (xdg-shell; AquaKit draws 10.6's title bar itself, as no
// WindowServer is there) and X11 (WindowServer draws it). A platform makes the windows' surfaces,
// gives cairo something to draw a whole frame on, and turns its input into Window::native* calls.
#pragma once

#include "appkit.h"

#include <memory>

namespace AK {

class NativeWindow {
public:
    virtual ~NativeWindow() = default;
    virtual void setTitle(const std::string &title) = 0;
    virtual void setIcon(const Image &) {}
    virtual void show() = 0;
    virtual void hide() = 0;
    // a frame: the whole surface, the title bar AquaKit draws included; null while it can't be drawn
    // (not configured yet, or the compositor hasn't shown the last one)
    virtual cairo_t *beginPaint() = 0;
    virtual void endPaint(cairo_t *cr) = 0;
    virtual Point toScreen(Point p) const { return p; }
    virtual void grabInput() {}
    // the toolbar's height under the title bar (0 hidden), and whether the title bar has the button
    // that shows and hides it
    virtual void setToolbar(int, bool) {}
    virtual void setProxyIcon(const Image &) {}
    virtual void setFloatingPanel() {}
    virtual void startMove() {}
    virtual void setBorderless() {}            // no frame from the window manager (an HUD panel's own)
    virtual bool moveTo(Point) { return false; }   // the surface's top left on the screen (X11)
    virtual void minimize() {}
    virtual void resize(Size surface) = 0;
    // the user may resize it, down to minSurface (the title bar included)
    virtual void setResizable(Size minSurface) = 0;
    virtual void startResize() {}   // the size box dragged (Wayland: the compositor resizes)
    virtual void setRoundedBottomCorners(bool) {}
    // -[NSWindow setAlphaValue:]: the whole window's opacity, 0 to 1 (the compositor's)
    virtual void setOpacity(double) {}
    // -[NSWindow setLevel:] kCGFloatingWindowLevelKey: kept over the other windows, still one of the
    // application's (in the Dock's list), shown or not
    virtual void setAbove(bool) {}
    // the Command keys (keysyms) the application's menus answer, which WindowServer then leaves to it
    virtual void setKeyEquivalents(const std::vector<uint32_t> &) {}
};

// A window's outline (CoreUI's CreateWindowPath): its top corners rounded, radius 4, and its bottom
// ones too with kCUIWindowFrameRoundedBottomCornersKey (a textured window's)
void windowPath(cairo_t *cr, Size size, bool roundedBottom);
constexpr double WindowCornerRadius = 4;

class Platform {
public:
    virtual ~Platform() = default;
    virtual const char *name() const = 0;
    virtual bool drawsTitleBars() const = 0;   // no window manager draws them (Wayland)
    // `surface` is the whole surface (the title bar included); a menu's `at` is in its parent's surface
    virtual std::unique_ptr<NativeWindow> createWindow(Window *w, Size surface, Window::Kind kind, Window *parent, Point at) = 0;
    virtual int fd() const = 0;
    virtual void beforePoll() {}            // Wayland: prepare to read, flush
    virtual bool hasPending() const { return false; }   // events to handle without waiting
    virtual bool afterPoll(bool readable) = 0;   // read and dispatch; false when the connection is gone
    virtual void flush() = 0;
    virtual Rect screenFrame() const = 0;
    // the screen less the menu bar and the Dock (their struts: _NET_WORKAREA); the screen when unknown
    virtual Rect visibleFrame() const { return screenFrame(); }
    // The application in front without a window (a Mac application keeps its menu bar with none
    // open): the root's _SNOWLEOPARD_FRONT_APP, [pid, serial], which the menu bar and the Dock read;
    // false takes it off (when it is this application's) or, quitting, lets them look again
    virtual void setFrontWithoutWindows(bool) {}

    static std::unique_ptr<Platform> wayland();   // null without a compositor (or without the backend built)
    static std::unique_ptr<Platform> x11();       // null without a display
};

} // namespace AK
