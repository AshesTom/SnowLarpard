// SPDX-License-Identifier: LGPL-2.1-or-later
// AquaKit: AppKit's skeleton for the shell's Mac applications, without Qt. An application on Wayland
// or X11 (platform.h), windows drawn whole with cairo into a back buffer, views in a tree with frames in their
// parent's flipped coordinates (top left origin, as NSView with isFlipped), the mouse tracked by the
// view it went down in (as -mouseDown: / -mouseDragged: / -mouseUp:), timers, and the first responder
// for the keyboard. The controls (controls.h) draw with CoreUI (coreui.h).
#pragma once

#include "graphics.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AK {

class View;
class Window;

enum Modifier : unsigned { Shift = 1, Control = 4, Option = 8, Command = 64 };

struct Event {
    enum Type { MouseDown, MouseUp, MouseDragged, MouseMoved, MouseExited, KeyDown, ScrollWheel } type = MouseMoved;
    Point location;        // in the view's coordinates
    Point screen;          // on the screen
    int clickCount = 1;
    unsigned modifiers = 0;
    uint32_t keysym = 0;   // KeyDown
    std::string text;      // KeyDown: what the key types
    double deltaY = 0;     // ScrollWheel: lines, down positive
};

class View {
public:
    View() = default;
    virtual ~View();
    View(const View &) = delete;
    View &operator=(const View &) = delete;

    template <typename T, typename... Args> T *add(Args &&...args)
    {
        auto *v = new T(std::forward<Args>(args)...);
        addSubview(v);
        return v;
    }
    void addSubview(View *v);   // takes it
    void removeSubview(View *v);   // deletes it
    void removeAllSubviews();
    const std::vector<View *> &subviews() const { return m_subviews; }
    View *superview() const { return m_parent; }
    Window *window() const;

    Rect frame() const { return m_frame; }
    Rect bounds() const { return {0, 0, m_frame.w, m_frame.h}; }
    virtual void setFrame(const Rect &r);
    bool isHidden() const { return m_hidden; }
    void setHidden(bool h);
    bool isEnabled() const { return m_enabled; }
    virtual void setEnabled(bool e);
    void setNeedsDisplay();
    Point toWindow(Point p) const;
    Point fromWindow(Point p) const;
    bool windowIsKey() const;

    virtual void draw(cairo_t *) {}
    virtual View *hitTest(Point inSuperview);
    virtual void mouseDown(Event &) {}
    virtual void mouseDragged(Event &) {}
    virtual void mouseUp(Event &) {}
    virtual void mouseMoved(Event &) {}
    virtual void mouseExited(Event &) {}
    virtual void scrollWheel(Event &e);   // passes it up
    virtual void keyDown(Event &e);       // passes it up
    virtual bool acceptsFirstResponder() const { return false; }
    virtual void focusChanged() { setNeedsDisplay(); }
    bool isFirstResponder() const;
    bool clipsToBounds = true;

protected:
    friend class Window;
    void render(cairo_t *cr);

private:
    View *m_parent = nullptr;
    Window *m_window = nullptr;   // the root view's
    std::vector<View *> m_subviews;
    Rect m_frame;
    bool m_hidden = false, m_enabled = true;
};

class NativeWindow;
class Platform;

class Window {
public:
    enum Kind { Titled, Menu, FullScreen };
    // A menu opens over `parent`, its top left at `at` in the parent's content
    Window(Size size, const std::string &title, Kind kind = Titled, Window *parent = nullptr, Point at = {});
    virtual ~Window();
    View *contentView() const { return m_content.get(); }
    void setTitle(const std::string &title);
    void setIcon(const Image &icon);
    void show();
    void close();
    bool isKey() const { return m_key; }
    bool isVisible() const { return m_visible; }
    Size size() const { return m_size; }
    // a new content size (System Preferences fits each pane); the top left stays
    void setContentSize(Size size);
    // A window the user resizes (a document's), its content no smaller than minContent; the size box
    // in its bottom right corner (WindowServer's on X11, AquaKit's on Wayland). onResize after each.
    void setResizable(Size minContent);
    bool isResizable() const { return m_resizable; }
    std::function<void(Size)> onResize;
    Kind kind() const { return m_kind; }
    Point toScreen(Point inWindow) const;
    // The title bar AquaKit draws itself where no window manager does (Wayland): its height
    double titleBarHeight() const { return m_titleBar; }
    NativeWindow *native() const { return m_native.get(); }

