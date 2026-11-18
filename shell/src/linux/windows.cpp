// SPDX-License-Identifier: LGPL-2.1-or-later
#include "windows.h"

#include "runloop.h"

#ifdef SNOWLEOPARD_WAYLAND
#include "windowserverclient.h"
namespace WS = WindowServerClient;
#define ON_WAYLAND(...)                                                                                                                   \
    if (wl) {                                                                                                                             \
        __VA_ARGS__;                                                                                                                      \
    }
#else
#define ON_WAYLAND(...)
#endif

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>

namespace Windows {

namespace {

xcb_connection_t *c = nullptr;
xcb_window_t r = 0;
bool wl = false;   // the Wayland window server's
std::function<void()> changed;
std::vector<xcb_atom_t> watched;
xcb_window_t hotKeyWindow = 0;
std::function<void(int)> hotKeyPressed;
int coalesce = 0;

void notify()
{
    if (coalesce || !changed)
        return;
    coalesce = AK::RunLoop::main().addTimer(0.05, false, [] {
        coalesce = 0;
        if (changed)
            changed();
    });
}

void readEvents()
{
    while (xcb_generic_event_t *ev = xcb_poll_for_event(c)) {
        switch (ev->response_type & ~0x80) {
        case XCB_PROPERTY_NOTIFY: {
            auto *p = reinterpret_cast<xcb_property_notify_event_t *>(ev);
            if (p->window == r)
                for (xcb_atom_t a : watched)
                    if (a == p->atom)
                        notify();
            break;
        }
        case XCB_DESTROY_NOTIFY: {
            auto *d = reinterpret_cast<xcb_destroy_notify_event_t *>(ev);
            if (d->event == d->window || !hasWindowManager())
                notify();
            break;
        }
        case XCB_MAP_NOTIFY:
        case XCB_UNMAP_NOTIFY:
        case XCB_CONFIGURE_NOTIFY:
            if (!hasWindowManager())
                notify();
            break;
        case XCB_CLIENT_MESSAGE: {
            auto *m = reinterpret_cast<xcb_client_message_event_t *>(ev);
            if (m->window == hotKeyWindow && m->type == atom("_SNOWLEOPARD_HOTKEY") && hotKeyPressed) {
                const int hotKey = int(m->data.data32[0]);
                AK::RunLoop::main().later([hotKey] {
                    if (hotKeyPressed)
                        hotKeyPressed(hotKey);
                });
            }
            break;
        }
        default:
            break;
        }
        free(ev);
    }
}

xcb_get_property_reply_t *property(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
    if (!c || !w)
        return nullptr;
    return xcb_get_property_reply(c, xcb_get_property(c, 0, w, prop, type, 0, 1 << 20), nullptr);
}

bool isOwnOrSpecial(xcb_window_t w)
{
    for (const char *t : {"_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_MENU",
                          "_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_TOOLTIP",
                          "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_SPLASH"})
        if (hasType(w, t))
            return true;
    return hasState(w, "_NET_WM_STATE_SKIP_TASKBAR");
}

void clientMessage(xcb_window_t w, xcb_atom_t type, std::initializer_list<uint32_t> data)
{
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = w;
    ev.type = type;
    int i = 0;
    for (uint32_t d : data)
        ev.data.data32[i++] = d;
    xcb_send_event(c, 0, r, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&ev));
}

} // namespace

#ifdef SNOWLEOPARD_WAYLAND
const WS::WindowInfo *info(xcb_window_t w) { return wl ? WS::window(w) : nullptr; }
uint32_t flags(xcb_window_t w)
{
    const WS::WindowInfo *i = info(w);
    return i ? i->flags : 0;
}
#endif

