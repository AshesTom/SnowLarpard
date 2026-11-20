// SPDX-License-Identifier: LGPL-2.1-or-later
#include "windowserverclient.h"

#include "snowleopard-window-server-v1-client-protocol.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace WindowServerClient {

namespace {

struct State {
    wl_display *display = nullptr;
    bool own = false, tried = false;
    sl_window_server_v1 *server = nullptr;
    // as of the last done, and what came since
    std::vector<WindowInfo> windows, pending;
    std::vector<uint32_t> order;
    bool orderChanged = false;
    uint32_t active = 0, frontPid = 0, frontSerial = 0;
    Rect workArea, screen;
    std::vector<int> hotKeys;
    std::function<void(int)> hotKeyPressed;
    std::vector<std::function<void()>> changed;
    std::function<void(uint32_t, const std::string &, uint32_t)> key;
};

State &state()
{
    static State s;
    return s;
}

WindowInfo *pendingWindow(uint32_t id)
{
    State &s = state();
    for (WindowInfo &w : s.pending)
        if (w.id == id)
            return &w;
    WindowInfo w;
    w.id = id;
    s.pending.push_back(w);
    return &s.pending.back();
}

const sl_window_server_v1_listener serverListener = {
    // window
    [](void *, sl_window_server_v1 *, uint32_t id, uint32_t pid, const char *appId, const char *title, uint32_t flags, int32_t x, int32_t y,
       int32_t w, int32_t h, const char *menuService, const char *menuPath) {
        WindowInfo *i = pendingWindow(id);
        i->pid = pid;
        i->appId = appId ? appId : "";
        i->title = title ? title : "";
        i->flags = flags;
        i->x = x, i->y = y, i->width = w, i->height = h;
        i->menuService = menuService ? menuService : "";
        i->menuPath = menuPath ? menuPath : "";
        if (std::find(state().order.begin(), state().order.end(), id) == state().order.end()) {
            state().order.push_back(id);
            state().orderChanged = true;
        }
    },
    // window_removed
    [](void *, sl_window_server_v1 *, uint32_t id) {
        State &s = state();
        s.pending.erase(std::remove_if(s.pending.begin(), s.pending.end(), [id](const WindowInfo &w) { return w.id == id; }), s.pending.end());
        s.order.erase(std::remove(s.order.begin(), s.order.end(), id), s.order.end());
        s.orderChanged = true;
    },
    // order
    [](void *, sl_window_server_v1 *, wl_array *windows) {
        State &s = state();
        const auto *v = static_cast<const uint32_t *>(windows->data);
        s.order.assign(v, v + windows->size / sizeof(uint32_t));
        s.orderChanged = true;
    },
    // active
    [](void *, sl_window_server_v1 *, uint32_t window) { state().active = window; },
    // front_app
    [](void *, sl_window_server_v1 *, uint32_t pid, uint32_t serial) {
        state().frontPid = pid;
        state().frontSerial = serial;
    },
    // work_area
    [](void *, sl_window_server_v1 *, int32_t x, int32_t y, int32_t w, int32_t h) { state().workArea = {x, y, w, h}; },
    // screen
    [](void *, sl_window_server_v1 *, int32_t w, int32_t h) { state().screen = {0, 0, w, h}; },
    // hot_key
    [](void *, sl_window_server_v1 *, uint32_t hotKey) {
        if (state().hotKeyPressed)
            state().hotKeyPressed(int(hotKey));
    },
    // key
    [](void *, sl_window_server_v1 *, uint32_t keysym, const char *text, uint32_t modifiers) {
        if (state().key)
            state().key(keysym, text ? text : "", modifiers);
    },
    // done
    [](void *, sl_window_server_v1 *) {
        State &s = state();
        // the windows in the server's order (bottom to top)
        std::vector<WindowInfo> ordered;
        for (uint32_t id : s.order)
            for (const WindowInfo &w : s.pending)
                if (w.id == id)
                    ordered.push_back(w);
        s.windows = ordered;
        s.pending = ordered;
        s.orderChanged = false;
        const auto callbacks = s.changed;
        for (const auto &c : callbacks)
            if (c)
                c();
    },
};

const wl_registry_listener registryListener = {
    [](void *, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
        if (!strcmp(interface, sl_window_server_v1_interface.name) && !state().server) {
            state().server = static_cast<sl_window_server_v1 *>(wl_registry_bind(registry, name, &sl_window_server_v1_interface, 1));
            sl_window_server_v1_add_listener(state().server, &serverListener, nullptr);
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

bool bind(wl_display *display)
{
    State &s = state();
    s.display = display;
    // the globals on a queue of their own, then the server's object back on the default one
    wl_event_queue *queue = wl_display_create_queue(display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *registry = wl_display_get_registry(wrapped);
    wl_proxy_wrapper_destroy(wrapped);
    wl_registry_add_listener(registry, &registryListener, nullptr);
    wl_display_roundtrip_queue(display, queue);
    wl_registry_destroy(registry);
    if (s.server) {
        // the first state (the server sends it all when bound, then done), then the default queue's
        wl_display_roundtrip_queue(display, queue);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(s.server), nullptr);
    }
    wl_event_queue_destroy(queue);
    return s.server != nullptr;
}

struct CaptureResult {
    bool done = false;
    uint32_t ok = 0;
    int x = 0, y = 0;
};

const sl_capture_v1_listener captureListener = {
    [](void *data, sl_capture_v1 *, uint32_t ok, int32_t x, int32_t y) {
        auto *r = static_cast<CaptureResult *>(data);
        r->done = true;
        r->ok = ok;
        r->x = x;
        r->y = y;
    },
};

// A capture's request on a queue of its own, waited for (two seconds at most)
template<typename Make>
CaptureResult waitCapture(Make make)
{
    State &s = state();
    CaptureResult result;
    if (!s.server)
        return result;
    wl_event_queue *queue = wl_display_create_queue(s.display);
    auto *server = static_cast<sl_window_server_v1 *>(wl_proxy_create_wrapper(s.server));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(server), queue);
    sl_capture_v1 *capture = make(server);
    wl_proxy_wrapper_destroy(server);
    sl_capture_v1_add_listener(capture, &captureListener, &result);
    wl_display_flush(s.display);
    for (int i = 0; i < 200 && !result.done; ++i) {
        if (wl_display_dispatch_queue_pending(s.display, queue) > 0)
            continue;
        if (wl_display_prepare_read_queue(s.display, queue) != 0)
            continue;
        wl_display_flush(s.display);
        pollfd p = {wl_display_get_fd(s.display), POLLIN, 0};
        if (poll(&p, 1, 10) > 0)
            wl_display_read_events(s.display);
        else
            wl_display_cancel_read(s.display);
    }
    sl_capture_v1_destroy(capture);
    wl_event_queue_destroy(queue);
    return result;
}

std::string readAll(int fd)
{
    std::string out;
    lseek(fd, 0, SEEK_SET);
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        out.append(buf, size_t(n));
    return out;
}

wl_array toArray(const std::vector<uint32_t> &v)
{
    wl_array a;
    wl_array_init(&a);
    if (void *p = wl_array_add(&a, v.size() * sizeof(uint32_t)))
        memcpy(p, v.data(), v.size() * sizeof(uint32_t));
    return a;
}

} // namespace

bool connect()
{
    State &s = state();
    if (s.tried || s.server)
        return s.server != nullptr;
    s.tried = true;
    if (!getenv("WAYLAND_DISPLAY") && !getenv("WAYLAND_SOCKET"))
        return false;
    wl_display *d = wl_display_connect(nullptr);
    if (!d)
        return false;
    s.own = true;
    if (!bind(d)) {
        // a Wayland compositor without the window server's protocol (KWin's): asked once
        wl_display_disconnect(d);
        s.display = nullptr;
        s.own = false;
        return false;
    }
    return true;
}

bool attach(wl_display *display)
{
    State &s = state();
    if (s.server)
        return true;
    return display && bind(display);
}

bool connected() { return state().server != nullptr; }

int fd() { return connected() ? wl_display_get_fd(state().display) : -1; }

void dispatch()
{
    State &s = state();
    if (!s.server || !s.own)
        return;
    if (wl_display_prepare_read(s.display) == 0) {
        pollfd p = {wl_display_get_fd(s.display), POLLIN, 0};
        if (poll(&p, 1, 0) > 0)
            wl_display_read_events(s.display);
        else
            wl_display_cancel_read(s.display);
    }
    wl_display_dispatch_pending(s.display);
    wl_display_flush(s.display);
}

void flush()
{
    if (connected())
        wl_display_flush(state().display);
}

const std::vector<WindowInfo> &windows() { return state().windows; }

const WindowInfo *window(uint32_t id)
{
    for (const WindowInfo &w : state().windows)
        if (w.id == id)
            return &w;
    return nullptr;
}

uint32_t active() { return state().active; }
uint32_t frontPid() { return state().frontPid; }
uint32_t frontSerial() { return state().frontSerial; }
Rect workArea() { return state().workArea; }
Rect screen() { return state().screen; }

#define WS_CALL(call)                                                                                                                     \
    do {                                                                                                                                  \
        if (connected()) {                                                                                                                \
            call;                                                                                                                         \
            flush();                                                                                                                      \
        }                                                                                                                                 \
    } while (0)

void activate(uint32_t w) { WS_CALL(sl_window_server_v1_activate(state().server, w)); }
void close(uint32_t w) { WS_CALL(sl_window_server_v1_close(state().server, w)); }
void minimize(uint32_t w) { WS_CALL(sl_window_server_v1_minimize(state().server, w)); }
void hide(uint32_t w) { WS_CALL(sl_window_server_v1_hide(state().server, w)); }
void setIconGeometry(uint32_t w, int x, int y, int width, int height)
{
    WS_CALL(sl_window_server_v1_set_icon_geometry(state().server, w, x, y, width, height));
}
void setFrontApp(uint32_t pid) { WS_CALL(sl_window_server_v1_set_front_app(state().server, pid)); }

void setKeyEquivalents(const std::vector<uint32_t> &keysyms)
{
    if (!connected())
        return;
    wl_array a = toArray(keysyms);
    sl_window_server_v1_set_key_equivalents(state().server, &a);
    wl_array_release(&a);
    flush();
}

void setMagicMirror(const std::string &layer, int direction, const std::vector<int> &quad)
{
    if (!connected())
        return;
    std::vector<uint32_t> v;
    for (int q : quad)
        v.push_back(uint32_t(q));
    wl_array a = toArray(v);
    sl_window_server_v1_set_magic_mirror(state().server, layer.c_str(), uint32_t(std::max(0, direction)), &a);
    wl_array_release(&a);
    flush();
}

void registerHotKeys(const std::vector<int> &hotKeys, std::function<void(int)> pressed)
{
    State &s = state();
    s.hotKeys = hotKeys;
    s.hotKeyPressed = std::move(pressed);
    if (!connected())
        return;
    std::vector<uint32_t> v(hotKeys.begin(), hotKeys.end());
    wl_array a = toArray(v);
    sl_window_server_v1_register_hot_keys(s.server, &a);
    wl_array_release(&a);
    flush();
}

std::string captureWindow(uint32_t window, bool shadow, int *x, int *y)
{
    const int fd = memfd_create("capture", MFD_CLOEXEC);
    if (fd < 0)
        return {};
    const CaptureResult r = waitCapture([&](sl_window_server_v1 *s) { return sl_window_server_v1_capture_window(s, window, shadow ? 1 : 0, fd); });
    std::string png = r.ok ? readAll(fd) : std::string();
    ::close(fd);
    if (x)
        *x = r.x;
    if (y)
        *y = r.y;
    return png;
}

std::string captureScreen(int x, int y, int width, int height)
{
    const int fd = memfd_create("capture", MFD_CLOEXEC);
    if (fd < 0)
        return {};
    const CaptureResult r = waitCapture([&](sl_window_server_v1 *s) { return sl_window_server_v1_capture_screen(s, x, y, width, height, fd); });
    std::string png = r.ok ? readAll(fd) : std::string();
    ::close(fd);
    return png;
}

uint32_t pickWindow(int x, int y)
{
    const CaptureResult r = waitCapture([&](sl_window_server_v1 *s) { return sl_window_server_v1_pick_window(s, x, y); });
    return r.ok ? uint32_t(r.x) : 0;
}

void onChange(std::function<void()> changed) { state().changed.push_back(std::move(changed)); }

void onKey(std::function<void(uint32_t, const std::string &, uint32_t)> key) { state().key = std::move(key); }

} // namespace WindowServerClient
