// SPDX-License-Identifier: LGPL-2.1-or-later
#include "menubar.h"

#include "aquakitmenus.h"
#include "coreui.h"
#include "desktop.h"
#include "findermenus.h"
#include "foundation.h"
#include "menubarfilter.h"
#include "session.h"
#include "text.h"
#include "windows.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <fstream>
#include <regex>

using namespace AK;
using LSApp = LaunchServices::App;

namespace {

const char *const DockBundle = "/System/Library/CoreServices/Dock.app";

Font titleFont(bool bold) { return bold ? Font::boldSystem(14) : Font::system(14); }

std::string dockTr(const char *key, const char *fallback) { return Assets::tr(DockBundle, "DockMenus", key, fallback); }

MenuItem item(const std::string &title, std::function<void()> action, bool enabled = true, const std::string &keys = {})
{
    MenuItem i(title, false, enabled);
    i.action = std::move(action);
    i.keyEquivalent = keys;
    return i;
}

MenuItem disabled(const std::string &title, const std::string &keys = {})
{
    MenuItem i(title, false, false);
    i.keyEquivalent = keys;
    return i;
}

// _SNOWLEOPARD_FRONT_APP: the application in front without a window, and its serial
uint32_t frontWithoutWindows(uint32_t *serial)
{
    const auto v = Windows::cardinals(Windows::root(), Windows::atom("_SNOWLEOPARD_FRONT_APP"), XCB_ATOM_CARDINAL);
    if (v.size() < 2 || kill(pid_t(v[0]), 0) != 0)
        return 0;
    *serial = v[1];
    return v[0];
}

long long prefsStamp()
{
    return modified(configDir() + "/desktop.conf") * 3 + modified(configDir() + "/menubar.conf");
}

} // namespace

// The bar's own drawing: the background and the titles; the toolbar is a subview
class MenuBar::BarView : public View {
public:
    explicit BarView(MenuBar *bar) : bar(bar) {}
    MenuBar *bar;
    void draw(cairo_t *cr) override
    {
        const double w = frame().w, h = MenuBar::Height;
        if (!bar->m_background.isNull()) {
            bar->m_background.draw(cr, Point{0, 0});
        } else {
            // CUIRenderer::DrawMenuBar: shared art 20102 stretched over a backing white to mid-height,
            // then grey to 150; the edge 75 (MenuBar's measurements on 10.6)
            static const Image art = CoreUI::shared(20102);
            cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, art.isNull() ? h : art.height());
            cairo_pattern_add_color_stop_rgb(g, 0, 1, 1, 1);
            cairo_pattern_add_color_stop_rgb(g, 0.5, 1, 1, 1);
            cairo_pattern_add_color_stop_rgb(g, 0.6, 233 / 255.0, 233 / 255.0, 233 / 255.0);
            cairo_pattern_add_color_stop_rgb(g, 1, 150 / 255.0, 150 / 255.0, 150 / 255.0);
            cairo_rectangle(cr, 0, 0, w, h);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
            if (!art.isNull())
                art.draw(cr, Rect{0, 0, w, double(art.height())});
            fill(cr, Rect{0, h - 1, w, 1}, Color::gray(75));
        }
        for (size_t i = 0; i < bar->m_titles.size(); ++i) {
            const Title &t = bar->m_titles[i];
            if (t.rect.empty())
                continue;
            const bool open = int(i) == bar->m_open;
            const Rect r{t.rect.x, 0, t.rect.w, h - 1};
            if (open)
                SystemUI::MenuExtraView::drawHighlight(cr, r);
            if (t.apple) {
                // over the translucent bar the window server draws the Apple, unless open
                if (!open && bar->m_transparent && bar->m_generatedApple)
                    continue;
                // CUIRenderer::DrawMenuTitle, kind 0xCF: art 20061 (20062 open), centred, one pixel right
                const Image apple = CoreUI::shared(open ? 20062 : 20061);
                if (!apple.isNull()) {
                    apple.draw(cr, Point{std::floor(r.x + r.w / 2 + 1 - apple.width() / 2.0), std::floor((h - 1 - apple.height()) / 2)});
                    continue;
                }
            }
            TextStyle s;
            s.font = titleFont(t.bold);
            s.align = Align::Center;
            s.elide = false;
            // the engraved white under the text, as on the Mac
            if (!open) {
                s.color = Color::gray(255, 140 / 255.0);
                drawText(cr, t.apple ? "\xEF\xA3\xBF" : t.text, r.translated(0, 1), s);
            }
            s.color = open ? Color::gray(255) : Color::gray(0);
            drawText(cr, t.apple ? "\xEF\xA3\xBF" : t.text, r, s);
        }
    }
    void mouseDown(Event &e) override
    {
        for (size_t i = 0; i < bar->m_titles.size(); ++i)
            if (bar->m_titles[i].rect.contains(e.location)) {
                bar->open(int(i));
                return;
            }
    }
};