bool open()
{
    if (c || wl)
        return true;
#ifdef SNOWLEOPARD_WAYLAND
    const char *server = getenv("SNOWLEOPARD_WINDOW_SERVER");
    if (server && !strcmp(server, "wayland") && WS::connect()) {
        wl = true;
        AK::RunLoop::main().watch(WS::fd(), [] { WS::dispatch(); });
        WS::onChange([] { notify(); });
        return true;
    }
#endif
    int screen = 0;
    c = xcb_connect(nullptr, &screen);
    if (!c || xcb_connection_has_error(c)) {
        c = nullptr;
        return false;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen; ++i)
        xcb_screen_next(&it);
    r = it.data->root;
    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(c, r, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(c);
    AK::RunLoop::main().watch(xcb_get_file_descriptor(c), readEvents);
    // events xcb queued while waiting for a reply leave nothing on the socket to wake the loop
    AK::RunLoop::main().addTimer(0.25, true, readEvents);
    return true;
}

xcb_connection_t *conn() { return c; }
bool wayland() { return wl; }
xcb_window_t root() { return r; }

xcb_atom_t atom(const char *name)
{
    static std::map<std::string, xcb_atom_t> cache;
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;
    xcb_atom_t a = XCB_ATOM_NONE;
    if (c)
        if (auto *reply = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, uint16_t(strlen(name)), name), nullptr)) {
            a = reply->atom;
            free(reply);
        }
    cache[name] = a;
    return a;
}

std::vector<uint32_t> cardinals(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
    std::vector<uint32_t> out;
    if (auto *reply = property(w, prop, type)) {
        if (reply->format == 32) {
            auto *data = static_cast<uint32_t *>(xcb_get_property_value(reply));
            out.assign(data, data + xcb_get_property_value_length(reply) / 4);
        }
        free(reply);
    }
    return out;
}

std::string bytes(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
    std::string out;
    if (auto *reply = property(w, prop, type)) {
        out.assign(static_cast<const char *>(xcb_get_property_value(reply)), size_t(xcb_get_property_value_length(reply)));
        free(reply);
    }
    return out;
}

bool hasType(xcb_window_t w, const char *type)
{
#ifdef SNOWLEOPARD_WAYLAND
    if (wl) {
        const std::string t = type;
        if (t == "_NET_WM_WINDOW_TYPE_UTILITY")
            return flags(w) & WS::Utility;
        if (t == "_NET_WM_WINDOW_TYPE_DIALOG")
            return flags(w) & WS::Dialog;
        return false;   // a desktop, a dock: layers, not windows
    }
#endif
    const xcb_atom_t a = atom(type);
    for (uint32_t t : cardinals(w, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM))
        if (t == a)
            return true;
    return false;
}

bool hasState(xcb_window_t w, const char *state)
{
#ifdef SNOWLEOPARD_WAYLAND
    if (wl) {
        const std::string st = state;
        if (st == "_NET_WM_STATE_SKIP_TASKBAR")
            return flags(w) & WS::SkipTaskbar;
        if (st == "_NET_WM_STATE_HIDDEN")
            return flags(w) & (WS::Minimized | WS::Hidden);
        if (st == "_NET_WM_STATE_FULLSCREEN")
            return flags(w) & WS::Fullscreen;
        if (st == "_NET_WM_STATE_ABOVE")
            return flags(w) & WS::Above;
        return false;
    }
#endif
    const xcb_atom_t a = atom(state);
    for (uint32_t s : cardinals(w, atom("_NET_WM_STATE"), XCB_ATOM_ATOM))
        if (s == a)
            return true;
    return false;
}

std::vector<xcb_window_t> allClientWindows()
{
    std::vector<xcb_window_t> out;
#ifdef SNOWLEOPARD_WAYLAND
    if (wl) {
        for (const WS::WindowInfo &i : WS::windows())
            out.push_back(i.id);
        return out;
    }
#endif
    for (uint32_t w : cardinals(r, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW))
        out.push_back(w);
    return out;
}

uint32_t frontApp(uint32_t *serial)
{
    uint32_t pid = 0, s = 0;
#ifdef SNOWLEOPARD_WAYLAND
    if (wl) {
        pid = WS::frontPid();
        s = WS::frontSerial();
    } else
#endif
    {
        const auto v = cardinals(r, atom("_SNOWLEOPARD_FRONT_APP"), XCB_ATOM_CARDINAL);
        if (v.size() >= 2)
            pid = v[0], s = v[1];
    }
    if (!pid || kill(pid_t(pid), 0) != 0)
        return 0;
    if (serial)
        *serial = s;
    return pid;
}

