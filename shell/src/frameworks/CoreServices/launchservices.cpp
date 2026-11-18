// SPDX-License-Identifier: LGPL-2.1-or-later
#include "launchservices.h"

#include "foundation.h"
#include "runloop.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace LaunchServices {

namespace {

struct MacApp {
    const char *bundle;
    const char *name;
    std::vector<std::string> candidates;
};
const std::vector<MacApp> &macApps()
{
    static const std::vector<MacApp> table = {
        {"/Applications/Safari.app", "Safari",
         {"firefox", "org.mozilla.firefox", "chromium", "google-chrome", "brave-browser", "org.gnome.Epiphany", "librewolf",
          "vivaldi-stable", "zen", "zen-browser"}},
        {"/Applications/Mail.app", "Mail",
         {"org.mozilla.Thunderbird", "thunderbird", "net.thunderbird.Thunderbird", "org.kde.kmail2", "org.gnome.Evolution",
          "org.gnome.Geary", "betterbird"}},
        {"/Applications/iChat.app", "iChat", {"org.telegram.desktop", "signal-desktop", "discord", "im.dino.Dino", "pidgin"}},
        {"/Applications/Address Book.app", "Address Book", {"org.gnome.Contacts", "org.kde.kaddressbook"}},
        {"/Applications/iCal.app", "iCal", {"org.gnome.Calendar", "org.kde.korganizer", "org.kde.merkuro.calendar"}},
        {"/Applications/Preview.app", "Preview",
         {"org.kde.okular", "okularApplication_pdf", "org.gnome.Evince", "org.gnome.Papers", "org.kde.gwenview", "org.gnome.Loupe",
          "eog", "org.gnome.eog"}},
        {"/Applications/iTunes.app", "iTunes",
         {"org.kde.elisa", "org.gnome.Rhythmbox3", "rhythmbox", "org.strawberrymusicplayer.strawberry", "audacious", "org.gnome.Lollypop"}},
        {"/Applications/Photo Booth.app", "Photo Booth", {"org.gnome.Cheese", "org.kde.kamoso", "org.gnome.Snapshot"}},
        {"/Applications/QuickTime Player.app", "QuickTime Player", {"mpv", "vlc", "org.kde.haruna", "org.gnome.Totem", "io.mpv.Mpv"}},
        {"/Applications/DVD Player.app", "DVD Player", {"vlc"}},
        {"/Applications/TextEdit.app", "TextEdit", {"org.kde.kwrite", "org.gnome.TextEditor", "org.xfce.mousepad", "gedit"}},
        {"/Applications/Calculator.app", "Calculator", {"org.kde.kcalc", "org.gnome.Calculator", "qalculate-gtk"}},
        {"/Applications/Stickies.app", "Stickies", {"org.kde.knotes", "xpad"}},
        {"/Applications/Dictionary.app", "Dictionary", {"org.gnome.Dictionary"}},
        {"/Applications/Font Book.app", "Font Book", {"org.gnome.font-viewer", "org.kde.kfontview"}},
        {"/Applications/Image Capture.app", "Image Capture", {"org.kde.skanlite", "simple-scan", "org.gnome.SimpleScan"}},
        {"/Applications/Chess.app", "Chess", {"org.kde.knights", "org.gnome.Chess"}},
        {"/Applications/System Preferences.app", "System Preferences",
         {"systemsettings", "org.kde.systemsettings", "org.gnome.Settings", "gnome-control-center", "xfce-settings-manager"}},
        {"/Applications/Utilities/Terminal.app", "Terminal",
         {"org.kde.konsole", "org.gnome.Console", "org.gnome.Terminal", "xfce4-terminal", "Alacritty", "kitty", "xterm"}},
        {"/Applications/Utilities/Activity Monitor.app", "Activity Monitor",
         {"org.kde.plasma-systemmonitor", "org.gnome.SystemMonitor", "gnome-system-monitor", "htop"}},
        {"/Applications/Utilities/Disk Utility.app", "Disk Utility", {"org.kde.partitionmanager", "org.gnome.DiskUtility", "gparted"}},
        {"/Applications/Utilities/Console.app", "Console", {"org.gnome.Logs", "org.kde.kjournaldbrowser"}},
        {"/Applications/Utilities/Grab.app", "Grab", {"org.kde.spectacle", "org.gnome.Screenshot"}},
    };
    return table;
}

const std::vector<std::string> fileManagers = {"org.kde.dolphin", "org.gnome.Nautilus", "thunar", "pcmanfm-qt", "pcmanfm", "nemo", "caja"};
const std::vector<std::string> ownBundles = {"/Applications/System Preferences.app", "/Applications/Preview.app",
                                             "/Applications/Mail.app", "/Applications/Photo Booth.app",
                                             "/Applications/Calculator.app", "/Applications/Stickies.app",
                                             "/Applications/TextEdit.app", "/System/Library/CoreServices/Software Update.app"};

bool same(const std::string &a, const std::string &b) { return !a.empty() && a.size() == b.size() && strcasecmp(a.c_str(), b.c_str()) == 0; }

bool executable(const std::string &p) { return access(p.c_str(), X_OK) == 0 && AK::isFile(p); }

std::string bundleName(const std::string &bundle)
{
    return AK::baseName(bundle);   // "/Applications/Photo Booth.app" -> "Photo Booth"
}

std::vector<std::string> splitExec(const std::string &exec)
{
    std::vector<std::string> args;
    std::string cur;
    bool quoted = false, any = false;
    for (size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (quoted) {
            if (c == '\\' && i + 1 < exec.size())
                cur += exec[++i];
            else if (c == '"')
                quoted = false;
            else
                cur += c;
        } else if (c == '"') {
            quoted = any = true;
        } else if (c == ' ' || c == '\t') {
            if (any || !cur.empty())
                args.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty())
        args.push_back(cur);
    std::vector<std::string> out;
    for (std::string a : args) {
        if (a.size() == 2 && a[0] == '%')
            continue;
        for (size_t p; (p = a.find("%%")) != std::string::npos;)
            a.replace(p, 2, "%");
        out.push_back(a);
    }
    return out;
}

std::vector<std::string> dataDirs()
{
    std::vector<std::string> dirs;
    const char *home = std::getenv("XDG_DATA_HOME");
    dirs.push_back(home && *home ? home : AK::home() + "/.local/share");
    const char *sys = std::getenv("XDG_DATA_DIRS");
    std::string list = sys && *sys ? sys : "/usr/local/share:/usr/share";
    size_t from = 0;
    while (from <= list.size()) {
        const size_t colon = list.find(':', from);
        const std::string d = list.substr(from, colon == std::string::npos ? std::string::npos : colon - from);
        if (!d.empty())
            dirs.push_back(d);
        if (colon == std::string::npos)
            break;
        from = colon + 1;
    }
    return dirs;
}

struct Catalog {
    std::vector<App> apps;
    std::map<std::string, int> byId;
};

void scan(const std::string &root, const std::string &dir, Catalog &c)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
        if (e->d_name[0] != '.')
            names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string &n : names) {
        const std::string file = dir + "/" + n;
        if (AK::isDir(file)) {
            scan(root, file, c);
            continue;
        }
        if (n.size() < 9 || n.compare(n.size() - 8, 8, ".desktop") != 0)
            continue;
        std::string id = file.substr(root.size() + 1);
        id.resize(id.size() - 8);
        std::replace(id.begin(), id.end(), '/', '-');
        if (c.byId.count(id))
            continue;
        std::ifstream in(file);
        std::string line;
        bool entry = false;
        std::map<std::string, std::string> keys;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '[') {
                entry = line.rfind("[Desktop Entry]", 0) == 0;
                continue;
            }
            const size_t eq = line.find('=');
            if (!entry || eq == std::string::npos)
                continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && k.back() == ' ')
                k.pop_back();
            while (!v.empty() && v.front() == ' ')
                v.erase(0, 1);
            if (!keys.count(k))
                keys[k] = v;
        }
        if (keys["Type"] != "Application" || keys["Hidden"] == "true") {
            c.byId[id] = -1;   // hidden by the user's own copy
            continue;
        }
        App a;
        a.id = id;
        a.name = keys["Name"];
        a.exec = splitExec(keys["Exec"]);
        a.iconName = keys["Icon"];
        a.shown = keys["NoDisplay"] != "true";
        a.wmClass = keys["StartupWMClass"];
        if (a.exec.empty())
            continue;
        if (a.wmClass.empty())
            a.wmClass = AK::fileName(a.exec.front());
        c.byId[id] = int(c.apps.size());
        c.apps.push_back(a);
    }
}

