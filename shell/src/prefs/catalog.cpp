// SPDX-License-Identifier: LGPL-2.1-or-later
#include "catalog.h"

#include "foundation.h"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <map>

using namespace AK;

namespace {

const char *const App = "/Applications/System Preferences.app";
const char *const Panes = "/System/Library/PreferencePanes";

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// NSPrefPaneGroups.xml: the groups (identifier, then their panes' identifiers), in order
std::vector<std::pair<std::string, std::vector<std::string>>> groupsXml()
{
    const std::string xml = readFile(Assets::path(std::string(App) + "/Contents/Resources/NSPrefPaneGroups.xml"));
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    auto stringAt = [&](size_t from, size_t &end) {
        const size_t s = xml.find("<string>", from) + 8;
        end = xml.find("</string>", s);
        return xml.substr(s, end - s);
    };
    for (size_t at = xml.find("<key>identifier</key>"); at != std::string::npos; at = xml.find("<key>identifier</key>", at)) {
        size_t end = 0;
        std::pair<std::string, std::vector<std::string>> g{stringAt(at, end), {}};
        const size_t panes = xml.find("<key>panes</key>", end);
        const size_t open = xml.find("<array", panes);
        at = end;
        if (panes != std::string::npos && open != std::string::npos && xml.compare(open, 8, "<array/>") != 0) {
            const size_t close = xml.find("</array>", open);
            for (size_t p = xml.find("<string>", open); p != std::string::npos && p < close; p = xml.find("<string>", end))
                g.second.push_back(stringAt(p, end));
            at = close;
        }
        out.push_back(g);
    }
    return out;
}

bool hardware(const std::string &id)
{
    // Trackpad, Ink, Fibre Channel: only with their hardware
    const std::string devices = lower(readFile("/proc/bus/input/devices"));
    if (id == "com.apple.preference.trackpad")
        return devices.find("touchpad") != std::string::npos || devices.find("trackpad") != std::string::npos;
    if (id == "com.apple.preference.ink")
        return devices.find("wacom") != std::string::npos || devices.find("tablet") != std::string::npos;
    if (id == "com.apple.prefpanel.fibrechannel") {
        DIR *d = opendir("/sys/class/fc_host");
        bool any = false;
        if (d) {
            while (dirent *e = readdir(d))
                any = any || e->d_name[0] != '.';
            closedir(d);
        }
        return any;
    }
    return true;
}

PaneInfo describe(const std::string &bundle)
{
    PaneInfo p;
    p.bundle = bundle;
    const auto info = Assets::plistStrings(bundle + "/Contents/Info.plist");
    auto get = [&](const char *k) {
        auto it = info.find(k);
        return it == info.end() ? std::string() : it->second;
    };
    p.id = get("CFBundleIdentifier");
    p.nib = get("NSMainNibFile");
    const auto strings = Assets::plistStrings(bundle + "/Contents/Resources/English.lproj/InfoPlist.strings");
    for (const char *k : {"NSPrefPaneIconLabel", "CFBundleName"}) {
        auto s = strings.find(k);
        if (s != strings.end() && !s->second.empty()) {
            p.label = s->second;
            break;
        }
        if (!get(k).empty()) {
            p.label = get(k);
            break;
        }
    }
    // the icon: CFBundleIconFile, NSPrefPaneIconFile, the bundle's own name, the first .icns
    std::vector<std::string> names{get("CFBundleIconFile"), get("NSPrefPaneIconFile"), baseName(bundle)};
    for (std::string n : names) {
        if (n.empty())
            continue;
        if (n.find('.') == std::string::npos)
            n += ".icns";
        Image img = Image::load(Assets::path(bundle + "/Contents/Resources/" + n), {32, 32});
        if (!img.isNull()) {
            p.icon = img.width() == 32 ? img : img.scaled(32, 32);
            break;
        }
    }
    if (p.icon.isNull())
        for (const std::string &f : files(Assets::path(bundle + "/Contents/Resources")))
            if (f.size() > 5 && f.compare(f.size() - 5, 5, ".icns") == 0) {
                Image img = Image::load(f, {32, 32});
                if (!img.isNull()) {
                    p.icon = img.width() == 32 ? img : img.scaled(32, 32);
                    break;
                }
            }
    return p;
}

} // namespace

