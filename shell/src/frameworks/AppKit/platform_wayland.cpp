// SPDX-License-Identifier: LGPL-2.1-or-later
// AquaKit on Wayland: xdg-shell windows drawn in shared memory, menus as xdg_popups with their grab,
// the keyboard through xkbcommon (with its repeat), the pointer's cursor from the cursor theme. No
// WindowServer draws the title bars here: AquaKit draws 10.6's itself (appkit.cpp), and moving the
// window by it asks the compositor (xdg_toplevel.move). The status bar (the menu bar) is a layer
// shell surface; on the Wayland WindowServer, the application in front without a window, its key
// equivalents and its keys then go through its private protocol (WindowServerClient).
#include "foundation.h"
#include "platform.h"

#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#ifdef SNOWLEOPARD_WAYLAND
#include "windowserverclient.h"
#endif

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace AK {

namespace {

class WlWindow;

struct Buffer {
    wl_buffer *buffer = nullptr;
    void *data = nullptr;
    size_t size = 0;
    cairo_surface_t *surface = nullptr;
    bool busy = false;
    bool dead = false;   // of an old size: gone when the compositor gives it back
    ~Buffer()
    {
        if (surface)
            cairo_surface_destroy(surface);
        if (buffer)
            wl_buffer_destroy(buffer);
        if (data)
            munmap(data, size);
    }
};

class WlPlatform : public Platform {
public:
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *wm = nullptr;
    zwlr_layer_shell_v1 *layerShell = nullptr;
    wl_seat *seat = nullptr;
    wl_pointer *pointer = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_output *output = nullptr;
    int outputWidth = 0, outputHeight = 0;
    wl_cursor_theme *cursorTheme = nullptr;
    wl_surface *cursorSurface = nullptr;
    xkb_context *xkb = nullptr;
    xkb_keymap *keymap = nullptr;
    xkb_state *state = nullptr;
    std::vector<WlWindow *> windows;
    WlWindow *pointerFocus = nullptr, *keyboardFocus = nullptr;
    Point pointerAt;
    uint32_t lastSerial = 0;
    uint32_t enterSerial = 0;          // the pointer's enter, for wl_pointer_set_cursor
    std::string cursorName = "default";
    int repeatRate = 25, repeatDelay = 400, repeatTimer = 0;
    bool reading = false;

    ~WlPlatform() override;
    const char *name() const override { return "wayland"; }
    bool drawsTitleBars() const override { return true; }
    std::unique_ptr<NativeWindow> createWindow(Window *w, Size surface, Window::Kind kind, Window *parent, Point at) override;
    int fd() const override { return wl_display_get_fd(display); }
    void flush() override { wl_display_flush(display); }
    Rect screenFrame() const override
    {
        return {0, 0, double(outputWidth > 0 ? outputWidth : 1280), double(outputHeight > 0 ? outputHeight : 800)};
    }
    Rect visibleFrame() const override
    {
#ifdef SNOWLEOPARD_WAYLAND
        // the window server's work area: the screen less the menu bar's and the Dock's zones
        const WindowServerClient::Rect a = WindowServerClient::workArea();
        if (WindowServerClient::connected() && a.width > 0 && a.height > 0)
            return {double(a.x), double(a.y), double(a.width), double(a.height)};
#endif
        return screenFrame();
    }
    void setFrontWithoutWindows(bool front) override
    {
#ifdef SNOWLEOPARD_WAYLAND
        if (windowServer())
            WindowServerClient::setFrontApp(front ? uint32_t(getpid()) : 0);
#else
        (void)front;
#endif
    }
    // The Wayland WindowServer's private protocol, on this display (none under another compositor)
    bool windowServer()
    {
#ifdef SNOWLEOPARD_WAYLAND
        if (!windowServerAsked) {
            windowServerAsked = true;
            if (WindowServerClient::attach(display))
                WindowServerClient::onKey([this](uint32_t keysym, const std::string &text, uint32_t mods) {
                    // its modifiers (1 shift, 4 control, 8 alt, 64 command) as AquaKit's
                    unsigned m = 0;
                    if (mods & 1) m |= Shift;
                    if (mods & 4) m |= Control;
                    if (mods & 8) m |= Option;
                    if (mods & 64) m |= Command;
                    if (keyWithoutWindows)
                        keyWithoutWindows(keysym, text, m);
                });
        }
        return WindowServerClient::connected();
#else
        return false;
#endif
    }
    bool windowServerAsked = false;
    void beforePoll() override
    {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);
        reading = true;
    }
    bool afterPoll(bool readable) override
    {
        if (reading) {
            if (readable)
                wl_display_read_events(display);
            else
                wl_display_cancel_read(display);
            reading = false;
        }
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        return wl_display_get_error(display) == 0;
    }
    WlWindow *find(wl_surface *s) const;
    unsigned modifiers() const
    {
        if (!state)
            return 0;
        unsigned m = 0;
        if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) m |= Shift;
        if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) m |= Control;
        if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) m |= Option;
        if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) m |= Command;
        return m;
    }
    void setCursor(uint32_t serial, bool hidden);
    void key(uint32_t key);
    void stopRepeat()
    {
        if (repeatTimer)
            App::shared().removeTimer(repeatTimer);
        repeatTimer = 0;
    }
};