const Catalog &catalog()
{
    static Catalog c = [] {
        Catalog k;
        for (const std::string &d : dataDirs())   // the user's first: they win
            scan(d + "/applications", d + "/applications", k);
        return k;
    }();
    return c;
}

App ownApp(const std::string &bundle)
{
    const std::string name = bundleName(bundle);
    const std::string exe = appsDir() + "/" + name + ".app/Contents/MacOS/" + name;
    if (!executable(exe))
        return {};
    App a;
    a.id = "mac-" + name;
    a.name = name;
    a.macBundle = bundle;
    a.exec = {exe};
    a.wmClass = name;
    return a;
}

App withMac(App a, const MacApp &mac)
{
    a.name = mac.name;
    a.macBundle = mac.bundle;
    return a;
}

std::string exeOf(uint32_t pid)
{
    char buf[4096];
    const ssize_t n = readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), buf, sizeof buf - 1);
    if (n <= 0)
        return {};
    buf[n] = 0;
    return AK::fileName(buf);
}

// A theme icon's file: the user's theme then hicolor's apps, the largest PNG not bigger than needed
std::string themeIcon(const std::string &name, int size)
{
    if (name.empty())
        return {};
    if (name[0] == '/')
        return AK::isFile(name) ? name : std::string();
    std::string best;
    int bestSize = 0;
    for (const std::string &d : dataDirs())
        for (const char *theme : {"breeze", "Adwaita", "Papirus", "hicolor"})
            for (int s : {16, 22, 24, 32, 48, 64, 128, 256}) {
                const std::string p = d + "/icons/" + theme + "/" + std::to_string(s) + "x" + std::to_string(s) + "/apps/" + name + ".png";
                if ((best.empty() || (bestSize < size && s > bestSize) || (s >= size && s < bestSize)) && AK::isFile(p))
                    best = p, bestSize = s;
            }
    for (const std::string &d : dataDirs())
        if (best.empty() && AK::isFile(d + "/pixmaps/" + name + ".png"))
            best = d + "/pixmaps/" + name + ".png";
    return best;
}

} // namespace

