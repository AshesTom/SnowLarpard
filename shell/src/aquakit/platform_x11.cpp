// SPDX-License-Identifier: LGPL-2.1-or-later
// AquaKit on X11 (xcb): WindowServer (or another window manager) draws the title bars; menus and the
// screen saver are override-redirect windows that grab the pointer and the keyboard.
#include "foundation.h"
#include "platform.h"

#include <cairo-xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <unistd.h>
#include <vector>

namespace AK {

namespace {

class X11Platform;
X11Platform *X = nullptr;

unsigned modifiers(uint16_t state)
{
    unsigned m = 0;
    if (state & XCB_MOD_MASK_SHIFT) m |= Shift;
    if (state & XCB_MOD_MASK_CONTROL) m |= Control;
    if (state & XCB_MOD_MASK_1) m |= Option;
    if (state & XCB_MOD_MASK_4) m |= Command;
    return m;
}

// An icon as _NET_WM_ICON has it: width, height, ARGB rows (cairo's premultiplied, made straight)
void appendIcon(std::vector<uint32_t> &data, const Image &img)
{
    cairo_surface_flush(img.surface());
    const unsigned char *px = cairo_image_surface_get_data(img.surface());
    const int stride = cairo_image_surface_get_stride(img.surface());
    data.push_back(uint32_t(img.width()));
    data.push_back(uint32_t(img.height()));
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t v = reinterpret_cast<const uint32_t *>(px + y * stride)[x];
            const uint32_t a = v >> 24;
            auto un = [a](uint32_t c) { return a ? std::min(255u, c * 255 / a) : 0u; };
            data.push_back(a << 24 | un(v >> 16 & 0xFF) << 16 | un(v >> 8 & 0xFF) << 8 | un(v & 0xFF));
        }
}

class X11Window;

class X11Platform : public Platform {
public:
    xcb_connection_t *conn = nullptr;
    xcb_screen_t *screen = nullptr;
    xcb_visualtype_t *visual = nullptr;
    xcb_key_symbols_t *keySymbols = nullptr;
    std::vector<X11Window *> windows;

    ~X11Platform() override
    {
        if (keySymbols)
            xcb_key_symbols_free(keySymbols);
        if (conn)
            xcb_disconnect(conn);
        X = nullptr;
    }
    const char *name() const override { return "x11"; }
    bool drawsTitleBars() const override { return false; }
    std::unique_ptr<NativeWindow> createWindow(Window *w, Size surface, Window::Kind kind, Window *parent, Point at) override;
    int fd() const override { return xcb_get_file_descriptor(conn); }
    void flush() override { xcb_flush(conn); }
    Rect screenFrame() const override { return {0, 0, double(screen->width_in_pixels), double(screen->height_in_pixels)}; }
    void setFrontWithoutWindows(bool front) override
    {
        const xcb_atom_t a = atom("_SNOWLEOPARD_FRONT_APP");
        if (front) {
            // the serial tells the menu bar the application came forward again
            static uint32_t serial = 0;
            const uint32_t v[2] = {uint32_t(getpid()), ++serial + uint32_t(now() * 1000)};
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, a, XCB_ATOM_CARDINAL, 32, 2, v);
        } else {
            auto *r = xcb_get_property_reply(conn, xcb_get_property(conn, 0, screen->root, a, XCB_ATOM_CARDINAL, 0, 2), nullptr);
            const bool ours = r && xcb_get_property_value_length(r) >= 4 &&
                              static_cast<const uint32_t *>(xcb_get_property_value(r))[0] == uint32_t(getpid());
            free(r);
            if (ours)
                xcb_delete_property(conn, screen->root, a);
            else   // its watchers look again (the Dock: this application's light goes)
                xcb_change_property(conn, XCB_PROP_MODE_APPEND, screen->root, a, XCB_ATOM_CARDINAL, 32, 0, nullptr);
        }
        xcb_flush(conn);
    }
    Rect visibleFrame() const override
    {
        // _NET_WORKAREA, as WindowServer sets it from the menu bar's and the Dock's struts
        Rect r = screenFrame();
        auto *reply = xcb_get_property_reply(
            conn, xcb_get_property(conn, 0, screen->root, const_cast<X11Platform *>(this)->atom("_NET_WORKAREA"), XCB_ATOM_CARDINAL, 0, 4), nullptr);
        if (reply && reply->format == 32 && xcb_get_property_value_length(reply) >= 16) {
            const auto *v = static_cast<const uint32_t *>(xcb_get_property_value(reply));
            if (v[2] > 0 && v[3] > 0)
                r = {double(v[0]), double(v[1]), double(v[2]), double(v[3])};
        }
        free(reply);
        return r;
    }
    bool afterPoll(bool) override;
    // what the poll would miss: the drawing still in xcb's buffer, the events xcb read already (while
    // waiting for a reply) and keeps in its queue
    void beforePoll() override
    {
        xcb_flush(conn);
        while (xcb_generic_event_t *e = xcb_poll_for_queued_event(conn))
            queued.push_back(e);
    }
    bool hasPending() const override { return !queued.empty(); }
    std::vector<xcb_generic_event_t *> queued;