MenuBar::MenuBar()
{
    const Rect screen = AK::App::shared().screenFrame();
    m_window = std::make_unique<Window>(Size{screen.w, Height}, "SystemUIServer", Window::Status);
    m_view = m_window->contentView()->add<BarView>(this);
    m_view->setFrame(Rect{0, 0, screen.w, Height});
    m_toolbar = m_view->add<MenuToolbar>();
    m_toolbar->openMenu = [this](SystemUI::MenuExtra *e, const std::vector<MenuItem> &items, std::function<void()> closed) {
        openExtra(e, items, std::move(closed));
    };
    m_toolbar->resized = [this] { layout(); };
    reloadDesktop();
    m_prefsStamp = prefsStamp();
    // Desktop & Screen Saver changes the picture, the translucency: the bar follows the files
    AK::App::shared().addTimer(2, true, [this] {
        const long long s = prefsStamp();
        if (s != m_prefsStamp) {
            m_prefsStamp = s;
            reloadDesktop();
        }
    });
    Windows::onChange([this] { activeChanged(); }, {"_SNOWLEOPARD_FRONT_APP"});
    m_registrar.changed = [this](uint32_t window) {
        if (window == m_active) {
            m_menuKey.clear();
            m_app = LSApp{};
            activeChanged();
        }
    };
}

MenuBar::~MenuBar() = default;

void MenuBar::show()
{
    m_window->setStatusBar(int(Height));
    m_window->show();
    m_window->moveTo(Point{AK::App::shared().screenFrame().x, AK::App::shared().screenFrame().y});
    activeChanged();
    rebuild();
}

void MenuBar::reloadDesktop()
{
    m_transparent = Settings("menubar").boolean("AppleEnableMenuBarTransparency", true);
    m_background = {};
    m_generatedApple = false;
    if (m_transparent) {
        // what is behind the bar is always the desktop picture (windows stay under it)
        const Rect screen = AK::App::shared().screenFrame();
        const Image desktop = Desktop::render(Desktop::load(), Size{screen.w, screen.h});
        if (!desktop.isNull()) {
            const Image behind = desktop.copy(Rect{0, 0, screen.w, Height});
            m_background = generateMenuBar(behind);
            m_generatedApple = !m_background.isNull() && drawMenuApple(m_background, behind);
        }
    }
    m_view->setNeedsDisplay();
}