std::string appsDir()
{
    // the program is apps/X or apps/X.app/Contents/MacOS/X (or a bundle's library below them)
    std::string dir = AK::dirName(AK::executablePath());
    for (int i = 0; i < 5; ++i) {
        if (AK::isDir(dir + "/Finder.app") || AK::isDir(dir + "/Dock.app") || AK::isDir(dir + "/SystemUIServer.app"))
            return dir;
        dir = AK::dirName(dir);
    }
    return AK::dirName(AK::executablePath());
}

const std::vector<App> &all() { return catalog().apps; }

App byId(const std::string &id)
{
    auto it = catalog().byId.find(id);
    return it == catalog().byId.end() || it->second < 0 ? App{} : catalog().apps[size_t(it->second)];
}

App forMacBundle(const std::string &bundle)
{
    if (bundle.size() >= 11 && bundle.compare(bundle.size() - 11, 11, "/Finder.app") == 0)
        return finder();
    if (std::find(ownBundles.begin(), ownBundles.end(), bundle) != ownBundles.end()) {
        App own = ownApp(bundle);
        if (own.valid())
            return own;
    }
    for (const MacApp &mac : macApps()) {
        if (bundle != mac.bundle)
            continue;
        for (const std::string &id : mac.candidates) {
            App a = byId(id);
            if (a.valid())
                return withMac(a, mac);
        }
    }
    return {};
}

