// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dbusmenu.h"

#include "dbus.h"
#include "runloop.h"
#include "windows.h"

#include <cstdio>
#include <cstring>

namespace {

const char *const RegistrarService = "com.canonical.AppMenu.Registrar";
const char *const RegistrarPath = "/com/canonical/AppMenu/Registrar";
const char *const MenuInterface = "com.canonical.dbusmenu";

int onRegister(sd_bus_message *m, void *userdata, sd_bus_error *)
{
    auto *r = static_cast<AppMenuRegistrar *>(userdata);
    uint32_t window = 0;
    const char *path = nullptr;
    if (sd_bus_message_read(m, "uo", &window, &path) < 0)
        return -EINVAL;
    const char *sender = sd_bus_message_get_sender(m);
    r->registerWindow(window, sender ? sender : "", path ? path : "/");
    sd_bus_emit_signal(sd_bus_message_get_bus(m), RegistrarPath, RegistrarService, "WindowRegistered", "uso", window,
                       sender ? sender : "", path ? path : "/");
    return sd_bus_reply_method_return(m, "");
}

int onUnregister(sd_bus_message *m, void *userdata, sd_bus_error *)
{
    auto *r = static_cast<AppMenuRegistrar *>(userdata);
    uint32_t window = 0;
    if (sd_bus_message_read(m, "u", &window) < 0)
        return -EINVAL;
    r->unregisterWindow(window);
    sd_bus_emit_signal(sd_bus_message_get_bus(m), RegistrarPath, RegistrarService, "WindowUnregistered", "u", window);
    return sd_bus_reply_method_return(m, "");
}

int onGetMenu(sd_bus_message *m, void *userdata, sd_bus_error *)
{
    auto *r = static_cast<AppMenuRegistrar *>(userdata);
    uint32_t window = 0;
    if (sd_bus_message_read(m, "u", &window) < 0)
        return -EINVAL;
    auto it = r->menus.find(window);
    const std::string service = it == r->menus.end() ? "" : it->second.first;
    const std::string path = it == r->menus.end() ? "/" : it->second.second;
    return sd_bus_reply_method_return(m, "so", service.c_str(), path.c_str());
}

const sd_bus_vtable registrarVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterWindow", "uo", "", onRegister, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UnregisterWindow", "u", "", onUnregister, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetMenuForWindow", "u", "so", onGetMenu, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("WindowRegistered", "uso", 0),
    SD_BUS_SIGNAL("WindowUnregistered", "u", 0),
    SD_BUS_VTABLE_END};

int onNameOwnerChanged(sd_bus_message *m, void *userdata, sd_bus_error *)
{
    const char *name = nullptr, *before = nullptr, *after = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &before, &after) >= 0 && after && !*after && name)
        static_cast<AppMenuRegistrar *>(userdata)->serviceGone(name);
    return 0;
}

} // namespace

// ---- the registrar

AppMenuRegistrar::AppMenuRegistrar()
{
    sd_bus *bus = DBus::session();
    if (!bus)
        return;
    sd_bus_add_object_vtable(bus, &m_object, RegistrarPath, RegistrarService, registrarVtable, this);
    sd_bus_match_signal(bus, &m_match, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                        onNameOwnerChanged, this);
    m_registered = sd_bus_request_name(bus, RegistrarService, 0) >= 0;
    if (!m_registered)
        std::fprintf(stderr, "SystemUIServer: %s is taken (another global menu runs): the menu bar shows the common menus\n",
                     RegistrarService);
    DBus::flush(bus);
}

AppMenuRegistrar::~AppMenuRegistrar()
{
    sd_bus_slot_unref(m_object);
    sd_bus_slot_unref(m_match);
}

void AppMenuRegistrar::registerWindow(uint32_t window, const std::string &service, const std::string &path)
{
    menus[window] = {service, path};
    if (changed)
        changed(window);
}

void AppMenuRegistrar::unregisterWindow(uint32_t window)
{
    if (menus.erase(window) && changed)
        changed(window);
}

void AppMenuRegistrar::serviceGone(const std::string &service)
{
    for (auto it = menus.begin(); it != menus.end();) {
        if (it->second.first == service) {
            const uint32_t w = it->first;
            it = menus.erase(it);
            if (changed)
                changed(w);
        } else {
            ++it;
        }
    }
}