void MenuBar::activeChanged()
{
    xcb_window_t active = Windows::activeWindow();
    // a non-activating panel (About This Mac, its Log Out panel) leaves the bar to the application in front
    const bool panel = Windows::hasState(active, "_NET_WM_STATE_SKIP_TASKBAR") &&
                       (Windows::hasType(active, "_NET_WM_WINDOW_TYPE_UTILITY") || Windows::hasType(active, "_NET_WM_WINDOW_TYPE_DIALOG"));
    if (active && panel && m_app.valid())
        return;
    // a UI element's window (Help Viewer's) leaves it too: such a program has no menus of its own
    if (active && m_app.valid() && LaunchServices::isUIElement(Windows::pid(active)))
        return;
    // the desktop is the Finder's
    bool desktop = Windows::hasType(active, "_NET_WM_WINDOW_TYPE_DESKTOP");
    LSApp app = active && !desktop ? Session::appForWindow(active) : LaunchServices::finder();
    // an application in front without a window (Preview launched alone): its menus, until another
    // window becomes active
    uint32_t serial = 0;
    const uint32_t frontPid = frontWithoutWindows(&serial);
    bool windowless = false;
    if (frontPid && serial != m_frontSerial) {
        m_frontSerial = serial;
        m_frontOver = active;
    }
    if (frontPid && (active == m_frontOver || !active || desktop) && Windows::pid(active) != frontPid) {
        const LSApp front = LaunchServices::forPid(frontPid);
        if (front.valid()) {
            app = front;
            active = 0;
            desktop = false;
            windowless = true;
        }
    }
    m_active = active;
    const bool desktopChanged = (desktop || !active) != m_desktopActive;
    m_desktopActive = desktop || !active;
    // the window's own menus, when it publishes them; or the shell's AquaKit applications'
    auto menu = active && !desktop ? m_registrar.menuFor(active) : std::pair<std::string, std::string>();
    std::string key = menu.first.empty() ? std::string() : menu.first + menu.second;
    int aquaKitPid = 0;
    if (windowless) {
        aquaKitPid = int(frontPid);
        key = "aquakit:" + std::to_string(frontPid);
    } else if (key.empty() && active && !desktop) {
        const int pid = int(Windows::pid(active));
        if (pid == m_aquaKitPid || AquaKitMenus::available(pid)) {
            aquaKitPid = pid;
            key = "aquakit:" + std::to_string(pid);
        }
    }
    if (app.id == m_app.id && key == m_menuKey && !desktopChanged)
        return;
    if (m_open >= 0 || m_openExtra) {
        // the menus' items know their sources: changed once the menu closes
        m_rebuildPending = true;
        return;
    }
    m_aquaKitPid = aquaKitPid;
    m_app = app;
    if (key != m_menuKey) {
        m_menuKey = key;
        m_imported.reset();
        if (!key.empty() && !aquaKitPid) {
            m_imported = std::make_unique<DBusMenu>(menu.first, menu.second);
            m_imported->layoutChanged = [this] {
                if (m_open < 0 && !m_openExtra)
                    rebuild();
                else
                    m_rebuildPending = true;
            };
        }
    }
    rebuild();
}

void MenuBar::rebuild()
{
    m_titles.clear();
    Title apple;
    apple.apple = true;
    apple.items = [this] { return appleMenu(); };
    m_titles.push_back(apple);
    const bool finder = m_app.id == "finder" || !m_app.valid();
    const std::string name = finder ? "Finder" : m_app.name;
    auto add = [this](const std::string &text, std::function<std::vector<MenuItem>()> items, bool bold = false) {
        Title t;
        t.text = text;
        t.bold = bold;
        t.items = std::move(items);
        m_titles.push_back(t);
    };
    // an AquaKit application's own menus (its MainMenu.nib), its application menu first
    if (m_aquaKitPid > 0 && !finder) {
        const int pid = m_aquaKitPid;
        const LSApp app = m_app;
        const std::vector<AquaKitMenus::Top> tops = AquaKitMenus::topLevel(pid);
        for (size_t i = 0; i < tops.size(); ++i) {
            const std::string path = tops[i].path;
            if (tops[i].windowMenu)
                add(tops[i].title, [this] { return windowMenu(); });
            else
                add(i == 0 ? name : tops[i].title, [pid, path, app] { return AquaKitMenus::menu(pid, path, app); }, i == 0);
        }
        if (!tops.empty()) {
            layout();
            return;
        }
    }
    add(name, [this, name, finder] { return appMenu(name, finder); }, true);
    if (m_imported && m_imported->ready() && !m_imported->topLevel().empty()) {
        // a Linux application's own menus after its name, instead of the common ones
        DBusMenu *menus = m_imported.get();
        for (const auto &[id, title] : m_imported->topLevel())
            add(title, [menus, id = id] { return menus->menu(id); });
        layout();
        return;
    }
    if (finder) {
        const bool desktop = m_desktopActive;
        for (const std::string &t : FinderMenus::titles())
            add(t, [t, desktop] { return FinderMenus::menu(t, desktop); });
    } else {
        const xcb_window_t active = m_active;
        add("File", [active] { return std::vector<MenuItem>{item("Close Window", [active] { Windows::close(active); }, active != 0, "⌘W")}; });
        add("Edit", [] {
            // the applications' own menus come with the global menu
            return std::vector<MenuItem>{disabled("Undo", "⌘Z"), disabled("Redo", "⇧⌘Z"), MenuItem::line(), disabled("Cut", "⌘X"),
                                         disabled("Copy", "⌘C"), disabled("Paste", "⌘V"), disabled("Select All", "⌘A")};
        });
        add("View", [] { return std::vector<MenuItem>{disabled("Enter Full Screen")}; });
    }
    add("Window", [this] { return windowMenu(); });
    // -[NSApplication showHelp:]: the Mac bundle's help book, else its anchor in Mac Help (the Finder's
    // mchlp001: Mac Help's home), in Help Viewer; none for a Linux program
    const std::string bundle = finder ? std::string("/System/Library/CoreServices/Finder.app") : m_app.macBundle;
    add("Help", [finder, name, bundle] {
        const std::string title = (finder ? std::string("Mac") : name) + " Help";
        const std::vector<std::string> args = helpArguments(bundle);
        if (args.empty())
            return std::vector<MenuItem>{disabled(title)};
        return std::vector<MenuItem>{item(title, [args] { Task::launchDetached(args); }, true, "⌘?")};
    });
    layout();
}