bool appMenu(xcb_window_t w, std::string *service, std::string *path)
{
#ifdef SNOWLEOPARD_WAYLAND
    if (wl) {
        const WS::WindowInfo *i = info(w);
        *service = i ? i->menuService : std::string();
        *path = i ? i->menuPath : std::string();
        return !service->empty() && !path->empty();
    }
#endif
    *service = bytes(w, atom("_KDE_NET_WM_APPMENU_SERVICE_NAME"), XCB_ATOM_STRING);
    *path = bytes(w, atom("_KDE_NET_WM_APPMENU_OBJECT_PATH"), XCB_ATOM_STRING);
    return !service->empty() && !path->empty();
}

bool hasWindowManager()
{
    if (wl)
        return true;
    const auto check = cardinals(r, atom("_NET_SUPPORTING_WM_CHECK"), XCB_ATOM_WINDOW);
    return !check.empty() && check[0];
}

std::vector<xcb_window_t> clientWindows()
{
    std::vector<xcb_window_t> out;
#ifdef SNOWLEOPARD_WAYLAND
    if (wl) {
        for (const WS::WindowInfo &i : WS::windows())
            if (!(i.flags & WS::SkipTaskbar))
                out.push_back(i.id);
        return out;
    }
#endif
    if (!c)
        return out;
    if (hasWindowManager()) {
        auto list = cardinals(r, atom("_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW);
        if (list.empty())
            list = cardinals(r, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
        for (uint32_t w : list)
            if (!isOwnOrSpecial(w))
                out.push_back(w);
        return out;
    }
    // no window manager: the root's mapped children with a WM_CLASS
    auto *tree = xcb_query_tree_reply(c, xcb_query_tree(c, r), nullptr);
    if (!tree)
        return out;
    xcb_window_t *children = xcb_query_tree_children(tree);
    for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
        auto *attr = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, children[i]), nullptr);
        const bool viewable = attr && attr->map_state == XCB_MAP_STATE_VIEWABLE && !attr->override_redirect;
        free(attr);
        if (viewable && !wmClass(children[i]).empty() && !isOwnOrSpecial(children[i]))
            out.push_back(children[i]);
    }
    free(tree);
    return out;
}