WlPlatform *wl = nullptr;

class WlWindow : public NativeWindow {
public:
    WlWindow(Window *w, Size size, Window::Kind kind, WlWindow *parent, Point at)
        : window(w), kind(kind), m_size(size), m_parent(parent), m_at(at)
    {
        surface = wl_compositor_create_surface(wl->compositor);
        wl->windows.push_back(this);
    }
    ~WlWindow() override
    {
        hide();
        for (Buffer *b : m_buffers) {
            if (b->busy)
                b->dead = true;   // the compositor still has it: deleted on its release
            else
                delete b;
        }
        if (m_frame)
            wl_callback_destroy(m_frame);
        dropShadow();
        wl_surface_destroy(surface);
        if (wl) {
            wl->windows.erase(std::remove(wl->windows.begin(), wl->windows.end(), this), wl->windows.end());
            if (wl->pointerFocus == this)
                wl->pointerFocus = nullptr;
            if (wl->keyboardFocus == this) {
                wl->keyboardFocus = nullptr;
                wl->stopRepeat();
            }
        }
    }
    void setTitle(const std::string &title) override
    {
        m_title = title;
        if (m_toplevel)
            xdg_toplevel_set_title(m_toplevel, title.c_str());
    }
    void show() override;
    void hide() override
    {
        if (m_layer)
            zwlr_layer_surface_v1_destroy(m_layer);
        m_layer = nullptr;
        if (m_popup)
            xdg_popup_destroy(m_popup);
        if (m_toplevel)
            xdg_toplevel_destroy(m_toplevel);
        if (m_xdg)
            xdg_surface_destroy(m_xdg);
        m_popup = nullptr;
        m_toplevel = nullptr;
        m_xdg = nullptr;
        m_configured = false;
        wl_surface_attach(surface, nullptr, 0, 0);
        wl_surface_commit(surface);
    }
    cairo_t *beginPaint() override
    {
        if (!m_configured || m_frame)
            return nullptr;
        Buffer *b = nullptr;
        for (Buffer *x : m_buffers)
            if (!x->busy) {
                b = x;
                break;
            }
        if (!b) {
            if (m_buffers.size() >= 4)
                return nullptr;
            b = new Buffer;
            if (!allocate(*b)) {
                delete b;
                return nullptr;
            }
            m_buffers.push_back(b);
        }
        m_painting = b;
        cairo_t *cr = cairo_create(b->surface);
        if (!hasShadow())
            return cr;
        // the shadow around the window, then the window inside its rounded frame
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, shadow(), 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_translate(cr, ShadowLeft, ShadowTop);
        windowPath(cr, m_size, m_roundedBottom);
        cairo_clip(cr);
        return cr;
    }
    void endPaint(cairo_t *cr) override
    {
        cairo_destroy(cr);
        Buffer *b = m_painting;
        m_painting = nullptr;
        cairo_surface_flush(b->surface);
        wl_surface_attach(surface, b->buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, bufferWidth(), bufferHeight());
        if (hasShadow() && m_xdg) {
            // the window is its frame, without the shadow: what the compositor places, resizes and
            // clicks reach (a click on the shadow goes to the window under it)
            xdg_surface_set_window_geometry(m_xdg, ShadowLeft, ShadowTop, int(m_size.w), int(m_size.h));
            wl_region *input = wl_compositor_create_region(wl->compositor);
            wl_region_add(input, ShadowLeft, ShadowTop, int(m_size.w), int(m_size.h));
            wl_surface_set_input_region(surface, input);
            wl_region_destroy(input);
        }
        // the next frame when the compositor has shown this one
        m_frame = wl_surface_frame(surface);
        static const wl_callback_listener done = {[](void *data, wl_callback *cb, uint32_t) {
            auto *w = static_cast<WlWindow *>(data);
            wl_callback_destroy(cb);
            w->m_frame = nullptr;
        }};
        wl_callback_add_listener(m_frame, &done, this);
        wl_surface_commit(surface);
        b->busy = true;
    }
    void startMove() override
    {
        if (m_toplevel)
            xdg_toplevel_move(m_toplevel, wl->seat, wl->lastSerial);
    }
    void minimize() override
    {
        if (m_toplevel)
            xdg_toplevel_set_minimized(m_toplevel);
    }
    void resize(Size size) override
    {
        m_size = size;
        dropShadow();
        // the buffers of the old size go (those the compositor holds, once it gives them back)
        for (Buffer *b : m_buffers) {
            if (b->busy)
                b->dead = true;
            else
                delete b;
        }
        m_buffers.clear();
        if (m_toplevel && kind == Window::Titled && !m_resizable) {
            xdg_toplevel_set_min_size(m_toplevel, int(size.w), int(size.h));
            xdg_toplevel_set_max_size(m_toplevel, int(size.w), int(size.h));
        }
    }
    void setResizable(Size minSize, Size maxSize) override
    {
        m_resizable = true;
        m_minSize = minSize;
        m_maxSize = maxSize;
        if (m_toplevel) {
            xdg_toplevel_set_min_size(m_toplevel, int(minSize.w), int(minSize.h));
            xdg_toplevel_set_max_size(m_toplevel, int(maxSize.w), int(maxSize.h));
        }
    }
    void startResize() override
    {
        if (m_toplevel)
            xdg_toplevel_resize(m_toplevel, wl->seat, wl->lastSerial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    }

    void setRoundedBottomCorners(bool rounded) override { m_roundedBottom = rounded; }
    // NSStatusBar's window: a layer over the windows along the screen's top (or bottom), keeping its
    // zone out of their way
    void setStatusBar(int top, int bottom) override
    {
        m_statusTop = top;
        m_statusBottom = bottom;
        if (!wl->layerShell)
            return;
        if (m_layer) {
            zwlr_layer_surface_v1_set_exclusive_zone(m_layer, top > 0 ? top : bottom);
            wl_surface_commit(surface);
        } else if (m_xdg) {
            hide();
            show();
        }
    }
    void setKeyEquivalents(const std::vector<uint32_t> &keysyms) override
    {
#ifdef SNOWLEOPARD_WAYLAND
        if (wl->windowServer())
            WindowServerClient::setKeyEquivalents(keysyms);
#else
        (void)keysyms;
#endif
    }
    zwlr_layer_surface_v1 *layer() const { return m_layer; }
    void setCursor(const std::string &name) override;
    void setActive(bool active)
    {
        if (active != m_active)
            dropShadow();
        m_active = active;
    }
    // a pointer's place on the surface, in the window's (the surface less its shadow)
    Point fromSurface(Point p) const { return hasShadow() ? Point{p.x - ShadowLeft, p.y - ShadowTop} : p; }

    Window *window;
    Window::Kind kind;
    wl_surface *surface = nullptr;
    xdg_surface *xdg() const { return m_xdg; }

private:
    // The surface has room for the shadow, made as 10.6's window server makes it (AppKit's
    // __NSSetWindowShadow gives CGSSetWindowShadowAndRimParametersWithStretch the key window's
    // standard deviation 20 and density 0.49, 15 points under it, another window's 10, 0.44 and 10;
    // CoreGraphics' CGXWindowShadowGet blurs the window's outline by a tent 1.7 deviations wide,
    // adds a rim one pixel around it and applies its gamma, the 1.23 power): the same as
    // WindowServer's compositor does on X11 (windowserver/compositor.cpp)
    static constexpr int ShadowLeft = 40, ShadowRight = 40, ShadowTop = 25, ShadowBottom = 55;
    bool hasShadow() const { return kind == Window::Titled; }
    int bufferWidth() const { return int(m_size.w) + (hasShadow() ? ShadowLeft + ShadowRight : 0); }
    int bufferHeight() const { return int(m_size.h) + (hasShadow() ? ShadowTop + ShadowBottom : 0); }
    cairo_surface_t *shadow()
    {
        if (m_shadow)
            return m_shadow;
        const int bw = bufferWidth(), bh = bufferHeight();
        const double sigma = m_active ? 20 : 10, density = m_active ? 0.49 : 0.44;
        const int dy = m_active ? 15 : 10, radius = int(std::lround(1.7 * sigma));
        // a rectangle blurred by the tent: the product of its blurred edges across and down
        auto blurred = [radius](int n, int from, int to) {
            std::vector<double> v(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                double sum = 0;
                for (int d = 1 - radius; d < radius; ++d)
                    if (i + d >= from && i + d < to)
                        sum += radius - std::abs(d);
                v[size_t(i)] = sum / (double(radius) * radius);
            }
            return v;
        };
        const int right = ShadowLeft + int(m_size.w), bottom = ShadowTop + int(m_size.h);
        const std::vector<double> across = blurred(bw, ShadowLeft, right);
        const std::vector<double> down = blurred(bh, ShadowTop + dy, bottom + dy);
        m_shadow = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
        unsigned char *data = cairo_image_surface_get_data(m_shadow);
        const int stride = cairo_image_surface_get_stride(m_shadow);
        const int rim = m_active ? (255 >> 3) + (255 >> 5) : (255 >> 3) - (255 >> 6);
        for (int y = 0; y < bh; ++y) {
            auto *row = reinterpret_cast<uint32_t *>(data + size_t(y) * size_t(stride));
            for (int x = 0; x < bw; ++x) {
                const bool around = x >= ShadowLeft - 1 && x <= right && y >= ShadowTop - 1 && y <= bottom;
                const int value = int(std::lround(255 * density * across[size_t(x)] * down[size_t(y)])) + (around ? rim : 0);
                row[x] = uint32_t(std::lround(255 * std::pow(std::min(value, 255) / 255.0, 1.23))) << 24;   // black
            }
        }
        cairo_surface_mark_dirty(m_shadow);
        return m_shadow;
    }
    void dropShadow()
    {
        if (m_shadow)
            cairo_surface_destroy(m_shadow);
        m_shadow = nullptr;
    }
    bool allocate(Buffer &b)
    {
        const int w = bufferWidth(), h = bufferHeight();
        const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
        b.size = size_t(stride) * size_t(h);
        const int fd = memfd_create("aquakit", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, off_t(b.size)) != 0) {
            if (fd >= 0)
                close(fd);
            return false;
        }
        b.data = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (b.data == MAP_FAILED) {
            b.data = nullptr;
            close(fd);
            return false;
        }
        wl_shm_pool *pool = wl_shm_create_pool(wl->shm, fd, int32_t(b.size));
        // cairo's ARGB32 (premultiplied, native endian) is wl_shm's ARGB8888
        b.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        static const wl_buffer_listener released = {[](void *data, wl_buffer *) {
            auto *b = static_cast<Buffer *>(data);
            if (b->dead)
                delete b;
            else
                b->busy = false;
        }};
        b.surface = cairo_image_surface_create_for_data(static_cast<unsigned char *>(b.data), CAIRO_FORMAT_ARGB32, w, h, stride);
        wl_buffer_add_listener(b.buffer, &released, &b);
        return true;
    }
    Size m_size;
    WlWindow *m_parent;
    Point m_at;
    std::string m_title;
    xdg_surface *m_xdg = nullptr;
    xdg_toplevel *m_toplevel = nullptr;
    xdg_popup *m_popup = nullptr;
    zwlr_layer_surface_v1 *m_layer = nullptr;
    int m_statusTop = -1, m_statusBottom = -1;   // a status bar's zones (setStatusBar), -1 for a window
    wl_callback *m_frame = nullptr;
    bool m_configured = false;
    bool m_resizable = false;
    Size m_minSize, m_maxSize, m_asked;   // m_asked: the compositor's size in its last configure (0: ours)
    std::vector<Buffer *> m_buffers;
    Buffer *m_painting = nullptr;
    bool m_active = false, m_roundedBottom = false;
    cairo_surface_t *m_shadow = nullptr;
};