std::vector<std::string> MenuBar::helpArguments(const std::string &bundle)
{
    const std::string viewer = LaunchServices::appsDir() + "/HelpViewer.app/Contents/MacOS/HelpViewer";
    if (bundle.empty() || !isFile(viewer))
        return {};
    const auto info = Assets::plistStrings(bundle + "/Contents/Info.plist");
    auto get = [&info](const char *k) {
        auto it = info.find(k);
        return it == info.end() ? std::string() : it->second;
    };
    if (const std::string book = get("CFBundleHelpBookName"); !book.empty())
        return {viewer, "--book", book};
    if (const std::string anchor = get("CFAppleHelpAnchor"); !anchor.empty())
        return {viewer, "--anchor", anchor};
    return {};
}

void MenuBar::layout()
{
    const double w = m_view->frame().w;
    m_toolbar->setFrame(Rect{w - m_toolbar->width(), 0, m_toolbar->width(), Height});
    double x = 10;
    for (Title &t : m_titles) {
        const double tw = t.apple ? 37 : std::ceil(textWidth(t.text, titleFont(t.bold))) + 20;
        // the titles that would run under the extras are left out, as the Mac hides them
        t.rect = x + tw <= m_toolbar->frame().x ? Rect{x, 0, tw, Height} : Rect{};
        x += tw;
    }
    m_view->setNeedsDisplay();
}

void MenuBar::open(int index)
{
    if (index < 0 || size_t(index) >= m_titles.size() || m_titles[size_t(index)].rect.empty())
        return;
    m_open = index;
    m_view->setNeedsDisplay();
    const Title &t = m_titles[size_t(index)];
    std::vector<MenuItem> items = t.items ? t.items() : std::vector<MenuItem>();
    if (items.empty()) {
        m_open = -1;
        m_view->setNeedsDisplay();
        return;
    }
    Menu::trackOutside = [this](Event &e) { return track(e); };
    Menu::popUp(m_window.get(), Point{t.rect.x, Height - 1}, items, 0, Font::system(14), [this](int) { menuClosed(); });
}

void MenuBar::openExtra(SystemUI::MenuExtra *extra, const std::vector<MenuItem> &items, std::function<void()> closed)
{
    m_openExtra = extra;
    const Rect place = m_toolbar->placeOf(extra);
    Menu::trackOutside = [this](Event &e) { return track(e); };
    Menu::popUp(m_window.get(), Point{m_toolbar->frame().x + place.x, Height - 1}, items, 0, Font::system(14),
                [this, closed](int) {
                    closed();
                    menuClosed();
                });
}

void MenuBar::menuClosed()
{
    m_open = -1;
    m_openExtra = nullptr;
    m_view->setNeedsDisplay();
    if (m_rebuildPending) {
        m_rebuildPending = false;
        AK::App::shared().later([this] {
            m_menuKey += "*";   // looked at anew
            activeChanged();
        });
    }
}