    xcb_atom_t atom(const char *name)
    {
        static std::map<std::string, xcb_atom_t> cache;
        auto it = cache.find(name);
        if (it != cache.end())
            return it->second;
        auto *r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, uint16_t(std::strlen(name)), name), nullptr);
        const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
        free(r);
        return cache[name] = a;
    }
    void property(xcb_window_t w, const char *name, xcb_atom_t type, int format, size_t count, const void *data)
    {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, atom(name), type, uint8_t(format), uint32_t(count), data);
    }
    void dispatch(xcb_generic_event_t *e);
};

class X11Window : public NativeWindow {
public:
    X11Window(Window *w, Size size, Window::Kind kind, Point screenAt) : m_window(w), m_kind(kind), m_size(size), m_at(screenAt)
    {
        X11Platform &p = *X;
        m_id = xcb_generate_id(p.conn);
        const uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BIT_GRAVITY | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_SAVE_UNDER | XCB_CW_EVENT_MASK;
        const uint32_t events = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_KEY_PRESS |
                                XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        const uint32_t values[] = {XCB_BACK_PIXMAP_NONE, XCB_GRAVITY_NORTH_WEST, uint32_t(kind != Window::Titled),
                                   uint32_t(kind == Window::Menu), events};
        xcb_create_window(p.conn, XCB_COPY_FROM_PARENT, m_id, p.screen->root, 0, 0, uint16_t(size.w), uint16_t(size.h), 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, p.screen->root_visual, mask, values);
        const xcb_atom_t protocols[] = {p.atom("WM_DELETE_WINDOW")};
        p.property(m_id, "WM_PROTOCOLS", XCB_ATOM_ATOM, 32, 1, protocols);
        // WM_CLASS: the program's name twice (the Dock knows the shell's own applications by it)
        const std::string name = programName();
        const std::string cls = name + '\0' + name + '\0';
        xcb_change_property(p.conn, XCB_PROP_MODE_REPLACE, m_id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, uint32_t(cls.size()), cls.data());
        const uint32_t pid = uint32_t(getpid());
        p.property(m_id, "_NET_WM_PID", XCB_ATOM_CARDINAL, 32, 1, &pid);
        const xcb_atom_t type = p.atom(kind == Window::Menu ? "_NET_WM_WINDOW_TYPE_POPUP_MENU" : "_NET_WM_WINDOW_TYPE_NORMAL");
        p.property(m_id, "_NET_WM_WINDOW_TYPE", XCB_ATOM_ATOM, 32, 1, &type);
        if (kind == Window::Titled) {
            // WM_NORMAL_HINTS: a fixed size (System Preferences' window doesn't resize)
            uint32_t hints[18] = {};
            hints[0] = 16 | 32;   // PMinSize | PMaxSize
            hints[5] = hints[7] = uint32_t(size.w);
            hints[6] = hints[8] = uint32_t(size.h);
            p.property(m_id, "WM_NORMAL_HINTS", p.atom("WM_SIZE_HINTS"), 32, 18, hints);
        }
        if (kind == Window::FullScreen) {
            // no pointer over the screen saver
            const xcb_pixmap_t empty = xcb_generate_id(p.conn);
            xcb_create_pixmap(p.conn, 1, empty, m_id, 1, 1);
            const xcb_cursor_t blank = xcb_generate_id(p.conn);
            xcb_create_cursor(p.conn, blank, empty, empty, 0, 0, 0, 0, 0, 0, 0, 0);
            xcb_change_window_attributes(p.conn, m_id, XCB_CW_CURSOR, &blank);
            xcb_free_pixmap(p.conn, empty);
        }
        m_surface = cairo_xcb_surface_create(p.conn, m_id, p.visual, int(size.w), int(size.h));
        p.windows.push_back(this);
    }
    ~X11Window() override
    {
        if (X) {
            X->windows.erase(std::remove(X->windows.begin(), X->windows.end(), this), X->windows.end());
            ungrab();
            cairo_surface_destroy(m_surface);
            if (m_back)
                cairo_surface_destroy(m_back);
            xcb_destroy_window(X->conn, m_id);
        }
    }
    void setTitle(const std::string &title) override
    {
        X->property(m_id, "_NET_WM_NAME", X->atom("UTF8_STRING"), 8, title.size(), title.data());
        xcb_change_property(X->conn, XCB_PROP_MODE_REPLACE, m_id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, uint32_t(title.size()), title.data());
    }
    void setIcon(const Image &icon) override
    {
        // _NET_WM_ICON: width, height, ARGB rows, for each size
        if (icon.isNull())
            return;
        std::vector<uint32_t> data;
        for (int s : {16, 32, 48, 128})
            appendIcon(data, icon.scaled(s, s));
        X->property(m_id, "_NET_WM_ICON", XCB_ATOM_CARDINAL, 32, data.size(), data.data());
    }
    void show() override
    {
        X11Platform &p = *X;
        m_mapped = true;
        if (m_kind == Window::Titled && m_hasPosition) {
            // where the program put it (a Stickies note's saved frame): USPosition, kept by WindowServer
            uint32_t hints[18] = {};
            hints[0] = 1 | (m_resizable ? 16 : 0);
            hints[1] = uint32_t(int32_t(std::lround(m_position.x)));
            hints[2] = uint32_t(int32_t(std::lround(m_position.y)));
            hints[5] = uint32_t(m_minSize.w);
            hints[6] = uint32_t(m_minSize.h);
            X->property(m_id, "WM_NORMAL_HINTS", X->atom("WM_SIZE_HINTS"), 32, 18, hints);
            const uint32_t pos[] = {hints[1], hints[2]};
            xcb_configure_window(p.conn, m_id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
        } else if (m_kind == Window::Titled) {
            // in the middle of the screen, a third down (as -[NSWindow center])
            const Rect s = p.screenFrame();
            const uint32_t pos[] = {uint32_t(std::max(0.0, (s.w - m_size.w) / 2)), uint32_t(std::max(22.0, (s.h - m_size.h) / 3))};
            xcb_configure_window(p.conn, m_id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
        } else {
            // a menu where it was asked, kept on the screen and under the menu bar
            const Rect s = p.screenFrame();
            Point at = m_at;
            if (m_kind == Window::Menu) {
                at.x = std::clamp(at.x, 0.0, std::max(0.0, s.w - m_size.w));
                at.y = std::clamp(at.y, 22.0, std::max(22.0, s.h - m_size.h));
            }
            const uint32_t pos[] = {uint32_t(int32_t(std::lround(at.x))), uint32_t(int32_t(std::lround(at.y))), XCB_STACK_MODE_ABOVE};
            xcb_configure_window(p.conn, m_id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_STACK_MODE, pos);
        }
        xcb_map_window(p.conn, m_id);
    }
    void hide() override
    {
        m_mapped = false;
        ungrab();
        xcb_unmap_window(X->conn, m_id);
    }
    // The window is drawn in an image, then put on the display at once: its pixels can be read while
    // it is drawn (the text's LCD smoothing mixes with what is under it)
    cairo_t *beginPaint() override
    {
        const int w = std::max(1, int(m_size.w)), h = std::max(1, int(m_size.h));
        if (!m_back || cairo_image_surface_get_width(m_back) != w || cairo_image_surface_get_height(m_back) != h) {
            if (m_back)
                cairo_surface_destroy(m_back);
            m_back = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        }
        return cairo_create(m_back);
    }
    void endPaint(cairo_t *cr) override
    {
        cairo_destroy(cr);
        cairo_surface_flush(m_back);
        cairo_t *out = cairo_create(m_surface);
        cairo_set_source_surface(out, m_back, 0, 0);
        cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
        cairo_paint(out);
        cairo_destroy(out);
        cairo_surface_flush(m_surface);
    }
    Point toScreen(Point p) const override
    {
        auto *r = xcb_translate_coordinates_reply(X->conn, xcb_translate_coordinates(X->conn, m_id, X->screen->root, 0, 0), nullptr);
        if (r) {
            p.x += r->dst_x;
            p.y += r->dst_y;
            free(r);
        }
        return p;
    }
    void grabInput() override
    {
        xcb_connection_t *c = X->conn;
        for (int attempt = 0; attempt < 20; ++attempt) {
            auto *p = xcb_grab_pointer_reply(
                c,
                xcb_grab_pointer(c, 0, m_id, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
                nullptr);
            const bool ok = p && p->status == XCB_GRAB_STATUS_SUCCESS;
            free(p);
            if (ok)
                break;
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        free(xcb_grab_keyboard_reply(c, xcb_grab_keyboard(c, 0, m_id, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), nullptr));
        m_grabbed = true;
    }
    void resize(Size size) override
    {
        m_size = size;
        if (m_kind == Window::Titled && !m_resizable) {
            uint32_t hints[18] = {};
            hints[0] = 16 | 32;
            hints[5] = hints[7] = uint32_t(size.w);
            hints[6] = hints[8] = uint32_t(size.h);
            X->property(m_id, "WM_NORMAL_HINTS", X->atom("WM_SIZE_HINTS"), 32, 18, hints);
        }
        const uint32_t wh[] = {uint32_t(size.w), uint32_t(size.h)};
        xcb_configure_window(X->conn, m_id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, wh);
        cairo_xcb_surface_set_size(m_surface, int(size.w), int(size.h));
    }
    void setResizable(Size minSize) override
    {
        // WM_NORMAL_HINTS: the smallest size only (WindowServer's size box resizes it)
        m_resizable = true;
        m_minSize = minSize;
        uint32_t hints[18] = {};
        hints[0] = 16;   // PMinSize
        hints[5] = uint32_t(minSize.w);
        hints[6] = uint32_t(minSize.h);
        X->property(m_id, "WM_NORMAL_HINTS", X->atom("WM_SIZE_HINTS"), 32, 18, hints);
    }
    void configured(Size size)
    {
        if (size.w == m_size.w && size.h == m_size.h)
            return;
        m_size = size;
        cairo_xcb_surface_set_size(m_surface, int(size.w), int(size.h));
        m_window->nativeResize(size);
    }
    void setFloatingPanel() override
    {
        // a utility window above the others, on every desktop, not in the Dock's list
        const xcb_atom_t type = X->atom("_NET_WM_WINDOW_TYPE_UTILITY");
        X->property(m_id, "_NET_WM_WINDOW_TYPE", XCB_ATOM_ATOM, 32, 1, &type);
        const xcb_atom_t state[] = {X->atom("_NET_WM_STATE_ABOVE"), X->atom("_NET_WM_STATE_SKIP_TASKBAR"), X->atom("_NET_WM_STATE_STICKY")};
        X->property(m_id, "_NET_WM_STATE", XCB_ATOM_ATOM, 32, 3, state);
        // where show() puts it (-[NSWindow center]) kept: PPosition, besides its fixed size
        uint32_t hints[18] = {};
        hints[0] = 4 | 16 | 32;   // PPosition | PMinSize | PMaxSize
        hints[5] = hints[7] = uint32_t(m_size.w);
        hints[6] = hints[8] = uint32_t(m_size.h);
        X->property(m_id, "WM_NORMAL_HINTS", X->atom("WM_SIZE_HINTS"), 32, 18, hints);
    }
    void setBorderless() override
    {
        // _MOTIF_WM_HINTS: flags MWM_HINTS_DECORATIONS, decorations none (WindowServer draws no frame)
        const uint32_t motif[5] = {2, 0, 0, 0, 0};
        X->property(m_id, "_MOTIF_WM_HINTS", X->atom("_MOTIF_WM_HINTS"), 32, 5, motif);
    }
    void setOpacity(double alpha) override
    {
        // _NET_WM_WINDOW_OPACITY, 0 to 0xffffffff, which WindowServer's compositor reads; none when opaque
        if (alpha >= 1) {
            xcb_delete_property(X->conn, m_id, X->atom("_NET_WM_WINDOW_OPACITY"));
            return;
        }
        const uint32_t v = uint32_t(std::lround(std::clamp(alpha, 0.0, 1.0) * 0xffffffffu));
        X->property(m_id, "_NET_WM_WINDOW_OPACITY", XCB_ATOM_CARDINAL, 32, 1, &v);
    }
    void setKeyEquivalents(const std::vector<uint32_t> &keysyms) override
    {
        X->property(m_id, "_SNOWLEOPARD_KEY_EQUIVALENTS", XCB_ATOM_CARDINAL, 32, keysyms.size(), keysyms.data());
    }
    void setAbove(bool above) override
    {
        // before it is shown, its _NET_WM_STATE; after, the window manager asked (EWMH: action, the state)
        const xcb_atom_t state = X->atom("_NET_WM_STATE_ABOVE");
        if (!m_mapped) {
            if (above)
                X->property(m_id, "_NET_WM_STATE", XCB_ATOM_ATOM, 32, 1, &state);
            else
                xcb_delete_property(X->conn, m_id, X->atom("_NET_WM_STATE"));
            return;
        }
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = m_id;
        ev.type = X->atom("_NET_WM_STATE");
        ev.data.data32[0] = above ? 1 : 0;
        ev.data.data32[1] = state;
        ev.data.data32[3] = 1;   // a normal application
        xcb_send_event(X->conn, 0, X->screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&ev));
    }
    bool moveTo(Point p) override
    {
        m_hasPosition = true;
        m_position = p;
        const uint32_t v[] = {uint32_t(int32_t(std::lround(p.x))), uint32_t(int32_t(std::lround(p.y)))};
        xcb_configure_window(X->conn, m_id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
        xcb_flush(X->conn);
        return true;
    }
    void setToolbar(int height, bool button) override
    {
        // WindowServer draws the title bar in the toolbar's gradient, and the toolbar button
        const uint32_t v[] = {uint32_t(height), button ? 1u : 0u};
        X->property(m_id, "_SNOWLEOPARD_UNIFIED_TOOLBAR", XCB_ATOM_CARDINAL, 32, 2, v);
    }
    void setRoundedBottomCorners(bool rounded) override
    {
        // WindowServer shapes the frame (its top corners always rounded)
        const uint32_t v = rounded ? 1 : 0;
        X->property(m_id, "_SNOWLEOPARD_ROUNDED_BOTTOM", XCB_ATOM_CARDINAL, 32, 1, &v);
    }
    void setProxyIcon(const Image &icon) override
    {
        // WindowServer draws it before the title: width, height, ARGB rows (straight), as _NET_WM_ICON
        std::vector<uint32_t> data;
        if (!icon.isNull())
            appendIcon(data, icon);
        X->property(m_id, "_SNOWLEOPARD_PROXY_ICON", XCB_ATOM_CARDINAL, 32, data.size(), data.data());
    }

    void handle(const xcb_generic_event_t *ev);
    xcb_window_t id() const { return m_id; }

private:
    void ungrab()
    {
        if (!m_grabbed)
            return;
        xcb_ungrab_pointer(X->conn, XCB_CURRENT_TIME);
        xcb_ungrab_keyboard(X->conn, XCB_CURRENT_TIME);
        m_grabbed = false;
    }
    Window *m_window;
    Window::Kind m_kind;
    Size m_size;
    Point m_at;
    xcb_window_t m_id = 0;
    cairo_surface_t *m_surface = nullptr;
    cairo_surface_t *m_back = nullptr;   // what is drawn, before it goes to the display
    bool m_grabbed = false;
    bool m_resizable = false;
    bool m_mapped = false;
    bool m_hasPosition = false;   // moveTo before show: where show puts it
    Point m_position;
    Size m_minSize;
};

void X11Window::handle(const xcb_generic_event_t *ev)
{
    switch (ev->response_type & 0x7F) {
    case XCB_EXPOSE:
        m_window->nativeExpose();
        break;
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: {
        auto *f = reinterpret_cast<const xcb_focus_in_event_t *>(ev);
        if (f->detail == XCB_NOTIFY_DETAIL_POINTER || f->mode == XCB_NOTIFY_MODE_GRAB || f->mode == XCB_NOTIFY_MODE_UNGRAB)
            break;
        m_window->nativeFocus((ev->response_type & 0x7F) == XCB_FOCUS_IN);
        break;
    }
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
        auto *b = reinterpret_cast<const xcb_button_press_event_t *>(ev);
        const bool press = (ev->response_type & 0x7F) == XCB_BUTTON_PRESS;
        const Point p{double(b->event_x), double(b->event_y)};
        if (b->detail >= 4 && b->detail <= 5) {
            if (press)
                m_window->nativeScroll(p, b->detail == 4 ? -1 : 1);
        } else if (b->detail == 1 || b->detail == 3) {
            m_window->nativeButton(press, p, {double(b->root_x), double(b->root_y)}, modifiers(b->state), b->time, b->detail == 3);
        }
        break;
    }
    case XCB_MOTION_NOTIFY: {
        auto *m = reinterpret_cast<const xcb_motion_notify_event_t *>(ev);
        m_window->nativeMotion({double(m->event_x), double(m->event_y)}, {double(m->root_x), double(m->root_y)});
        break;
    }
    case XCB_LEAVE_NOTIFY:
        m_window->nativeLeave();
        break;
    case XCB_KEY_PRESS: {
        auto *k = reinterpret_cast<const xcb_key_press_event_t *>(ev);
        xcb_keysym_t sym = xcb_key_symbols_get_keysym(X->keySymbols, k->detail, (k->state & XCB_MOD_MASK_SHIFT) ? 1 : 0);
        if (!sym)
            sym = xcb_key_symbols_get_keysym(X->keySymbols, k->detail, 0);
        char buf[8] = {};
        const int n = xkb_keysym_to_utf8(sym, buf, sizeof buf);
        const bool printable = n > 1 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7F;
        m_window->nativeKey(sym, printable ? std::string(buf) : std::string(), modifiers(k->state));
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        auto *c = reinterpret_cast<const xcb_configure_notify_event_t *>(ev);
        if (m_kind == Window::Titled && m_resizable)   // not System Preferences' own resizing coming back late
            configured({double(c->width), double(c->height)});
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        auto *cm = reinterpret_cast<const xcb_client_message_event_t *>(ev);
        if (cm->type == X->atom("WM_PROTOCOLS") && cm->data.data32[0] == X->atom("WM_DELETE_WINDOW"))
            m_window->nativeClose();
        else if (cm->type == X->atom("_SNOWLEOPARD_TOGGLE_TOOLBAR"))
            m_window->nativeToggleToolbar();
        break;
    }
    default:
        break;
    }
}

std::unique_ptr<NativeWindow> X11Platform::createWindow(Window *w, Size surface, Window::Kind kind, Window *parent, Point at)
{
    return std::make_unique<X11Window>(w, surface, kind, parent ? parent->toScreen(at) : at);
}

void X11Platform::dispatch(xcb_generic_event_t *e)
{
    xcb_window_t target = 0;
    switch (e->response_type & 0x7F) {
    case XCB_EXPOSE: target = reinterpret_cast<xcb_expose_event_t *>(e)->window; break;
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: target = reinterpret_cast<xcb_focus_in_event_t *>(e)->event; break;
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: target = reinterpret_cast<xcb_button_press_event_t *>(e)->event; break;
    case XCB_MOTION_NOTIFY: target = reinterpret_cast<xcb_motion_notify_event_t *>(e)->event; break;
    case XCB_LEAVE_NOTIFY: target = reinterpret_cast<xcb_leave_notify_event_t *>(e)->event; break;
    case XCB_KEY_PRESS: target = reinterpret_cast<xcb_key_press_event_t *>(e)->event; break;
    case XCB_CLIENT_MESSAGE: target = reinterpret_cast<xcb_client_message_event_t *>(e)->window; break;
    case XCB_CONFIGURE_NOTIFY: target = reinterpret_cast<xcb_configure_notify_event_t *>(e)->window; break;
    case XCB_MAPPING_NOTIFY:
        xcb_refresh_keyboard_mapping(keySymbols, reinterpret_cast<xcb_mapping_notify_event_t *>(e));
        return;
    default: return;
    }
    // a grab sends everything to the grabbing window, whatever is under the pointer
    for (X11Window *w : std::vector<X11Window *>(windows))
        if (w->id() == target) {
            w->handle(e);
            return;
        }
}

bool X11Platform::afterPoll(bool)
{
    std::vector<xcb_generic_event_t *> early;
    early.swap(queued);
    for (xcb_generic_event_t *e : early) {
        dispatch(e);
        free(e);
    }
    while (xcb_generic_event_t *e = xcb_poll_for_event(conn)) {
        dispatch(e);
        free(e);
    }
    return !xcb_connection_has_error(conn);
}

} // namespace

std::unique_ptr<Platform> Platform::x11()
{
    auto p = std::make_unique<X11Platform>();
    int screenNumber = 0;
    p->conn = xcb_connect(nullptr, &screenNumber);
    if (xcb_connection_has_error(p->conn))
        return nullptr;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(p->conn));
    for (int i = 0; i < screenNumber && it.rem; ++i)
        xcb_screen_next(&it);
    p->screen = it.data;
    for (xcb_depth_iterator_t d = xcb_screen_allowed_depths_iterator(p->screen); d.rem && !p->visual; xcb_depth_next(&d))
        for (xcb_visualtype_iterator_t v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v))
            if (v.data->visual_id == p->screen->root_visual) {
                p->visual = v.data;
                break;
            }
    if (!p->visual)
        return nullptr;
    p->keySymbols = xcb_key_symbols_alloc(p->conn);
    X = p.get();
    return p;
}

} // namespace AK
