// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 11.4's Store in the window (BWStoreController, StoreWebKitViewController): the pages drawn by
// iTunesStore (storeprocess.cpp), a process of their own, their pictures shown here, the mouse and the
// keyboard handed to it (docs/itunes-reverse.md, section 8).
#pragma once

#include "appkit.h"
#include "runloop.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iTunes {

// One of the store's nav bar menus (addProtocol's plist: navbar ▸ menus, each a title and a URL)
struct StoreMenu {
    std::string title, url;
    bool divider = false;
    std::vector<StoreMenu> items;   // its drop-down's
};

class StoreView : public AK::View {
public:
    StoreView();
    ~StoreView() override;
    static bool available();   // iTunesStore beside iTunes

    void start();   // the process, the bag's store front first
    void load(const std::string &url);
    void command(const std::string &line);   // "back", "forward", "home", "reload"
    void search(const std::string &term);    // the bag's "search" page for the term (FUN_00232dd8)
    bool canGoBack() const { return m_back; }
    bool canGoForward() const { return m_forward; }
    bool loading() const { return m_loading; }

    std::function<void()> onNavigation;   // the history, the loading state
    std::function<void(const std::vector<StoreMenu> &, const std::string &selected)> onMenus;
    std::function<void()> onLibrary;      // iTunes.goToLibrary()

    void setFrame(const AK::Rect &r) override;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    void mouseMoved(AK::Event &e) override;
    void scrollWheel(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }
    void focusChanged() override;
    const char *cursor() const override { return m_cursor.c_str(); }

private:
    void line(const std::string &l);
    void send(const std::string &l);
    void mouse(const char *kind, const AK::Event &e);
    void protocol(const std::string &xml);

    std::unique_ptr<AK::Task> m_task;
    std::string m_frameFile, m_cursor = "default";
    AK::Image m_frame;
    bool m_back = false, m_forward = false, m_loading = false;
    std::vector<StoreMenu> m_menus;
    std::string m_selected;
};

} // namespace iTunes