bool MenuBar::track(Event &e)
{
    // the pointer over the bar with a menu open: the menu under it opens instead, as on the Mac
    const Point p = e.screen;
    if (p.y < 0 || p.y >= Height)
        return false;
    int title = -1;
    for (size_t i = 0; i < m_titles.size(); ++i)
        if (m_titles[i].rect.contains(p))
            title = int(i);
    SystemUI::MenuExtra *extra = nullptr;
    if (title < 0) {
        const Rect tb = m_toolbar->frame();
        if (tb.contains(p))
            extra = m_toolbar->extraAt(Point{p.x - tb.x, p.y});
    }
    const bool current = (title >= 0 && title == m_open) || (extra && extra == m_openExtra);
    if (current) {
        if (e.type == Event::MouseDown) {
            Menu::close();   // a click on the open title closes it
            return true;
        }
        return true;
    }
    if (title < 0 && (!extra || !extra->slidesFromMenus()))
        return e.type != Event::MouseDown ? false : false;
    Menu::close();
    AK::App::shared().later([this, title, extra] {
        if (title >= 0) {
            open(title);
        } else {
            extra->setModifierFlags(0);
            extra->popUpMenu();
        }
    });
    return true;
}

// ---- the Apple menu

std::vector<MenuItem> MenuBar::appleMenu()
{
    std::vector<MenuItem> m;
    m.push_back(item("About This Mac", [] { Session::aboutThisMac(); }));
    // 10.6's Software Update (Apple's catalog); without it, Linux's
    const LSApp update = LaunchServices::forMacBundle("/System/Library/CoreServices/Software Update.app");
    std::vector<LSApp> linuxUpdaters;
    for (const char *id : {"org.kde.discover", "org.gnome.Software", "pamac-manager", "octopi"})
        if (LSApp a = LaunchServices::byId(id); a.valid())
            linuxUpdaters.push_back(a);
    m.push_back(item("Software Update…", [update, linuxUpdaters] {
        if (LaunchServices::launch(update))
            return;
        for (const LSApp &a : linuxUpdaters)
            if (LaunchServices::launch(a))
                return;
    }, update.valid() || !linuxUpdaters.empty()));
    m.push_back(MenuItem::line());
    const LSApp prefs = LaunchServices::forMacBundle("/Applications/System Preferences.app");
    m.push_back(item("System Preferences…", [prefs] { LaunchServices::launch(prefs); }, prefs.valid()));
    // Dock ▸, with the Dock's own words
    Settings dock("dock");
    const bool hiding = dock.boolean("autohide", false), mag = dock.boolean("magnification", false);
    MenuItem dockItem("Dock");
    dockItem.submenu.push_back(item(hiding ? dockTr("TURN_HIDING_OFF", "Turn Hiding Off") : dockTr("TURN_HIDING_ON", "Turn Hiding On"),
                                    [hiding] { Settings("dock").set("autohide", !hiding); }));
    dockItem.submenu.push_back(item(mag ? dockTr("TURN_MAG_OFF", "Turn Magnification Off") : dockTr("TURN_MAG_ON", "Turn Magnification On"),
                                    [mag] { Settings("dock").set("magnification", !mag); }));
    dockItem.submenu.push_back(MenuItem::line());
    const std::string orientation = dock.string("orientation", "bottom");
    for (const auto &[key, fallback, value] : {std::tuple{"LEFT", "Position on Left", "left"}, std::tuple{"BOTTOM", "Position at Bottom", "bottom"},
                                               std::tuple{"RIGHT", "Position on Right", "right"}}) {
        MenuItem pos(dockTr(key, fallback), false, std::string(value) == "bottom", orientation == value);   // the sides are for later
        dockItem.submenu.push_back(pos);
    }
    dockItem.submenu.push_back(MenuItem::line());
    dockItem.submenu.push_back(disabled(dockTr("DOCK_PREFS", "Dock Preferences…")));
    m.push_back(dockItem);
    m.push_back(MenuItem::line());
    MenuItem recent("Recent Items");
    recent.submenu = recentItems();
    m.push_back(recent);
    m.push_back(MenuItem::line());
    m.push_back(item("Force Quit…", [] { Session::forceQuit(); }, true, "⌥⌘⎋"));
    m.push_back(MenuItem::line());
    m.push_back(item("Sleep", [] { Session::sleep(); }));
    m.push_back(item("Restart…", [] { Session::restart(); }));
    m.push_back(item("Shut Down…", [] { Session::shutDown(); }));
    m.push_back(MenuItem::line());
    m.push_back(item("Log Out " + Session::userFullName() + "…", [] { Session::logOut(true); }, true, "⇧⌘Q"));
    return m;
}