    View *firstResponder() const { return m_firstResponder; }
    void makeFirstResponder(View *v);
    void invalidate();
    // The toolbar under the title bar, in one gradient with it (its height in points): WindowServer's
    // property on X11, AquaKit's own title bar on Wayland
    void setUnifiedToolbarHeight(int height);
    // The toolbar button at the right of the title bar ([NSWindow setShowsToolbarButton:]); clicking it
    // calls onToggleToolbar
    void setShowsToolbarButton(bool shows);
    std::function<void()> onToggleToolbar;
    // The represented file's icon before the title ([NSWindow setRepresentedURL:]'s proxy icon)
    void setRepresentedIcon(const Image &icon);
    // A floating panel (loginwindow's About This Mac: CGWindowLevelForKey(kCGModalPanelWindowLevelKey),
    // on every Space): above the windows, not in the Dock; before show()
    void setFloatingPanel();
    // Rounded bottom corners, as a textured window's (the Finder's): the top ones always are
    void setRoundedBottomCorners(bool rounded);
    void grabInput();   // a menu, a full screen screen saver: every click and key comes here
    std::function<void()> onClose;   // the close button, or close()
    std::function<void(bool)> onKeyChanged;
    std::function<void(Event &)> onUnhandledKey;   // a key no view took (Command-J...)
    std::function<void(Event &)> onOutsideClick;   // with grabInput: a click outside the window

    // From the platform (platform.h), in the surface's coordinates (the title bar's included)
    void nativeButton(bool press, Point p, Point screen, unsigned modifiers, uint32_t timeMs);
    void nativeMotion(Point p, Point screen);
    void nativeLeave();
    void nativeScroll(Point p, double lines);
    void nativeKey(uint32_t keysym, const std::string &text, unsigned modifiers);
    void nativeFocus(bool key);
    void nativeClose();     // the window manager's close
    void nativeToggleToolbar() { if (onToggleToolbar) onToggleToolbar(); }   // its toolbar button
    void nativeDismiss();   // a menu's outside click (Wayland's popup_done)
    void nativeExpose() { invalidate(); }
    void nativeResize(Size surface);   // the window manager's new size
    void redraw();

private:
    friend class View;
    void dispatchMouse(Event &e, Point inWindow, bool press, bool release, bool motion);
    void drawTitleBar(cairo_t *cr);
    void drawSizeBox(cairo_t *cr);
    int titleButtonAt(Point p) const;

    Kind m_kind;
    Size m_size;
    std::string m_title;
    std::unique_ptr<View> m_content;
    std::unique_ptr<NativeWindow> m_native;
    View *m_firstResponder = nullptr;
    View *m_tracking = nullptr, *m_hover = nullptr;
    bool m_key = false, m_visible = false, m_dirty = true;
    double m_titleBar = 0;
    int m_unified = 0;
    bool m_toolbarButton = false;
    Image m_proxyIcon;
    int m_titlePressed = -1;           // the title bar's button held down
    bool m_resizable = false;
    Size m_minSize;
    bool m_overTitleButtons = false;
    Point m_lastClick;
    uint32_t m_lastClickTime = 0;
    int m_clickCount = 0;
};

class App {
public:
    static App &shared();
    bool open();   // Wayland when there is a compositor (AQUAKIT_PLATFORM=x11 to choose X11), else X11
    int run();
    void quit(int code = 0);
    Rect screenFrame() const;
    Rect visibleFrame() const;   // without the menu bar and the Dock ([NSScreen visibleFrame])
    Platform *platform() const { return m_platform.get(); }
    std::string platformName() const;

    // timers, in seconds; the callback may remove its own
    int addTimer(double seconds, bool repeat, std::function<void()> fire);
    void removeTimer(int id);
    void later(std::function<void()> f) { addTimer(0, false, std::move(f)); }

private:
    friend class Window;
    App() = default;
    std::unique_ptr<Platform> m_platform;
    std::vector<Window *> m_windows;
    struct Timer {
        int id;
        double due, interval;
        bool repeat;
        std::function<void()> fire;
    };
    std::vector<Timer> m_timers;
    int m_nextTimer = 1;
    bool m_running = false;
    int m_code = 0;
};

double now();   // seconds, monotonic

} // namespace AK