xcb_window_t activeWindow()
{
    ON_WAYLAND(return WS::active())
    if (hasWindowManager()) {
        const auto active = cardinals(r, atom("_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW);
        return active.empty() ? 0 : active[0];
    }
    const auto clients = clientWindows();
    return clients.empty() ? 0 : clients.back();
}

static std::pair<std::string, std::string> classHint(xcb_window_t w)
{
    const std::string raw = bytes(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
    const size_t nul = raw.find('\0');
    if (nul == std::string::npos)
        return {raw, {}};
    std::string second = raw.substr(nul + 1);
    if (!second.empty() && second.back() == '\0')
        second.pop_back();
    return {raw.substr(0, nul), second};
}

std::string wmClass(xcb_window_t w)
{
    ON_WAYLAND(const WS::WindowInfo *i = info(w); return i ? i->appId : std::string())
    return classHint(w).second;
}

std::string wmInstance(xcb_window_t w)
{
    ON_WAYLAND(const WS::WindowInfo *i = info(w); return i ? i->appId : std::string())
    return classHint(w).first;
}

std::string title(xcb_window_t w)
{
    ON_WAYLAND(const WS::WindowInfo *i = info(w); return i ? i->title : std::string())
    const std::string name = bytes(w, atom("_NET_WM_NAME"), atom("UTF8_STRING"));
    return name.empty() ? bytes(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING) : name;
}

uint32_t pid(xcb_window_t w)
{
    ON_WAYLAND(const WS::WindowInfo *i = info(w); return i ? i->pid : 0)
    const auto p = cardinals(w, atom("_NET_WM_PID"), XCB_ATOM_CARDINAL);
    return p.empty() ? 0 : p[0];
}

bool minimized(xcb_window_t w)
{
    ON_WAYLAND(return flags(w) & (WS::Minimized | WS::Hidden))
    if (hasState(w, "_NET_WM_STATE_HIDDEN"))
        return true;
    const auto s = cardinals(w, atom("WM_STATE"), atom("WM_STATE"));
    return !s.empty() && s[0] == 3;
}

bool hidden(xcb_window_t w)
{
    ON_WAYLAND(return flags(w) & WS::Hidden)
    const auto h = cardinals(w, atom("_SNOWLEOPARD_HIDDEN"), XCB_ATOM_CARDINAL);
    return !h.empty() && h[0];
}

void activate(xcb_window_t w)
{
    ON_WAYLAND(WS::activate(w); return)
    if (!c || !w)
        return;
    if (hasWindowManager()) {
        clientMessage(w, atom("_NET_ACTIVE_WINDOW"), {2, XCB_CURRENT_TIME, 0});
    } else {
        xcb_map_window(c, w);
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(c, w, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, w, XCB_CURRENT_TIME);
    }
    xcb_flush(c);
}

void close(xcb_window_t w)
{
    ON_WAYLAND(WS::close(w); return)
    if (!c || !w)
        return;
    if (hasWindowManager()) {
        clientMessage(w, atom("_NET_CLOSE_WINDOW"), {XCB_CURRENT_TIME, 2});
    } else {
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = w;
        ev.type = atom("WM_PROTOCOLS");
        ev.data.data32[0] = atom("WM_DELETE_WINDOW");
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(c, 0, w, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
    }
    xcb_flush(c);
}

void minimize(xcb_window_t w)
{
    ON_WAYLAND(WS::minimize(w); return)
    if (!c || !w)
        return;
    if (hasWindowManager())
        clientMessage(w, atom("WM_CHANGE_STATE"), {3});
    else
        xcb_unmap_window(c, w);
    xcb_flush(c);
}

void hide(xcb_window_t w)
{
    ON_WAYLAND(WS::hide(w); return)
    if (!c || !w)
        return;
    if (hasWindowManager())
        clientMessage(w, atom("_SNOWLEOPARD_HIDE"), {1});
    else
        xcb_unmap_window(c, w);
    xcb_flush(c);
}

void onChange(std::function<void()> f, const std::vector<std::string> &rootAtoms)
{
    changed = std::move(f);
    watched.clear();
    for (const char *a : {"_NET_ACTIVE_WINDOW", "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING"})
        watched.push_back(atom(a));
    for (const std::string &a : rootAtoms)
        watched.push_back(atom(a.c_str()));
}

void watchDestroy(xcb_window_t w)
{
    if (!c || !w)
        return;
    const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(c, w, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(c);
}

void registerSymbolicHotKeys(const std::vector<int> &hotKeys, std::function<void(int)> pressed)
{
#ifdef SNOWLEOPARD_WAYLAND
    if (wl) {
        hotKeyPressed = std::move(pressed);
        WS::registerHotKeys(hotKeys, [](int hotKey) {
            AK::RunLoop::main().later([hotKey] {
                if (hotKeyPressed)
                    hotKeyPressed(hotKey);
            });
        });
        return;
    }
#endif
    if (!c)
        return;
    hotKeyPressed = std::move(pressed);
    if (!hotKeyWindow) {
        hotKeyWindow = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, hotKeyWindow, r, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0,
                          nullptr);
    }
    // the root's list (pairs: hot key, window): the others' kept, ours over theirs for the same keys
    std::vector<uint32_t> list, old = cardinals(r, atom("_SNOWLEOPARD_SYMBOLIC_HOTKEYS"), XCB_ATOM_CARDINAL);
    for (size_t i = 0; i + 1 < old.size(); i += 2)
        if (std::find(hotKeys.begin(), hotKeys.end(), int(old[i])) == hotKeys.end()) {
            list.push_back(old[i]);
            list.push_back(old[i + 1]);
        }
    for (int k : hotKeys) {
        list.push_back(uint32_t(k));
        list.push_back(hotKeyWindow);
    }
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, r, atom("_SNOWLEOPARD_SYMBOLIC_HOTKEYS"), XCB_ATOM_CARDINAL, 32, uint32_t(list.size()),
                        list.data());
    xcb_flush(c);
}

} // namespace Windows
