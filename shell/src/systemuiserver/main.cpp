// SPDX-License-Identifier: LGPL-2.1-or-later
// SystemUIServer.app on AquaKit, as 10.6.8's (docs/reverse-status.md, section 19; docs/systemuiserver-
// port.md): Search.bundle (Spotlight) loaded first, then the extras of the startup list
// (SUISStartupObject), each by its principal class, SystemUIServer's own first (AppleClockExtra,
// BatteryExtra, AppleVolumeExtra), else the bundle's code (a .menu's library, loaded as NSBundle
// would); NSMenuToolbar lays them at the bar's right. On Linux it draws the whole menu bar
// (menubar.h). The session ends with it (Log Out).
#include "menubar.h"
#include "screencapture.h"
#include "spotlight.h"
#include "startup.h"

#include "foundation.h"
#include "launchservices.h"
#include "text.h"
#include "windows.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <dlfcn.h>
#include <map>

using namespace AK;

namespace {

// -[NSBundle load] for a .menu bundle beside SystemUIServer.app: its library registers its classes.
// -[NSBundle principalClass]: the class NSPrincipalClass names, else the bundle's first class (Eject.menu's
// Info.plist names ProcessExtra, its code's class is EjectExtra); its name, empty when none
std::string loadBundleCode(const std::string &bundle)
{
    static const std::string coreServices = "/System/Library/CoreServices/";
    if (bundle.rfind(coreServices, 0) != 0)
        return {};
    const std::string rel = bundle.substr(coreServices.size());
    const std::string library = LaunchServices::appsDir() + "/" + rel + "/Contents/MacOS/" + baseName(bundle);
    if (!isFile(library))
        return {};
    static std::map<std::string, std::string> first;   // loaded once: its first class kept
    if (auto it = first.find(bundle); it != first.end())
        return it->second;
    const size_t before = SystemUI::classCount();
    if (!dlopen(library.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
        std::fprintf(stderr, "SystemUIServer: %s: %s\n", bundle.c_str(), dlerror());
        return {};
    }
    return first[bundle] = SystemUI::classRegisteredAt(before);
}

} // namespace

int main()
{
    setProgramName("SystemUIServer");
    // the clock in the user's formats (International's Formats on the Mac)
    std::setlocale(LC_TIME, "");
    if (!App::shared().open() || !Windows::open()) {
        std::fprintf(stderr, "SystemUIServer: no display\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    MenuBar bar;
    // -_loadMenuBarExtras: each extra of the list by its principal class, if allowed; Spotlight at the
    // edge (-loadMatadorSearch). Again when the list changes (a pane shows or hides an extra).
    std::vector<std::string> loaded;
    SpotlightExtra *spotlight = nullptr;
    auto load = [&] {
        const std::vector<std::string> extras = Startup::menuExtrasForStartup();
        if (extras == loaded)
            return;
        loaded = extras;
        // -writeMenuBarPlugins: the list as the bar has it
        Startup::writeMenuExtras(extras);
        // the extras gone from the list leave; the new ones come at their place; the others stay
        MenuToolbar *toolbar = bar.toolbar();
        for (SystemUI::MenuExtra *e : std::vector<SystemUI::MenuExtra *>(toolbar->items()))
            if (e != spotlight && std::find(extras.begin(), extras.end(), e->bundlePath()) == extras.end())
                toolbar->removeItem(e);
        size_t index = 0;
        for (const std::string &bundle : extras) {
            const auto &items = toolbar->items();
            auto at = std::find_if(items.begin(), items.end(), [&](SystemUI::MenuExtra *e) { return e->bundlePath() == bundle; });
            if (at != items.end()) {
                index = size_t(at - items.begin()) + 1;
                continue;
            }
            const std::string cls = Assets::plistStrings(bundle + "/Contents/Info.plist")["NSPrincipalClass"];
            if (!Startup::canLoadClass(cls))
                continue;
            SystemUI::Factory make = SystemUI::classNamed(cls);
            if (!make) {
                const std::string first = loadBundleCode(bundle);
                make = SystemUI::classNamed(cls);
                if (!make && !first.empty())
                    make = SystemUI::classNamed(first);
            }
            if (make)
                toolbar->insertItem(make(bundle), index++);
        }
        if (!spotlight) {
            spotlight = new SpotlightExtra("/System/Library/CoreServices/Search.bundle");
            toolbar->addItem(spotlight);
        }
    };
    bar.toolbar()->listChanged = [&](const std::vector<std::string> &order, const std::string &removed) {
        // -writeMenuBarPlugins:, and dontAutoLoad for the one removed (Autoload.plist mustn't bring it back)
        Settings prefs("systemuiserver");
        if (!removed.empty()) {
            std::vector<std::string> dont = prefs.list("dontAutoLoad");
            if (std::find(dont.begin(), dont.end(), removed) == dont.end())
                dont.push_back(removed);
            prefs.set("dontAutoLoad", dont);
        }
        Startup::writeMenuExtras(order);
        loaded = Startup::menuExtrasForStartup();
    };
    load();
    bar.toolbar()->fixed = spotlight;
    long long stamp = modified(configDir() + "/systemuiserver.conf");
    App::shared().addTimer(1, true, [&] {
        const long long now = modified(configDir() + "/systemuiserver.conf");
        if (now != stamp && !Menu::isOpen()) {
            stamp = now;
            load();
        }
    });
    bar.show();
    ScreenCapture::install();   // ⇧⌘3, ⇧⌘4 (and with ⌃): screencapture
    return App::shared().run();
}
