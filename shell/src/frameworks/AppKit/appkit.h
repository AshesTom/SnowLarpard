// SPDX-License-Identifier: LGPL-2.1-or-later
// AquaKit: AppKit's skeleton for the shell's Mac applications, without Qt. An application on Wayland
// or X11 (platform.h), windows drawn whole with cairo into a back buffer, views in a tree with frames in their
// parent's flipped coordinates (top left origin, as NSView with isFlipped), the mouse tracked by the
// view it went down in (as -mouseDown: / -mouseDragged: / -mouseUp:), timers, and the first responder
// for the keyboard. The controls (controls.h) draw with CoreUI (coreui.h).
#pragma once

#include "graphics.h"
#include "runloop.h"

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
    // the right button, or a click with Control alone (-[NSView menuForEvent:]); passes it up
    virtual void rightMouseDown(Event &e);
    virtual void scrollWheel(Event &e);   // passes it up
    virtual void keyDown(Event &e);       // passes it up
    virtual bool acceptsFirstResponder() const { return false; }
    virtual void focusChanged() { setNeedsDisplay(); }
    // -[NSView resetCursorRects]'s cursor over the view, by its freedesktop name ("text" for
    // NSCursor's IBeamCursor, "pointer" for pointingHandCursor); null: its superview's, the arrow at last
    virtual const char *cursor() const { return nullptr; }
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
    // Status: NSStatusBar's system status window, transparent over the menu bar, its items drawn on
    // nothing (X11: a 32-bit window the compositor blends)
    enum Kind { Titled, Menu, FullScreen, Status };
    // A menu opens over `parent`, its top left at `at` in the parent's content
    Window(Size size, const std::string &title, Kind kind = Titled, Window *parent = nullptr, Point at = {});
    virtual ~Window();
    View *contentView() const { return m_content.get(); }
    void setTitle(const std::string &title);
    void setIcon(const Image &icon);
    void show();
    // -[NSWindow makeKeyAndOrderFront:]: shown, or brought forward and made key when it is
    void orderFront();
    void close();
    void miniaturize();   // into the Dock ([NSWindow performMiniaturize:])
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
    // -[NSWindow setDocumentEdited:]: the close button's dot (CoreUI's closedirty)
    void setDocumentEdited(bool edited);
    bool isDocumentEdited() const { return m_edited; }
    // A floating panel (loginwindow's About This Mac: CGWindowLevelForKey(kCGModalPanelWindowLevelKey),
    // on every Space): above the windows, not in the Dock; before show(). A utility window's small
    // title bar (NSUtilityWindowMask), else a regular one (loginwindow's Log Out panel)
    void setFloatingPanel(bool utility = true);
    // -[NSWindow styleMask]'s buttons (NSClosableWindowMask 2, NSMiniaturizableWindowMask 4,
    // NSResizableWindowMask 8): a titled window with none of them has no buttons in its title bar
    // (NSThemeFrame hides the three; loginwindow's Log Out panel, style 129); before show()
    void setStyleMask(unsigned mask);
    // No title bar nor frame: the window draws its own (Quick Look's HUD panel); before show()
    void setBorderless();
    // NSStatusBar's window: borderless, a dock on every desktop above the others, keeping `top` points
    // of the screen (`bottom` of its bottom) out of the other windows' way (X11; Wayland: not yet)
    void setStatusBar(int top, int bottom = 0);
    // Moving it by a drag of its own content: from a mouse down's screen point, to the dragged one
    void dragMove(Point fromScreen, Point toScreen, bool start);
    bool moveTo(Point screenTopLeft);   // X11 only (Wayland places its windows itself): false there
    // Rounded bottom corners, as a textured window's (the Finder's): the top ones always are
    void setRoundedBottomCorners(bool rounded);
    // -[NSWindow setAlphaValue:] (X11: WindowServer's compositor draws it so)
    void setAlphaValue(double alpha);
    double alphaValue() const { return m_alpha; }
    // -[NSWindow setLevel:] between kCGNormalWindowLevelKey and kCGFloatingWindowLevelKey
    void setFloating(bool floating);
    bool isFloating() const { return m_floating; }
    // The window's top left on the screen (the title bar's, when there is one)
    Point screenOrigin() const;
    // Command keys the application's menus give their own meaning (Stickies' ⌘M collapses a note,
    // ⌘Q saves and quits): WindowServer, which minimizes, closes and hides with them, passes them on
    void setKeyEquivalents(const std::vector<uint32_t> &keysyms);
    void grabInput();   // a menu, a full screen screen saver: every click and key comes here
    // the close button, or close(): it is called by close() itself, so a handler that only hides the
    // window must not call close() (none is needed: the close button closes it)
    std::function<void()> onClose;
    std::function<void(bool)> onKeyChanged;
    std::function<void(Event &)> onUnhandledKey;   // a key no view took (Command-J...)
    std::function<void(Event &)> onOutsideClick;   // with grabInput: a click outside the window
    std::function<void(Event &)> onMouseMovedOutside;   // with grabInput: the pointer outside the window

    // From the platform (platform.h), in the surface's coordinates (the title bar's included)
    void nativeButton(bool press, Point p, Point screen, unsigned modifiers, uint32_t timeMs, bool secondary = false);
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
    bool m_edited = false;
    bool m_titleButtons = true;
    int m_titlePressed = -1;           // the title bar's button held down
    bool m_resizable = false;
    bool m_secondaryDown = false;
    double m_alpha = 1;
    bool m_floating = false;
    Point m_dragOrigin;   // dragMove's window origin when it started
    Size m_minSize;
    bool m_overTitleButtons = false;
    std::string m_cursor;   // the cursor set ([NSCursor set]): the view's under the pointer
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
    size_t visibleWindowCount() const;   // the windows shown (a program with none left may quit)
    Rect visibleFrame() const;   // without the menu bar and the Dock ([NSScreen visibleFrame])
    // In front with no window: the menu bar shows its menus, the Dock its light (X11)
    void setFrontWithoutWindows(bool front);
    Platform *platform() const { return m_platform.get(); }
    std::string platformName() const;

    // the main run loop's (Foundation's RunLoop), the display one of its sources: timers, in seconds
    // (the callback may remove its own), and file descriptors watched with the display's
    int addTimer(double seconds, bool repeat, std::function<void()> fire) { return RunLoop::main().addTimer(seconds, repeat, std::move(fire)); }
    void removeTimer(int id) { RunLoop::main().removeTimer(id); }
    void later(std::function<void()> f) { RunLoop::main().later(std::move(f)); }
    void watch(int fd, std::function<void()> readable) { RunLoop::main().watch(fd, std::move(readable)); }
    void unwatch(int fd) { RunLoop::main().unwatch(fd); }

private:
    friend class Window;
    App() = default;
    std::unique_ptr<Platform> m_platform;
    std::vector<Window *> m_windows;
};

} // namespace AK