void WlWindow::show()
{
    if (m_xdg || m_layer)
        return;
    if (m_statusTop >= 0 && wl->layerShell) {
        // the status bar: a layer surface, the program's name its namespace ("com.apple.systemuiserver")
        std::string scope = "com.apple." + programName();
        std::transform(scope.begin(), scope.end(), scope.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        m_layer = zwlr_layer_shell_v1_get_layer_surface(wl->layerShell, surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_TOP, scope.c_str());
        static const zwlr_layer_surface_v1_listener layerListener = {
            [](void *data, zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t width, uint32_t height) {
                auto *w = static_cast<WlWindow *>(data);
                zwlr_layer_surface_v1_ack_configure(l, serial);
                w->m_configured = true;
                // the screen's width
                if (width > 0 && height > 0 && (double(width) != w->m_size.w || double(height) != w->m_size.h)) {
                    const Size s{double(width), double(height)};
                    w->resize(s);
                    w->window->nativeResize(s);
                }
                w->window->nativeExpose();
            },
            [](void *data, zwlr_layer_surface_v1 *) {
                auto *w = static_cast<WlWindow *>(data);
                App::shared().later([win = w->window] { win->nativeClose(); });
            },
        };
        zwlr_layer_surface_v1_add_listener(m_layer, &layerListener, this);
        const bool top = m_statusTop > 0 || m_statusBottom <= 0;
        zwlr_layer_surface_v1_set_anchor(m_layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                                      (top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM));
        zwlr_layer_surface_v1_set_size(m_layer, 0, uint32_t(m_size.h));
        zwlr_layer_surface_v1_set_exclusive_zone(m_layer, top ? m_statusTop : m_statusBottom);
        zwlr_layer_surface_v1_set_keyboard_interactivity(m_layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(surface);
        return;
    }
    m_xdg = xdg_wm_base_get_xdg_surface(wl->wm, surface);
    static const xdg_surface_listener surfaceListener = {[](void *data, xdg_surface *s, uint32_t serial) {
        auto *w = static_cast<WlWindow *>(data);
        xdg_surface_ack_configure(s, serial);
        w->m_configured = true;
        if (w->m_resizable && w->m_asked.w > 0 && w->m_asked.h > 0 &&
            (w->m_asked.w != w->m_size.w || w->m_asked.h != w->m_size.h)) {
            const Size s{std::max(w->m_asked.w, w->m_minSize.w), std::max(w->m_asked.h, w->m_minSize.h)};
            w->resize(s);
            w->window->nativeResize(s);
        }
        w->window->nativeExpose();
    }};
    xdg_surface_add_listener(m_xdg, &surfaceListener, this);
    if (kind == Window::Menu && m_parent && (m_parent->xdg() || m_parent->layer())) {
        xdg_positioner *pos = xdg_wm_base_create_positioner(wl->wm);
        xdg_positioner_set_size(pos, int(m_size.w), int(m_size.h));
        xdg_positioner_set_anchor_rect(pos, int(m_at.x), int(m_at.y), 1, 1);
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                                                          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
        // a layer's menu (the menu bar's): its parent given by the layer surface
        m_popup = xdg_surface_get_popup(m_xdg, m_parent->xdg(), pos);
        if (!m_parent->xdg())
            zwlr_layer_surface_v1_get_popup(m_parent->layer(), m_popup);
        xdg_positioner_destroy(pos);
        static const xdg_popup_listener popupListener = {
            [](void *, xdg_popup *, int32_t, int32_t, int32_t, int32_t) {},
            [](void *data, xdg_popup *) {
                // a click outside, or the compositor taking the grab back
                auto *w = static_cast<WlWindow *>(data);
                App::shared().later([win = w->window] { win->nativeDismiss(); });
            },
            nullptr,
        };
        xdg_popup_add_listener(m_popup, &popupListener, this);
        // a menu has the pointer and the keyboard until it closes (before its first commit)
        xdg_popup_grab(m_popup, wl->seat, wl->lastSerial);
    } else {
        m_toplevel = xdg_surface_get_toplevel(m_xdg);
        static const xdg_toplevel_listener toplevelListener = {
            [](void *data, xdg_toplevel *, int32_t width, int32_t height, wl_array *states) {
                auto *w = static_cast<WlWindow *>(data);
                w->m_asked = {double(width), double(height)};
                bool active = false;
                for (auto *s = static_cast<uint32_t *>(states->data);
                     reinterpret_cast<char *>(s) < static_cast<char *>(states->data) + states->size; ++s)
                    if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
                        active = true;
                w->setActive(active);
                w->window->nativeFocus(active);
            },
            [](void *data, xdg_toplevel *) {
                auto *w = static_cast<WlWindow *>(data);
                App::shared().later([win = w->window] { win->nativeClose(); });
            },
            nullptr,
            nullptr,
        };
        xdg_toplevel_add_listener(m_toplevel, &toplevelListener, this);
        xdg_toplevel_set_title(m_toplevel, m_title.c_str());
        // the app id: the program's name, as WM_CLASS on X11 (and the .desktop entry's name)
        xdg_toplevel_set_app_id(m_toplevel, programName().c_str());
        if (kind == Window::FullScreen) {
            xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
        } else if (m_resizable) {
            xdg_toplevel_set_min_size(m_toplevel, int(m_minSize.w), int(m_minSize.h));
            xdg_toplevel_set_max_size(m_toplevel, int(m_maxSize.w), int(m_maxSize.h));
        } else {
            // System Preferences' window doesn't resize
            xdg_toplevel_set_min_size(m_toplevel, int(m_size.w), int(m_size.h));
            xdg_toplevel_set_max_size(m_toplevel, int(m_size.w), int(m_size.h));
        }
    }
    wl_surface_commit(surface);
}

WlPlatform::~WlPlatform()
{
    stopRepeat();
    if (state)
        xkb_state_unref(state);
    if (keymap)
        xkb_keymap_unref(keymap);
    if (xkb)
        xkb_context_unref(xkb);
    if (cursorTheme)
        wl_cursor_theme_destroy(cursorTheme);
    if (display)
        wl_display_disconnect(display);
    wl = nullptr;
}

WlWindow *WlPlatform::find(wl_surface *s) const
{
    for (WlWindow *w : windows)
        if (w->surface == s)
            return w;
    return nullptr;
}

void WlWindow::setCursor(const std::string &name)
{
    if (wl->cursorName == name)
        return;
    wl->cursorName = name;
    if (wl->pointerFocus == this)
        wl->setCursor(wl->enterSerial, kind == Window::FullScreen);
}

void WlPlatform::setCursor(uint32_t serial, bool hidden)
{
    if (!pointer)
        return;
    if (hidden) {
        wl_pointer_set_cursor(pointer, serial, nullptr, 0, 0);
        return;
    }
    if (!cursorTheme) {
        const char *size = std::getenv("XCURSOR_SIZE");
        cursorTheme = wl_cursor_theme_load(std::getenv("XCURSOR_THEME"), size ? std::atoi(size) : 24, shm);
        cursorSurface = wl_compositor_create_surface(compositor);
    }
    enterSerial = serial;
    wl_cursor *c = cursorTheme ? wl_cursor_theme_get_cursor(cursorTheme, cursorName.c_str()) : nullptr;
    if (!c && cursorTheme)
        c = wl_cursor_theme_get_cursor(cursorTheme, "default");
    if (!c && cursorTheme)
        c = wl_cursor_theme_get_cursor(cursorTheme, "left_ptr");
    if (!c || !c->image_count)
        return;
    wl_cursor_image *img = c->images[0];
    wl_surface_attach(cursorSurface, wl_cursor_image_get_buffer(img), 0, 0);
    wl_surface_damage_buffer(cursorSurface, 0, 0, int(img->width), int(img->height));
    wl_surface_commit(cursorSurface);
    wl_pointer_set_cursor(pointer, serial, cursorSurface, int(img->hotspot_x), int(img->hotspot_y));
}

void WlPlatform::key(uint32_t key)
{
    if (!state || !keyboardFocus)
        return;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(state, key + 8);
    char buf[16] = {};
    xkb_state_key_get_utf8(state, key + 8, buf, sizeof buf);
    const bool printable = (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7F;
    keyboardFocus->window->nativeKey(sym, printable ? std::string(buf) : std::string(), modifiers());
}

// ---- input

// filled by name: the listener grows with libwayland's versions
const wl_pointer_listener pointerListener = [] {
    wl_pointer_listener l{};
    l.enter = [](void *, wl_pointer *, uint32_t serial, wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
        wl->pointerFocus = wl->find(s);
        wl->pointerAt = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
        if (wl->pointerFocus)
            wl->pointerAt = wl->pointerFocus->fromSurface(wl->pointerAt);
        wl->setCursor(serial, wl->pointerFocus && wl->pointerFocus->kind == Window::FullScreen);
        if (wl->pointerFocus)
            wl->pointerFocus->window->nativeMotion(wl->pointerAt, wl->pointerAt);
    };
    l.leave = [](void *, wl_pointer *, uint32_t, wl_surface *) {
        if (wl->pointerFocus)
            wl->pointerFocus->window->nativeLeave();
        wl->pointerFocus = nullptr;
    };
    l.motion = [](void *, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        wl->pointerAt = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
        if (wl->pointerFocus)
            wl->pointerAt = wl->pointerFocus->fromSurface(wl->pointerAt);
        if (wl->pointerFocus)
            wl->pointerFocus->window->nativeMotion(wl->pointerAt, wl->pointerAt);
    };
    l.button = [](void *, wl_pointer *, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
        wl->lastSerial = serial;
        if ((button == BTN_LEFT || button == BTN_RIGHT) && wl->pointerFocus)
            wl->pointerFocus->window->nativeButton(state == WL_POINTER_BUTTON_STATE_PRESSED, wl->pointerAt, wl->pointerAt,
                                                   wl->modifiers(), time, button == BTN_RIGHT);
    };
    l.axis = [](void *, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value) {   // axis: 10 a wheel notch
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && wl->pointerFocus)
            wl->pointerFocus->window->nativeScroll(wl->pointerAt, wl_fixed_to_double(value) / 10);
    };
    l.frame = [](void *, wl_pointer *) {};
    l.axis_source = [](void *, wl_pointer *, uint32_t) {};
    l.axis_stop = [](void *, wl_pointer *, uint32_t, uint32_t) {};
    l.axis_discrete = [](void *, wl_pointer *, uint32_t, int32_t) {};
    return l;
}();

const wl_keyboard_listener keyboardListener = {
    [](void *, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size) {   // keymap
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
            close(fd);
            return;
        }
        void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED)
            return;
        xkb_keymap *km = xkb_keymap_new_from_string(wl->xkb, static_cast<const char *>(map), XKB_KEYMAP_FORMAT_TEXT_V1,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(map, size);
        if (!km)
            return;
        if (wl->state)
            xkb_state_unref(wl->state);
        if (wl->keymap)
            xkb_keymap_unref(wl->keymap);
        wl->keymap = km;
        wl->state = xkb_state_new(km);
    },
    [](void *, wl_keyboard *, uint32_t serial, wl_surface *s, wl_array *) {   // enter
        wl->lastSerial = serial;
        wl->keyboardFocus = wl->find(s);
    },
    [](void *, wl_keyboard *, uint32_t, wl_surface *) {   // leave
        wl->keyboardFocus = nullptr;
        wl->stopRepeat();
    },
    [](void *, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {   // key
        wl->lastSerial = serial;
        wl->stopRepeat();
        if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        wl->key(key);
        // held down: again after the delay, then at the rate
        if (wl->keymap && xkb_keymap_key_repeats(wl->keymap, key + 8) && wl->repeatRate > 0)
            wl->repeatTimer = App::shared().addTimer(wl->repeatDelay / 1000.0, false, [key] {
                wl->repeatTimer = App::shared().addTimer(1.0 / wl->repeatRate, true, [key] { wl->key(key); });
            });
    },
    [](void *, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {   // modifiers
        if (wl->state)
            xkb_state_update_mask(wl->state, depressed, latched, locked, 0, 0, group);
    },
    [](void *, wl_keyboard *, int32_t rate, int32_t delay) {   // repeat_info
        wl->repeatRate = rate;
        wl->repeatDelay = delay;
    },
};

const wl_seat_listener seatListener = {
    [](void *, wl_seat *seat, uint32_t caps) {
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->pointer) {
            wl->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(wl->pointer, &pointerListener, nullptr);
        }
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->keyboard) {
            wl->keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(wl->keyboard, &keyboardListener, nullptr);
        }
    },
    [](void *, wl_seat *, const char *) {},
};

const wl_output_listener outputListener = {
    [](void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {},   // geometry
    [](void *, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t) {   // mode
        if (flags & WL_OUTPUT_MODE_CURRENT)
            wl->outputWidth = w, wl->outputHeight = h;
    },
    [](void *, wl_output *) {},            // done
    [](void *, wl_output *, int32_t) {},   // scale
    nullptr,
    nullptr,
};

const xdg_wm_base_listener wmListener = {[](void *, xdg_wm_base *wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }};

const wl_registry_listener registryListener = {
    [](void *, wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
        const std::string i = interface;
        if (i == wl_compositor_interface.name)
            wl->compositor = static_cast<wl_compositor *>(wl_registry_bind(r, name, &wl_compositor_interface, std::min(version, 4u)));
        else if (i == wl_shm_interface.name)
            wl->shm = static_cast<wl_shm *>(wl_registry_bind(r, name, &wl_shm_interface, 1));
        else if (i == xdg_wm_base_interface.name) {
            wl->wm = static_cast<xdg_wm_base *>(wl_registry_bind(r, name, &xdg_wm_base_interface, std::min(version, 2u)));
            xdg_wm_base_add_listener(wl->wm, &wmListener, nullptr);
        } else if (i == zwlr_layer_shell_v1_interface.name && !wl->layerShell) {
            wl->layerShell = static_cast<zwlr_layer_shell_v1 *>(wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, std::min(version, 3u)));
        } else if (i == wl_seat_interface.name && !wl->seat) {
            wl->seat = static_cast<wl_seat *>(wl_registry_bind(r, name, &wl_seat_interface, std::min(version, 5u)));
            wl_seat_add_listener(wl->seat, &seatListener, nullptr);
        } else if (i == wl_output_interface.name && !wl->output) {
            wl->output = static_cast<wl_output *>(wl_registry_bind(r, name, &wl_output_interface, std::min(version, 2u)));
            wl_output_add_listener(wl->output, &outputListener, nullptr);
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

std::unique_ptr<NativeWindow> WlPlatform::createWindow(Window *w, Size surface, Window::Kind kind, Window *parent, Point at)
{
    WlWindow *p = parent ? static_cast<WlWindow *>(parent->native()) : nullptr;
    return std::make_unique<WlWindow>(w, surface, kind, p, at);
}

} // namespace

std::unique_ptr<Platform> Platform::wayland()
{
    auto set = [](const char *name) {
        const char *v = std::getenv(name);
        return v && *v;
    };
    if (!set("WAYLAND_DISPLAY") && !set("WAYLAND_SOCKET"))
        return nullptr;
    auto p = std::make_unique<WlPlatform>();
    wl = p.get();
    p->display = wl_display_connect(nullptr);
    if (!p->display)
        return nullptr;
    p->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    p->registry = wl_display_get_registry(p->display);
    wl_registry_add_listener(p->registry, &registryListener, nullptr);
    wl_display_roundtrip(p->display);   // the globals
    wl_display_roundtrip(p->display);   // the seat's capabilities, the output's mode
    if (!p->compositor || !p->shm || !p->wm)
        return nullptr;
    p->windowServer();
    return p;
}

} // namespace AK