std::pair<std::string, std::string> AppMenuRegistrar::menuFor(xcb_window_t window) const
{
    auto it = menus.find(window);
    if (it != menus.end())
        return it->second;
    // KDE's way: X properties on the window
    std::string service, path;
    Windows::appMenu(window, &service, &path);
    if (!service.empty() && !path.empty())
        return {service, path};
    return {};
}

// ---- a window's menus

std::string dbusMenuLabel(const std::string &label)
{
    std::string out;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '_') {
            if (i + 1 < label.size() && label[i + 1] == '_')
                out += '_', ++i;
            continue;
        }
        out += label[i];
    }
    return out;
}

namespace {

int onLayoutSignal(sd_bus_message *, void *userdata, sd_bus_error *)
{
    static_cast<std::function<void()> *>(userdata)->operator()();
    return 0;
}

// a{sv}'s values: strings, booleans, integers; the shortcut (aas) as the Mac writes it
int readProperties(sd_bus_message *m, DBusMenu::Item &item)
{
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0)
        return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key = nullptr;
        sd_bus_message_read(m, "s", &key);
        const std::string k = key ? key : "";
        char type = 0;
        const char *contents = nullptr;
        sd_bus_message_peek_type(m, &type, &contents);
        const std::string c = contents ? contents : "";
        if (c == "s") {
            const char *v = nullptr;
            sd_bus_message_read(m, "v", "s", &v);
            item.text[k] = v ? v : "";
        } else if (c == "b") {
            int v = 0;
            sd_bus_message_read(m, "v", "b", &v);
            item.numbers[k] = v;
        } else if (c == "i") {
            int32_t v = 0;
            sd_bus_message_read(m, "v", "i", &v);
            item.numbers[k] = v;
        } else if (c == "aas" && k == "shortcut") {
            sd_bus_message_enter_container(m, 'v', "aas");
            sd_bus_message_enter_container(m, 'a', "as");
            bool first = true;
            while (sd_bus_message_enter_container(m, 'a', "s") > 0) {
                std::string mods, key2;
                const char *s = nullptr;
                while (sd_bus_message_read(m, "s", &s) > 0) {
                    const std::string p = s;
                    if (!first)
                        continue;
                    if (p == "Control")
                        mods += "⌃";
                    else if (p == "Alt")
                        mods += "⌥";
                    else if (p == "Shift")
                        mods += "⇧";
                    else if (p == "Super")
                        mods += "⌘";
                    else
                        key2 = p.size() == 1 ? std::string(1, char(std::toupper((unsigned char)p[0]))) : p;
                }
                sd_bus_message_exit_container(m);
                if (first)
                    item.shortcut = mods + key2;
                first = false;
            }
            sd_bus_message_exit_container(m);
            sd_bus_message_exit_container(m);
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    return sd_bus_message_exit_container(m);
}

// (ia{sv}av)
int readItem(sd_bus_message *m, DBusMenu::Item &item)
{
    int r = sd_bus_message_enter_container(m, 'r', "ia{sv}av");
    if (r <= 0)
        return r < 0 ? r : -EINVAL;
    sd_bus_message_read(m, "i", &item.id);
    readProperties(m, item);
    sd_bus_message_enter_container(m, 'a', "v");
    while (sd_bus_message_enter_container(m, 'v', "(ia{sv}av)") > 0) {
        DBusMenu::Item child;
        readItem(m, child);
        item.children.push_back(std::move(child));
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    return sd_bus_message_exit_container(m);
}

bool shown(const DBusMenu::Item &i)
{
    auto it = i.numbers.find("visible");
    return it == i.numbers.end() || it->second;
}

const DBusMenu::Item *find(const DBusMenu::Item &root, int id)
{
    if (root.id == id)
        return &root;
    for (const DBusMenu::Item &c : root.children)
        if (const DBusMenu::Item *f = find(c, id))
            return f;
    return nullptr;
}

} // namespace

DBusMenu::DBusMenu(std::string service, std::string path) : m_service(std::move(service)), m_path(std::move(path))
{
    layoutChanged = nullptr;
    sd_bus *bus = DBus::session();
    if (!bus)
        return;
    // whatever changes, the layout again (they come in bursts: coalesced)
    static auto refetch = [](void *self) {
        auto *d = static_cast<DBusMenu *>(self);
        if (d->m_refetch)
            return;
        d->m_refetch = AK::RunLoop::main().addTimer(0.05, false, [d] {
            d->m_refetch = 0;
            d->fetch();
            if (d->layoutChanged)
                d->layoutChanged();
        });
    };
    auto *cb = new std::function<void()>([this] { refetch(this); });
    sd_bus_match_signal(bus, &m_layoutSlot, m_service.c_str(), m_path.c_str(), MenuInterface, "LayoutUpdated", onLayoutSignal, cb);
    sd_bus_match_signal(bus, &m_propsSlot, m_service.c_str(), m_path.c_str(), MenuInterface, "ItemsPropertiesUpdated", onLayoutSignal, cb);
    sd_bus_slot_set_destroy_callback(m_layoutSlot, [](void *p) { delete static_cast<std::function<void()> *>(p); });
    fetch();
}

DBusMenu::~DBusMenu()
{
    if (m_refetch)
        AK::RunLoop::main().removeTimer(m_refetch);
    sd_bus_slot_unref(m_propsSlot);
    sd_bus_slot_unref(m_layoutSlot);
}

void DBusMenu::fetch()
{
    sd_bus *bus = DBus::session();
    if (!bus)
        return;
    sd_bus_message *call = nullptr, *reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    if (sd_bus_message_new_method_call(bus, &call, m_service.c_str(), m_path.c_str(), MenuInterface, "GetLayout") < 0)
        return;
    sd_bus_message_append(call, "ii", 0, -1);
    sd_bus_message_append_strv(call, nullptr);
    if (sd_bus_call(bus, call, 400000, &error, &reply) >= 0) {
        uint32_t revision = 0;
        sd_bus_message_read(reply, "u", &revision);
        Item root;
        if (readItem(reply, root) >= 0) {
            m_root = std::move(root);
            m_ready = true;
        }
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(call);
}

std::vector<std::pair<int, std::string>> DBusMenu::topLevel() const
{
    std::vector<std::pair<int, std::string>> out;
    for (const Item &i : m_root.children) {
        auto t = i.text.find("type");
        if (shown(i) && (t == i.text.end() || t->second != "separator"))
            out.emplace_back(i.id, dbusMenuLabel(i.text.count("label") ? i.text.at("label") : ""));
    }
    return out;
}

std::vector<AK::MenuItem> DBusMenu::menu(int id)
{
    // the application may fill the menu now (AboutToShow), then the layout again
    if (sd_bus *bus = DBus::session()) {
        sd_bus_message *reply = nullptr;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_call_method(bus, m_service.c_str(), m_path.c_str(), MenuInterface, "AboutToShow", &error, &reply, "i", id);
        sd_bus_error_free(&error);
        sd_bus_message_unref(reply);
        fetch();
    }
    const Item *i = find(m_root, id);
    return i ? items(*i) : std::vector<AK::MenuItem>();
}

std::vector<AK::MenuItem> DBusMenu::items(const Item &parent)
{
    std::vector<AK::MenuItem> out;
    for (const Item &i : parent.children) {
        if (!shown(i))
            continue;
        auto type = i.text.find("type");
        if (type != i.text.end() && type->second == "separator") {
            out.push_back(AK::MenuItem::line());
            continue;
        }
        auto en = i.numbers.find("enabled");
        AK::MenuItem item(dbusMenuLabel(i.text.count("label") ? i.text.at("label") : ""), false, en == i.numbers.end() || en->second);
        auto disp = i.text.find("children-display");
        if (!i.children.empty() || (disp != i.text.end() && disp->second == "submenu")) {
            item.submenu = items(i);
            if (item.submenu.empty())
                item.submenu.push_back(AK::MenuItem("", false, false));
        } else {
            item.keyEquivalent = i.shortcut;
            auto toggle = i.text.find("toggle-type");
            auto state = i.numbers.find("toggle-state");
            item.checked = toggle != i.text.end() && (toggle->second == "checkmark" || toggle->second == "radio") &&
                           state != i.numbers.end() && state->second == 1;
            const int id = i.id;
            item.action = [this, id] { activate(id); };
        }
        out.push_back(std::move(item));
    }
    return out;
}

void DBusMenu::activate(int id)
{
    sd_bus *bus = DBus::session();
    if (!bus)
        return;
    sd_bus_message *call = nullptr;
    if (sd_bus_message_new_method_call(bus, &call, m_service.c_str(), m_path.c_str(), MenuInterface, "Event") < 0)
        return;
    sd_bus_message_append(call, "isvu", id, "clicked", "s", "", 0u);
    sd_bus_call_async(bus, nullptr, call, nullptr, nullptr, 0);
    sd_bus_message_unref(call);
    DBus::flush(bus);
}