App finder()
{
    App f;
    f.id = "finder";
    f.name = "Finder";
    f.macBundle = "/System/Library/CoreServices/Finder.app";
    const std::string exe = appsDir() + "/Finder.app/Contents/MacOS/FinderWindows";
    if (executable(exe)) {
        f.exec = {exe};
        f.wmClass = "Finder";
        return f;
    }
    for (const std::string &id : fileManagers) {
        App fm = byId(id);
        if (fm.valid()) {
            f.exec = fm.exec;
            f.wmClass = fm.wmClass;
            f.iconName = fm.iconName;
            break;
        }
    }
    if (f.exec.empty())
        f.exec = {"xdg-open", AK::home()};
    return f;
}

App forWindow(const std::string &wmClass, const std::string &instance, uint32_t pid)
{
    if (same(wmClass, "Finder") || same(instance, "Finder"))
        return finder();
    for (const std::string &bundle : ownBundles) {
        const std::string name = bundleName(bundle);
        if (same(name, wmClass) || same(name, instance)) {
            App own = ownApp(bundle);
            if (own.valid())
                return own;
        }
    }
    App found;
    for (const App &a : all()) {
        const size_t dot = a.id.rfind('.');
        if (same(a.wmClass, wmClass) || same(a.wmClass, instance) || same(a.id, wmClass) || same(a.id, instance) ||
            (dot != std::string::npos && same(a.id.substr(dot + 1), wmClass))) {
            found = a;
            break;
        }
    }
    if (!found.valid() && pid) {
        const std::string exe = exeOf(pid);
        for (const App &a : all())
            if (!exe.empty() && AK::fileName(a.exec.front()) == exe) {
                found = a;
                break;
            }
    }
    if (!found.valid()) {
        found.id = "x-window-" + (wmClass.empty() ? instance : wmClass);
        found.name = wmClass.empty() ? instance : wmClass;
        found.wmClass = wmClass;
        return found;
    }
    App f = finder();
    if (same(found.wmClass, f.wmClass))
        return f;
    for (const MacApp &mac : macApps())
        if (std::find(mac.candidates.begin(), mac.candidates.end(), found.id) != mac.candidates.end())
            return withMac(found, mac);
    return found;
}

App forPid(uint32_t pid)
{
    const std::string exe = exeOf(pid);
    return exe.empty() ? App{} : forWindow(exe, exe, pid);
}

AK::Image icon(const App &app, int size)
{
    if (!app.macBundle.empty()) {
        std::string icns;
        if (app.id == "finder") {
            icns = "/System/Library/CoreServices/Finder.app/Contents/Resources/Finder.icns";
        } else {
            std::string file = AK::Assets::plistStrings(app.macBundle + "/Contents/Info.plist")["CFBundleIconFile"];
            if (!file.empty()) {
                if (file.find('.') == std::string::npos)
                    file += ".icns";
                icns = app.macBundle + "/Contents/Resources/" + file;
            }
        }
        if (!icns.empty() && AK::Assets::exists(icns)) {
            AK::Image img = AK::Image::load(AK::Assets::path(icns), {double(size), double(size)});
            if (!img.isNull())
                return img.width() == size ? img : img.scaled(size, size);
        }
    }
    const std::string file = themeIcon(app.iconName, size);
    if (!file.empty()) {
        AK::Image img = AK::Image::load(file, {double(size), double(size)});
        if (!img.isNull())
            return img.width() == size ? img : img.scaled(size, size);
    }
    return {};
}

bool launch(const App &app, const std::vector<std::string> &files)
{
    if (app.exec.empty())
        return false;
    std::vector<std::string> args = app.exec;
    args.insert(args.end(), files.begin(), files.end());
    return AK::Task::launchDetached(args);
}

bool open(const std::string &path)
{
    // PDFs and pictures are Preview's (LaunchServices' default on 10.6)
    static const std::set<std::string> preview = {"pdf", "png", "jpg", "jpeg", "tif", "tiff", "gif", "bmp", "icns"};
    const size_t dot = path.rfind('.');
    if (dot != std::string::npos && path.find("://") == std::string::npos) {
        std::string ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (preview.count(ext)) {
            const App a = forMacBundle("/Applications/Preview.app");
            if (a.valid() && launch(a, {path}))
                return true;
        }
    }
    return AK::Task::launchDetached({"xdg-open", path});
}

} // namespace LaunchServices
