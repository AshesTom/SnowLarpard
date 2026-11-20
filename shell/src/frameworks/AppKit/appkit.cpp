// SPDX-License-Identifier: LGPL-2.1-or-later
#include "appkit.h"

#include "foundation.h"

#include "coreui.h"
#include "platform.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <cstdio>
#include <poll.h>

namespace AK {

// ---- View

View::~View()
{
    if (Window *w = window()) {
        if (w->m_firstResponder == this)
            w->m_firstResponder = nullptr;
        if (w->m_tracking == this)
            w->m_tracking = nullptr;
        if (w->m_hover == this)
            w->m_hover = nullptr;
    }
    // the children still know their window while they go (the focus, the mouse's tracking)
    std::vector<View *> children;
    children.swap(m_subviews);
    for (View *v : children)
        delete v;
}

void View::addSubview(View *v)
{
    v->m_parent = this;
    m_subviews.push_back(v);
    setNeedsDisplay();
}

void View::removeSubview(View *v)
{
    auto it = std::find(m_subviews.begin(), m_subviews.end(), v);
    if (it == m_subviews.end())
        return;
    m_subviews.erase(it);
    delete v;   // its parent pointer kept until it is gone
    setNeedsDisplay();
}

void View::removeAllSubviews()
{
    std::vector<View *> old;
    old.swap(m_subviews);
    for (View *v : old)
        delete v;   // its parent pointer kept: it finds its window while it goes
    setNeedsDisplay();
}

Window *View::window() const
{
    const View *v = this;
    while (v->m_parent)
        v = v->m_parent;
    return v->m_window;
}

void View::setFrame(const Rect &r)
{
    m_frame = r;
    setNeedsDisplay();
}

void View::setHidden(bool h)
{
    if (m_hidden != h) {
        m_hidden = h;
        setNeedsDisplay();
    }
}

void View::setEnabled(bool e)
{
    if (m_enabled != e) {
        m_enabled = e;
        setNeedsDisplay();
    }
}

void View::setNeedsDisplay()
{
    if (Window *w = window())
        w->invalidate();
}

Point View::toWindow(Point p) const
{
    for (const View *v = this; v && v->m_parent; v = v->m_parent)
        p.x += v->m_frame.x, p.y += v->m_frame.y;
    return p;
}

Point View::fromWindow(Point p) const
{
    const Point o = toWindow({0, 0});
    return {p.x - o.x, p.y - o.y};
}

bool View::windowIsKey() const
{
    Window *w = window();
    return w && w->isKey();
}

bool View::isFirstResponder() const
{
    Window *w = window();
    return w && w->firstResponder() == this;
}

View *View::hitTest(Point p)
{
    if (m_hidden || !m_frame.contains(p))
        return nullptr;
    const Point local{p.x - m_frame.x, p.y - m_frame.y};
    for (auto it = m_subviews.rbegin(); it != m_subviews.rend(); ++it)
        if (View *hit = (*it)->hitTest(local))
            return hit;
    return this;
}

void View::scrollWheel(Event &e)
{
    if (m_parent)
        m_parent->scrollWheel(e);
}

void View::keyDown(Event &e)
{
    if (m_parent)
        m_parent->keyDown(e);
    else if (m_window && m_window->onUnhandledKey)   // the window's own (its menu's key equivalents)
        m_window->onUnhandledKey(e);
}

void View::rightMouseDown(Event &e)
{
    if (m_parent) {
        e.location = {e.location.x + m_frame.x, e.location.y + m_frame.y};
        m_parent->rightMouseDown(e);
    }
}

void View::render(cairo_t *cr)
{
    if (m_hidden)
        return;
    cairo_save(cr);
    cairo_translate(cr, m_frame.x, m_frame.y);
    if (clipsToBounds) {
        cairo_rectangle(cr, 0, 0, m_frame.w, m_frame.h);
        cairo_clip(cr);
    }
    draw(cr);
    for (View *v : m_subviews)
        v->render(cr);
    cairo_restore(cr);
}

// ---- Window

namespace {

struct RootView : View {
    void draw(cairo_t *cr) override
    {
        // the window's 237 grey; a status window has none (its items over the menu bar)
        if (!window() || window()->kind() != Window::Status)
            fill(cr, bounds(), Color::gray(237));
    }
};

// The title bar's buttons (WindowServer's): 14 x 16, 21 apart from 8 in, 3 down; the toolbar button
// 20 x 13, 7 from the right, 5 down; the proxy icon 16 x 16, 4 before the title
constexpr double ButtonLeft = 8, ButtonPitch = 21, ButtonTop = 3, ButtonWidth = 14, ButtonHeight = 16, TitleBar = 22;
constexpr double ToolbarButtonRight = 7, ToolbarButtonTop = 5, ToolbarButtonWidth = 20, ToolbarButtonHeight = 13;
constexpr double ProxyIconSize = 16, ProxyIconGap = 4, ProxyIconTop = 3;
constexpr int ToolbarButtonIndex = 3;

// The unified title bar and toolbar's gradient (measured on 10.6): a 218 first row, then 207 down to
// 167 at the toolbar's last row; inactive lighter
Color unifiedRow(int row, int total, bool active)
{
    if (row <= 0)
        return active ? Color::gray(218) : Color::gray(236);
    const double t = std::clamp(double(row - 1) / std::max(1, total - 2), 0.0, 1.0);
    const double top = active ? 207 : 232, bottom = active ? 167 : 207;
    return Color::gray(int(std::lround(top + (bottom - top) * t)));
}

} // namespace

Window::Window(Size size, const std::string &title, Kind kind, Window *parent, Point at)
    : m_kind(kind), m_size(size), m_title(title), m_content(new RootView)
{
    App &app = App::shared();
    m_content->m_window = this;
    m_content->setFrame({0, 0, size.w, size.h});
    Platform *p = app.platform();
    if (kind == Titled && p->drawsTitleBars())
        m_titleBar = TitleBar;
    if (parent)
        at = {at.x, at.y + parent->m_titleBar};
    m_native = p->createWindow(this, {size.w, size.h + m_titleBar}, kind, parent, at);
    m_native->setTitle(title);
    app.m_windows.push_back(this);
}

Window::~Window()
{
    App &app = App::shared();
    app.m_windows.erase(std::remove(app.m_windows.begin(), app.m_windows.end(), this), app.m_windows.end());
    m_content.reset();
    m_native.reset();
    app.platform()->flush();
}

void Window::setTitle(const std::string &title)
{
    m_title = title;
    m_native->setTitle(title);
    invalidate();
}

void Window::setContentSize(Size size)
{
    if (size.w == m_size.w && size.h == m_size.h)
        return;
    m_size = size;
    m_content->setFrame({0, 0, size.w, size.h});
    m_native->resize({size.w, size.h + m_titleBar});
    invalidate();
}

void Window::setResizable(Size minContent, Size maxContent)
{
    m_resizable = true;
    m_minSize = minContent;
    m_native->setResizable({minContent.w, minContent.h + m_titleBar},
                           {maxContent.w, maxContent.h > 0 ? maxContent.h + m_titleBar : 0});
}

void Window::nativeResize(Size surface)
{
    const Size size{std::max(1.0, surface.w), std::max(1.0, surface.h - m_titleBar)};
    if (size.w == m_size.w && size.h == m_size.h)
        return;
    m_size = size;
    m_content->setFrame({0, 0, size.w, size.h});
    if (onResize)
        onResize(size);
    invalidate();
}

void Window::setIcon(const Image &icon) { m_native->setIcon(icon); }

void Window::setFloatingPanel(bool utility) { m_native->setFloatingPanel(utility); }

void Window::setStyleMask(unsigned mask)
{
    m_titleButtons = (mask & (2 | 4 | 8)) != 0;
    m_native->setTitleButtons(m_titleButtons);
    invalidate();
}

void Window::setStatusBar(int top, int bottom)
{
    setBorderless();
    m_native->setStatusBar(top, bottom);
}

void Window::setBorderless()
{
    if (m_titleBar > 0) {
        m_titleBar = 0;
        m_native->resize(m_size);
    }
    m_native->setBorderless();
}

bool Window::moveTo(Point screenTopLeft) { return m_native->moveTo(screenTopLeft); }

void Window::setAlphaValue(double alpha)
{
    m_alpha = std::clamp(alpha, 0.0, 1.0);
    m_native->setOpacity(m_alpha);
    App::shared().platform()->flush();
}

void Window::setFloating(bool floating)
{
    m_floating = floating;
    m_native->setAbove(floating);
    App::shared().platform()->flush();
}

Point Window::screenOrigin() const { return m_native->toScreen({0, 0}); }

void Window::setKeyEquivalents(const std::vector<uint32_t> &keysyms) { m_native->setKeyEquivalents(keysyms); }

void Window::dragMove(Point fromScreen, Point toScreen, bool start)
{
    if (start) {
        m_dragOrigin = m_native->toScreen({0, 0});
        // Wayland: the compositor moves it (it can't be placed)
        if (!m_native->moveTo(m_dragOrigin))
            m_native->startMove();
        return;
    }
    m_native->moveTo({m_dragOrigin.x + toScreen.x - fromScreen.x, m_dragOrigin.y + toScreen.y - fromScreen.y});
}

void Window::setUnifiedToolbarHeight(int height)
{
    m_unified = height;
    m_native->setToolbar(height, m_toolbarButton);
    invalidate();
}

void Window::setRoundedBottomCorners(bool rounded)
{
    m_native->setRoundedBottomCorners(rounded);
    invalidate();
}

void windowPath(cairo_t *cr, Size size, bool roundedBottom)
{
    const double r = WindowCornerRadius, w = size.w, h = size.h;
    cairo_new_path(cr);
    cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
    cairo_arc(cr, w - r, r, r, 1.5 * M_PI, 2 * M_PI);
    if (roundedBottom) {
        cairo_arc(cr, w - r, h - r, r, 0, 0.5 * M_PI);
        cairo_arc(cr, r, h - r, r, 0.5 * M_PI, M_PI);
    } else {
        cairo_line_to(cr, w, h);
        cairo_line_to(cr, 0, h);
    }
    cairo_close_path(cr);
}

void Window::setShowsToolbarButton(bool shows)
{
    m_toolbarButton = shows;
    m_native->setToolbar(m_unified, shows);
    invalidate();
}

void Window::setRepresentedIcon(const Image &icon)
{
    m_proxyIcon = icon.isNull() ? Image() : icon.scaled(int(ProxyIconSize), int(ProxyIconSize));
    m_native->setProxyIcon(m_proxyIcon);
    invalidate();
}

void Window::setDocumentEdited(bool edited)
{
    if (edited == m_edited)
        return;
    m_edited = edited;
    m_native->setDocumentEdited(edited);
    invalidate();
}

void Window::show()
{
    m_native->show();
    m_visible = true;
    if (m_kind == Menu)
        m_key = true;
    invalidate();
    App::shared().platform()->flush();
}

void Window::orderFront()
{
    if (!m_visible) {
        show();
        return;
    }
    m_native->activate();
}

void Window::close()
{
    m_native->hide();
    App::shared().platform()->flush();
    m_visible = false;
    if (onClose)
        onClose();
}

void Window::grabInput() { m_native->grabInput(); }

Point Window::toScreen(Point p) const { return m_native->toScreen({p.x, p.y + m_titleBar}); }

void Window::makeFirstResponder(View *v)
{
    if (m_firstResponder == v)
        return;
    View *old = m_firstResponder;
    m_firstResponder = v;
    if (old)
        old->focusChanged();
    if (v)
        v->focusChanged();
}

void Window::invalidate() { m_dirty = true; }

void Window::drawTitleBar(cairo_t *cr)
{
    // as WindowServer draws it (windowserver.cpp): the gradient, the buttons from CoreUI's
    // titlebarcontrols, the title in Lucida Grande 13, engraved
    const double w = m_size.w;
    const bool active = m_key;
    if (m_unified > 0) {
        for (int y = 0; y < TitleBar; ++y)
            fill(cr, Rect{0, double(y), w, 1}, unifiedRow(y, int(TitleBar) + m_unified, active));
    } else {
        fill(cr, Rect{0, 0, w, 1}, Color::gray(255));
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 1, 0, TitleBar - 1);
        const double top = (active ? 239 : 246) / 255.0, bottom = (active ? 217 : 236) / 255.0;
        cairo_pattern_add_color_stop_rgb(g, 0, top, top, top);
        cairo_pattern_add_color_stop_rgb(g, 1, bottom, bottom, bottom);
        cairo_rectangle(cr, 0, 1, w, TitleBar - 2);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        fill(cr, Rect{0, TitleBar - 1, w, 1}, Color::gray(active ? 153 : 186));
    }
    static const char *const names[] = {"close", "minimize", "zoom"};
    for (int b = 0; b < (m_titleButtons ? 3 : 0); ++b) {
        std::vector<std::string> path{"titlebarcontrols", b == 0 && m_edited ? "closedirty" : names[b], "colorsryg", "regularwin"};
        if (!active)
            path.push_back("inactive");
        else if (m_titlePressed == b)
            path.push_back("pressed");
        else if (m_overTitleButtons)
            path.push_back("rollover");   // the three show their glyphs together
        CoreUI::Element e = CoreUI::element(path);
        if (!e.isNull())
            e.pieces[0].draw(cr, Point{ButtonLeft + b * ButtonPitch, ButtonTop});
    }
    if (m_toolbarButton) {
        std::vector<std::string> path{"titlebarcontrols", "toolbar", "colorsrgb", "regularwin"};
        if (!active)
            path.push_back("inactive");
        else if (m_titlePressed == ToolbarButtonIndex)
            path.push_back("pressed");
        CoreUI::Element e = CoreUI::element(path);
        if (!e.isNull())
            e.pieces[0].draw(cr, Point{w - ToolbarButtonRight - ToolbarButtonWidth, ToolbarButtonTop});
    }
    // the title and the proxy icon before it, centred together
    TextStyle t;
    t.engraved = true;
    t.engravedAlpha = 120 / 255.0;
    t.color = active ? Color::rgb(0x26, 0x26, 0x26) : Color::gray(0x7F);
    const double left = ButtonLeft + 3 * ButtonPitch + 6;
    const double iconRoom = m_proxyIcon.isNull() ? 0 : ProxyIconSize + ProxyIconGap;
    const double room = std::max(0.0, w - 2 * left - iconRoom);
    const double textW = std::min(room, std::ceil(textWidth(m_title, t.font)));
    const double x = std::floor((w - iconRoom - textW) / 2);
    if (!m_proxyIcon.isNull())
        m_proxyIcon.draw(cr, Rect{x, ProxyIconTop, ProxyIconSize, ProxyIconSize}, active ? 1 : 0.5);
    drawText(cr, m_title, Rect{x + iconRoom, 0, textW + 1, TitleBar - 1}, t);
}