std::string PaneInfo::name() const
{
    std::string n = label;
    std::replace(n.begin(), n.end(), '\n', ' ');
    return n;
}

std::string PaneInfo::resources() const { return Assets::path(bundle + "/Contents/Resources"); }

const std::vector<PaneGroup> &catalog()
{
    static const std::vector<PaneGroup> groups = [] {
        // every pane on the disk, by identifier
        std::map<std::string, PaneInfo> panes;
        if (DIR *d = opendir(Assets::path(Panes).c_str())) {
            while (dirent *e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() > 9 && n.compare(n.size() - 9, 9, ".prefPane") == 0) {
                    PaneInfo p = describe(std::string(Panes) + '/' + n);
                    if (!p.id.empty())
                        panes[p.id] = p;
                }
            }
            closedir(d);
        }
        std::vector<PaneGroup> out;
        for (const auto &[gid, ids] : groupsXml()) {
            PaneGroup g{gid, Assets::tr(App, "NSPrefPaneGroups", gid, gid), {}};
            for (const std::string &id : ids) {
                auto it = panes.find(id);
                if (it == panes.end() || !hardware(id))
                    continue;
                it->second.group = gid;
                g.panes.push_back(it->second);
                panes.erase(it);
            }
            out.push_back(g);
        }
        // the rest go to the catch-all group ("Other"), which shows only when it has some
        for (auto &[id, p] : panes)
            if (hardware(id) && id.rfind("com.apple.", 0) != 0)
                for (PaneGroup &g : out)
                    if (g.id == "addons")
                        g.panes.push_back(p);
        out.erase(std::remove_if(out.begin(), out.end(), [](const PaneGroup &g) { return g.panes.empty(); }), out.end());
        return out;
    }();
    return groups;
}

const PaneInfo *paneById(const std::string &id)
{
    for (const PaneGroup &g : catalog())
        for (const PaneInfo &p : g.panes)
            if (p.id == id)
                return &p;
    return nullptr;
}

const PaneInfo *paneByShortName(const std::string &name)
{
    static const std::map<std::string, std::string> names = {
        {"desktop", "com.apple.preference.desktopscreeneffect"}, {"screensaver", "com.apple.preference.desktopscreeneffect"},
        {"dock", "com.apple.preference.dock"}, {"appearance", "com.apple.preference.general"}, {"sound", "com.apple.preference.sound"},
        {"displays", "com.apple.preference.displays"}, {"energy", "com.apple.preference.energysaver"},
        {"keyboard", "com.apple.preference.keyboard"}, {"mouse", "com.apple.preference.mouse"}, {"network", "com.apple.preference.network"},
        {"sharing", "com.apple.preferences.sharing"}, {"accounts", "com.apple.preferences.users"}, {"datetime", "com.apple.preference.datetime"},
        {"softwareupdate", "com.apple.preferences.softwareupdate"}, {"startupdisk", "com.apple.preference.startupdisk"},
        {"bluetooth", "com.apple.preferences.Bluetooth"}, {"printfax", "com.apple.preference.printfax"}, {"expose", "com.apple.preference.expose"},
        {"security", "com.apple.preference.security"}, {"spotlight", "com.apple.preference.spotlight"}, {"language", "com.apple.Localization"},
        {"speech", "com.apple.preference.speech"}, {"timemachine", "com.apple.prefs.backup"}, {"universalaccess", "com.apple.preference.universalaccess"},
        {"parentalcontrols", "com.apple.preferences.parentalcontrols"}, {"mobileme", "com.apple.preference.internet"},
        {"cds", "com.apple.preference.digihub.discs"}, {"trackpad", "com.apple.preference.trackpad"},
    };
    auto it = names.find(lower(name));
    return paneById(it == names.end() ? name : it->second);
}

std::vector<std::string> searchPanes(const std::string &text)
{
    std::vector<std::string> out;
    const std::string t = lower(text);
    if (t.empty())
        return out;
    for (const PaneGroup &g : catalog())
        for (const PaneInfo &p : g.panes) {
            std::string words = lower(p.name());
            for (const std::string &f : files(p.resources() + "/English.lproj"))
                if (f.size() > 12 && f.compare(f.size() - 12, 12, ".searchTerms") == 0)
                    words += ' ' + lower(readFile(f));
            if (words.find(t) != std::string::npos)
                out.push_back(p.id);
        }
    return out;
}
