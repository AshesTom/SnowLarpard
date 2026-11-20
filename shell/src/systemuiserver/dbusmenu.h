// SPDX-License-Identifier: LGPL-2.1-or-later
// Linux's applications' own menus in the menu bar, over D-Bus (sd-bus), as Linux desktops with a
// global menu do (the Mac's applications draw theirs themselves):
//
//  - the registrar is com.canonical.AppMenu.Registrar: applications (Qt's QDBusMenuBar, GTK's
//    appmenu-gtk-module, KDE's KXmlGui) register which object holds each window's menus; KDE's may
//    name it in X properties instead (_KDE_NET_WM_APPMENU_SERVICE_NAME, ..._OBJECT_PATH);
//  - DBusMenu reads such an object (com.canonical.dbusmenu: GetLayout, AboutToShow, Event,
//    LayoutUpdated) into AppKit's menu items; choosing one sends its "clicked" event back.
#pragma once

#include "controls.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <xcb/xcb.h>

struct sd_bus_slot;

class AppMenuRegistrar {
public:
    AppMenuRegistrar();
    ~AppMenuRegistrar();
    bool registered() const { return m_registered; }
    // the service and object of a window's menus, or empty ones
    std::pair<std::string, std::string> menuFor(xcb_window_t window) const;
    std::function<void(uint32_t window)> changed;

    // the bus' calls
    void registerWindow(uint32_t window, const std::string &service, const std::string &path);
    void unregisterWindow(uint32_t window);
    void serviceGone(const std::string &service);
    std::map<uint32_t, std::pair<std::string, std::string>> menus;

private:
    bool m_registered = false;
    sd_bus_slot *m_object = nullptr, *m_match = nullptr;
};

class DBusMenu {
public:
    DBusMenu(std::string service, std::string path);
    ~DBusMenu();
    struct Item {
        int id = 0;
        std::map<std::string, std::string> text;   // label, type, toggle-type, children-display...
        std::map<std::string, int> numbers;        // toggle-state; enabled and visible (0, 1)
        std::string shortcut;                      // as the Mac writes it: "⇧⌘S"
        std::vector<Item> children;
    };
    bool ready() const { return m_ready; }
    // the menu bar's titles: the root's shown children (id, title)
    std::vector<std::pair<int, std::string>> topLevel() const;
    // a top-level menu's items now (AboutToShow asked, the layout read again)
    std::vector<AK::MenuItem> menu(int id);
    std::function<void()> layoutChanged;

    void fetch();   // GetLayout, now

private:
    std::vector<AK::MenuItem> items(const Item &parent);
    void activate(int id);
    std::string m_service, m_path;
    Item m_root;
    bool m_ready = false;
    sd_bus_slot *m_layoutSlot = nullptr, *m_propsSlot = nullptr;
    int m_refetch = 0;
};

std::string dbusMenuLabel(const std::string &label);   // "_File" -> "File"