std::vector<MenuItem> MenuBar::recentItems()
{
    // the freedesktop list of recent files (recently-used.xbel), newest first
    const char *data = std::getenv("XDG_DATA_HOME");
    const std::string file = (data && *data ? std::string(data) : home() + "/.local/share") + "/recently-used.xbel";
    std::ifstream in(file);
    const std::string xml((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::pair<std::string, std::string>> items;   // modified, href
    static const std::regex bookmark(R"re(<bookmark\b[^>]*>)re"), href(R"re(href="([^"]*)")re"), when(R"re(modified="([^"]*)")re");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), bookmark); it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        std::smatch h, m;
        if (std::regex_search(tag, h, href))
            items.emplace_back(std::regex_search(tag, m, when) ? m[1].str() : "", h[1].str());
    }
    std::sort(items.begin(), items.end(), [](auto &a, auto &b) { return a.first > b.first; });
    std::vector<MenuItem> out;
    out.push_back(disabled("Documents"));
    int n = 0;
    for (const auto &[stamp, url] : items) {
        std::string path = url;
        if (path.rfind("file://", 0) == 0) {
            path = path.substr(7);
            // %XX
            std::string decoded;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '%' && i + 2 < path.size()) {
                    decoded += char(std::strtol(path.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else {
                    decoded += path[i];
                }
            }
            path = decoded;
            if (!isFile(path) && !isDir(path))
                continue;
        }
        out.push_back(item(fileName(path), [url] { LaunchServices::open(url); }));
        if (++n == 10)
            break;
    }
    out.push_back(MenuItem::line());
    out.push_back(item("Clear Menu", [file] { std::remove(file.c_str()); }));
    return out;
}

// ---- the application's menus: the Finder's, or the ones every application has

std::vector<MenuItem> MenuBar::appMenu(const std::string &name, bool finder)
{
    const LSApp app = m_app;
    std::vector<MenuItem> m;
    m.push_back(item("About " + name, [] { Session::aboutThisMac(); }, finder));
    m.push_back(MenuItem::line());
    if (finder)
        m.push_back(item("Preferences…", [] { FinderMenus::send("cmdPreferences:"); }, true, "⌘,"));
    else
        m.push_back(disabled("Preferences…", "⌘,"));
    if (finder) {
        m.push_back(MenuItem::line());
        m.push_back(item("Empty Trash…", [] { FinderMenus::send("cmdEmptyTrash:"); }, true, "⇧⌘⌫"));
        if (Settings("finder").boolean("EmptyTrashSecurely", false))
            m.push_back(item("Secure Empty Trash…", [] { FinderMenus::send("cmdEmptyTrashSecure:"); }));
    }
    m.push_back(MenuItem::line());
    MenuItem services("Services", false, false);
    services.submenu.push_back(disabled("No Services Apply"));
    m.push_back(services);
    m.push_back(MenuItem::line());
    m.push_back(item("Hide " + name, [app] { Session::hideApp(app); }, true, "⌘H"));
    m.push_back(item("Hide Others", [app] { Session::hideOthers(app); }, true, "⌥⌘H"));
    m.push_back(item("Show All", [] { Session::showAll(); }));
    if (!finder) {
        m.push_back(MenuItem::line());
        m.push_back(item("Quit " + name, [app] { Session::quitApp(app); }, true, "⌘Q"));
    }
    return m;
}

std::vector<MenuItem> MenuBar::windowMenu()
{
    const LSApp app = m_app;
    const xcb_window_t active = Windows::activeWindow();
    std::vector<MenuItem> m;
    m.push_back(item("Minimize", [active] { Windows::minimize(active); }, active != 0, "⌘M"));
    m.push_back(disabled("Zoom"));
    m.push_back(MenuItem::line());
    m.push_back(item("Bring All to Front", [app] { Session::bringToFront(app); }));
    bool first = true;
    for (xcb_window_t w : Windows::clientWindows()) {
        if (Session::appForWindow(w).id != app.id)
            continue;
        if (first)
            m.push_back(MenuItem::line()), first = false;
        MenuItem i = item(Windows::title(w), [w] { Windows::activate(w); });
        i.checked = w == active;
        m.push_back(i);
    }
    return m;
}