void Window::drawSizeBox(cairo_t *cr)
{
    // NSThemeFrame's size box (10.6): three diagonal ridges in the bottom right corner, each a dark
    // line with a light one under it, 4 points apart
    const double r = m_size.w, b = m_size.h;
    cairo_save(cr);
    cairo_set_line_width(cr, 1);
    for (int i = 0; i < 3; ++i) {
        const double d = 4 + 4 * i;
        Color::gray(0, 0.45).set(cr);
        cairo_move_to(cr, r - d - 0.5, b - 1);
        cairo_line_to(cr, r - 1, b - d - 0.5);
        cairo_stroke(cr);
        Color::gray(255, 0.75).set(cr);
        cairo_move_to(cr, r - d + 0.5, b - 1);
        cairo_line_to(cr, r - 1, b - d + 0.5);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

int Window::titleButtonAt(Point p) const
{
    for (int b = 0; b < (m_titleButtons ? 3 : 0); ++b)
        if (Rect{ButtonLeft + b * ButtonPitch, ButtonTop, ButtonWidth, ButtonHeight}.contains(p))
            return b;
    if (m_toolbarButton && Rect{m_size.w - ToolbarButtonRight - ToolbarButtonWidth, ToolbarButtonTop, ToolbarButtonWidth, ToolbarButtonHeight}.contains(p))
        return ToolbarButtonIndex;
    return -1;
}

void Window::redraw()
{
    if (!m_dirty || !m_visible)
        return;
    cairo_t *cr = m_native->beginPaint();
    if (!cr)
        return;   // stays dirty: drawn when the platform can take a frame
    m_dirty = false;
    if (m_titleBar > 0) {
        drawTitleBar(cr);
        cairo_translate(cr, 0, m_titleBar);
    }
    m_content->render(cr);
    if (m_resizable && m_kind == Titled)
        drawSizeBox(cr);
    // AQUAKIT_CAPTURE=file.png: each frame of the windows written there, for checking without a screen
    if (const char *capture = std::getenv("AQUAKIT_CAPTURE")) {
        cairo_surface_flush(cairo_get_group_target(cr));
        const std::string file = m_kind == Titled ? std::string(capture) : std::string(capture) + (m_kind == Menu ? ".menu.png" : ".full.png");
        cairo_surface_write_to_png(cairo_get_group_target(cr), file.c_str());
    }
    m_native->endPaint(cr);
}

void Window::dispatchMouse(Event &e, Point p, bool press, bool release, bool motion)
{
    auto local = [&](View *v) {
        e.location = v->fromWindow(p);
        return v;
    };
    if (press) {
        View *hit = m_content->hitTest(p);
        if (!hit && onOutsideClick) {
            onOutsideClick(e);
            return;
        }
        if (!hit || !hit->isEnabled())
            return;
        if (hit->acceptsFirstResponder())
            makeFirstResponder(hit);
        m_tracking = hit;
        local(hit)->mouseDown(e);
    } else if (release) {
        if (View *t = m_tracking) {
            m_tracking = nullptr;
            local(t)->mouseUp(e);
        } else if (onOutsideClick && !m_content->hitTest(p)) {
            onOutsideClick(e);
        }
    } else if (motion) {
        if (onMouseMovedOutside && !m_tracking && (p.x < 0 || p.y < 0 || p.x >= m_size.w || p.y >= m_size.h)) {
            onMouseMovedOutside(e);
            return;
        }
        if (m_tracking) {
            e.type = Event::MouseDragged;
            local(m_tracking)->mouseDragged(e);
            return;
        }
        View *hit = m_content->hitTest(p);
        if (hit != m_hover) {
            if (m_hover) {
                Event x = e;
                x.type = Event::MouseExited;
                x.location = m_hover->fromWindow(p);
                m_hover->mouseExited(x);
            }
            m_hover = hit;
            // the cursor rectangles: the view's cursor, else its superviews', else the arrow
            const char *name = nullptr;
            for (View *v = hit; v && !name; v = v->superview())
                name = v->cursor();
            const std::string cursor = name ? name : "default";
            if (cursor != m_cursor) {
                m_cursor = cursor;
                m_native->setCursor(cursor);
            }
        }
        if (hit)
            local(hit)->mouseMoved(e);
    }
}

void Window::nativeButton(bool press, Point p, Point screen, unsigned modifiers, uint32_t timeMs, bool secondary)
{
    // the right button, or Control and the left one: the view's contextual menu (a menu's own window
    // takes it as a click)
    if ((secondary || (press && modifiers == Control) || m_secondaryDown) && m_kind == Titled && (p.y >= m_titleBar || m_secondaryDown)) {
        if (press && !m_secondaryDown) {
            m_secondaryDown = true;
            const Point inContent{p.x, p.y - m_titleBar};
            if (View *hit = m_content->hitTest(inContent); hit && hit->isEnabled()) {
                Event e;
                e.type = Event::MouseDown;
                e.screen = screen;
                e.modifiers = modifiers;
                e.location = hit->fromWindow(inContent);
                if (hit->acceptsFirstResponder())
                    makeFirstResponder(hit);
                hit->rightMouseDown(e);
            }
        } else if (!press) {
            m_secondaryDown = false;
        }
        return;
    }
    // the title bar AquaKit draws: its buttons, and moving the window
    if (m_titleBar > 0 && (p.y < m_titleBar || m_titlePressed >= 0) && !m_tracking) {
        const int b = titleButtonAt(p);
        if (press) {
            if (b >= 0)
                m_titlePressed = b;
            else
                m_native->startMove();
        } else if (m_titlePressed >= 0) {
            const int pressed = m_titlePressed;
            m_titlePressed = -1;
            if (b == pressed) {
                if (b == 0)
                    nativeClose();
                else if (b == 1)
                    m_native->minimize();
                else if (b == ToolbarButtonIndex)
                    nativeToggleToolbar();
            }
        }
        invalidate();
        return;
    }
    p.y -= m_titleBar;
    if (press && m_resizable && m_kind == Titled && !m_tracking && p.x >= m_size.w - 15 && p.y >= m_size.h - 15) {
        // the size box (on X11 WindowServer's grip window over it has the click first)
        m_native->startResize();
        return;
    }
    Event e;
    e.screen = screen;
    e.modifiers = modifiers;
    if (press) {
        const bool again = timeMs - m_lastClickTime < 400 && std::abs(p.x - m_lastClick.x) < 4 && std::abs(p.y - m_lastClick.y) < 4;
        m_clickCount = again ? m_clickCount + 1 : 1;
        m_lastClick = p;
        m_lastClickTime = timeMs;
    }
    e.clickCount = m_clickCount;
    e.type = press ? Event::MouseDown : Event::MouseUp;
    dispatchMouse(e, p, press, !press, false);
}

void Window::nativeMotion(Point p, Point screen)
{
    if (m_titleBar > 0) {
        const bool over = p.y < m_titleBar && p.x < ButtonLeft + 3 * ButtonPitch;
        if (over != m_overTitleButtons) {
            m_overTitleButtons = over;
            invalidate();
        }
        if (m_titlePressed >= 0)
            return;
    }
    Event e;
    e.type = Event::MouseMoved;
    e.screen = screen;
    dispatchMouse(e, {p.x, p.y - m_titleBar}, false, false, true);
}

void Window::nativeLeave()
{
    if (m_overTitleButtons) {
        m_overTitleButtons = false;
        invalidate();
    }
    if (m_hover && !m_tracking) {
        Event e;
        e.type = Event::MouseExited;
        m_hover->mouseExited(e);
        m_hover = nullptr;
    }
}

void Window::nativeScroll(Point p, double lines)
{
    p.y -= m_titleBar;
    Event e;
    e.type = Event::ScrollWheel;
    e.deltaY = lines;
    if (View *hit = m_content->hitTest(p)) {
        e.location = hit->fromWindow(p);
        hit->scrollWheel(e);
    }
}

void Window::nativeKey(uint32_t keysym, const std::string &text, unsigned modifiers)
{
    Event e;
    e.type = Event::KeyDown;
    e.keysym = keysym;
    e.modifiers = modifiers;
    if (!(modifiers & (Control | Command)))
        e.text = text;
    if (m_firstResponder)
        m_firstResponder->keyDown(e);
    else
        m_content->keyDown(e);
}

void Window::nativeFocus(bool key)
{
    if (key == m_key || m_kind != Titled)
        return;
    m_key = key;
    invalidate();
    if (onKeyChanged)
        onKeyChanged(key);
}

void Window::nativeClose() { close(); }

void Window::nativeDismiss()
{
    if (onOutsideClick) {
        Event e;
        e.type = Event::MouseDown;
        onOutsideClick(e);
    } else {
        close();
    }
}

// ---- App

#ifndef AQUAKIT_WAYLAND
std::unique_ptr<Platform> Platform::wayland() { return nullptr; }
#endif


size_t App::visibleWindowCount() const
{
    size_t n = 0;
    for (const Window *w : m_windows)
        n += w->isVisible() && w->kind() == Window::Titled;
    return n;
}

App &App::shared()
{
    static App app;
    return app;
}

bool App::open()
{
    if (m_platform)
        return true;
    const char *choice = std::getenv("AQUAKIT_PLATFORM");
    const std::string want = choice ? choice : "";
    if (want != "x11")
        m_platform = Platform::wayland();
    if (!m_platform && want != "wayland")
        m_platform = Platform::x11();
    if (!m_platform)
        return false;
    FontManager::registerSystemFonts();
    return true;
}

std::string App::platformName() const { return m_platform ? m_platform->name() : ""; }

Rect App::screenFrame() const { return m_platform->screenFrame(); }

Rect App::visibleFrame() const { return m_platform->visibleFrame(); }

void App::setFrontWithoutWindows(bool front)
{
    if (m_platform)
        m_platform->setFrontWithoutWindows(front);
}

int App::run()
{
    // the display, a source of the main run loop: the windows redrawn after the timers, its events
    // flushed before the wait and read after it
    RunLoop::Source display;
    display.fd = m_platform->fd();
    display.afterTimers = [this] {
        for (Window *w : std::vector<Window *>(m_windows))
            w->redraw();
    };
    display.prepare = [this] {
        m_platform->beforePoll();
        return m_platform->hasPending();
    };
    display.dispatch = [this](bool readable) {
        if (m_platform->afterPoll(readable))
            return true;
        std::fprintf(stderr, "AquaKit: the display connection is gone\n");
        return false;
    };
    RunLoop::main().setSource(std::move(display));
    return RunLoop::main().run();
}

void Window::miniaturize()
{
    if (m_native)
        m_native->minimize();
}

void App::quit(int code) { RunLoop::main().stop(code); }

} // namespace AK
