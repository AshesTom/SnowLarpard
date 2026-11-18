// SPDX-License-Identifier: LGPL-2.1-or-later
// The menu bar, SystemUIServer's on Linux. On the Mac each application draws its own (HIToolbox,
// AppKit) and SystemUIServer its status items at the right; Linux's applications have none, so here
// SystemUIServer draws the whole bar: the Apple menu, the active application's name and menus (the
// Finder's MenuBar.nib, an AquaKit application's MainMenu.nib over its socket, a Linux application's
// over D-Bus, else the common ones), and NSMenuToolbar's extras at the right (Spotlight at the edge).
// The bar is the desktop under it through the window server's filter (menubarfilter.h), or CoreUI's
// art without AppleEnableMenuBarTransparency.
#pragma once

#include "dbusmenu.h"
#include "launchservices.h"
#include "menutoolbar.h"

#include <memory>
#include <string>
#include <vector>
#include <xcb/xcb.h>

class MenuBar {
public:
    MenuBar();
    ~MenuBar();
    MenuToolbar *toolbar() const { return m_toolbar; }
    AK::Window *window() const { return m_window.get(); }
    void show();
    static constexpr double Height = 22;

private:
    struct Title {
        std::string text;
        bool bold = false, apple = false;
        AK::Rect rect;
        std::function<std::vector<AK::MenuItem>()> items;   // made when it opens
    };
    class BarView;
    void activeChanged();
    void rebuild();
    void layout();
    void reloadDesktop();
    void open(int title);
    void openExtra(SystemUI::MenuExtra *extra, const std::vector<AK::MenuItem> &items, std::function<void()> closed);
    void menuClosed();
    bool track(AK::Event &e);

    std::vector<AK::MenuItem> appleMenu();
    std::vector<AK::MenuItem> appMenu(const std::string &name, bool finder);
    std::vector<AK::MenuItem> windowMenu();
    std::vector<AK::MenuItem> recentItems();

    std::unique_ptr<AK::Window> m_window;
    BarView *m_view = nullptr;
    MenuToolbar *m_toolbar = nullptr;
    std::vector<Title> m_titles;
    int m_open = -1;
    SystemUI::MenuExtra *m_openExtra = nullptr;
    AK::Image m_background;
    bool m_transparent = true, m_generatedApple = false;
    long long m_prefsStamp = -1;

    LaunchServices::App m_app;
    xcb_window_t m_active = 0;
    bool m_desktopActive = false;
    AppMenuRegistrar m_registrar;
    std::unique_ptr<DBusMenu> m_imported;
    std::string m_menuKey;
    int m_aquaKitPid = 0;
    uint32_t m_frontSerial = 0;
    xcb_window_t m_frontOver = 0;
    bool m_rebuildPending = false;
};
