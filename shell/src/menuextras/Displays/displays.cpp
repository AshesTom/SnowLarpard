// SPDX-License-Identifier: LGPL-2.1-or-later
// Displays.menu's code (DisplaysExtra) on AquaKit's SystemUIPlugin, loaded by SystemUIServer as on the
// Mac. Reversed from 10.6's Displays.menu 2.2, x86_64, with REA (docs/reverse-status.md, section 27):
//
//   - -initWithBundle:: display.pdf as a template image; the menu made again when the screens change;
//   - -createMenu: "Detect Displays" (when a display can detect), "Turn On / Off Mirroring" (more than
//     one display, mirroring not forced), a line; for each display its name (disabled; "Main Display",
//     "Display %d" when two have the same name, -nameDupeDisplays) then its modes: the recent ones
//     (com.apple.preference.displays2's numberOfRecentModes, 3 by default; its recentModeDescriptions),
//     the current one put in (-MakeSureOurModeIsAccountedFor), filled up with the largest safe modes
//     in millions of colours (_AddDefaultModes), sorted by width, height and refresh rate (television
//     modes last), checked the current one; a line between displays; a line; "Number of Recent Items"
//     (0, 3, 5, 10); "Displays Preferences…";
//   - -timingString:forDisplayID:: "%d × %d, %d Hz", "%d × %d, %.1f Hz" when the rate isn't whole,
//     "%d × %d" for a display without a rate (an LCD), "(Interlaced)" and the television names; the
//     depth (" , Thousands"...) only for fewer than millions of colours (-modeString:forDisplayID:);
//   - -_displayMenuHit:: the mode set (CGSConfigureDisplayMode); one not flagged safe asks to be kept
//     (DisplaysMenu.nib, 15 seconds, then put back).
//
// On Linux: RandR through xrandr(1): the outputs and their modes (every mode it lists comes from the
// display's EDID: safe, so no confirmation), the names from the EDID's monitor name descriptor.
#include "systemuiplugin.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

using namespace AK;

namespace {

struct Mode {
    int width = 0, height = 0;
    double rate = 0;          // Hz; 0 for a display without one
    bool interlaced = false;
    bool current = false;
    std::string name;         // xrandr's mode name
    bool operator==(const Mode &o) const
    {
        return width == o.width && height == o.height && std::fabs(rate - o.rate) < 0.05 && interlaced == o.interlaced;
    }
};

struct Output {
    std::string connector;    // eDP-1
    std::string name;         // the EDID's monitor name
    bool primary = false, builtIn = false;
    int x = 0, y = 0;
    std::vector<Mode> modes;
};

std::string run(const std::string &command)
{
    std::string out;
    if (FILE *p = popen(command.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0)
            out.append(buf, n);
        pclose(p);
    }
    return out;
}

// the EDID's descriptors: 0xFC the monitor name (13 bytes, ended by a newline)
std::string edidName(const std::string &hex)
{
    std::vector<unsigned char> b;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        b.push_back(static_cast<unsigned char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    for (size_t d = 54; d + 18 <= b.size() && d < 126; d += 18)
        if (b[d] == 0 && b[d + 1] == 0 && b[d + 3] == 0xFC) {
            std::string s;
            for (size_t i = d + 5; i < d + 18 && b[i] != '\n'; ++i)
                s += char(b[i]);
            while (!s.empty() && s.back() == ' ')
                s.pop_back();
            return s;
        }
    return {};
}

std::vector<Output> outputs()
{
    std::vector<Output> out;
    std::istringstream in(run("xrandr --query --verbose 2>/dev/null"));
    Output *o = nullptr;
    bool inEdid = false;
    std::string edid;
    Mode *pending = nullptr;   // --verbose: a mode's line, then its h: and v: lines
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
            if (o && !edid.empty())
                o->name = edidName(edid);
            o = nullptr;
            edid.clear();
            inEdid = false;
            pending = nullptr;
            std::istringstream l(line);
            std::string connector, state;
            l >> connector >> state;
            if (state != "connected")
                continue;
            out.push_back({});
            o = &out.back();
            o->connector = connector;
            o->primary = line.find(" primary ") != std::string::npos;
            o->builtIn = connector.rfind("eDP", 0) == 0 || connector.rfind("LVDS", 0) == 0 || connector.rfind("DSI", 0) == 0;
            int w, h, x, y;
            for (std::string word; l >> word;)
                if (std::sscanf(word.c_str(), "%dx%d+%d+%d", &w, &h, &x, &y) == 4) {
                    o->x = x;
                    o->y = y;
                    break;
                }
            continue;
        }
        if (!o)
            continue;
        const std::string t = line.substr(line.find_first_not_of(" \t"));
        if (t.rfind("EDID:", 0) == 0) {
            inEdid = true;
            continue;
        }
        if (inEdid) {
            if (t.find(':') == std::string::npos && std::all_of(t.begin(), t.end(), ::isxdigit)) {
                edid += t;
                continue;
            }
            inEdid = false;
        }
        // "  1920x1080 (0x48) 148.500MHz +HSync +VSync *current +preferred"
        int w, h;
        char sep;
        if (t.find("MHz") != std::string::npos && std::sscanf(t.c_str(), "%dx%d%c", &w, &h, &sep) >= 2) {
            Mode m;
            m.name = t.substr(0, t.find(' '));
            m.width = w;
            m.height = h;
            m.interlaced = t.find("Interlace") != std::string::npos || m.name.find('i') != std::string::npos;
            m.current = t.find("*current") != std::string::npos;
            o->modes.push_back(m);
            pending = &o->modes.back();
            continue;
        }
        // "        v: height 1080 start 1083 end 1088 total 1120           clock  60.00Hz"
        if (pending && t.rfind("v:", 0) == 0) {
            const size_t c = t.find("clock");
            if (c != std::string::npos)
                pending->rate = std::atof(t.c_str() + c + 5);
            pending = nullptr;
        }
    }
    if (o && !edid.empty())
        o->name = edidName(edid);
    // the main display first (CGSGetOnlineDisplayList)
    std::stable_partition(out.begin(), out.end(), [](const Output &a) { return a.primary; });
    return out;
}

} // namespace

class DisplaysExtra : public SystemUI::MenuExtra {
public:
    explicit DisplaysExtra(const std::string &bundle) : MenuExtra(bundle)
    {
        setImage(templateImage("display.pdf"));
        setAlternateImage(templateImage("display.pdf", true));
    }

    std::vector<MenuItem> menu() override
    {
        // -createMenu
        std::vector<MenuItem> m;
        std::vector<Output> list = outputs();
        names(list);
        auto item = [&](const std::string &title, std::function<void()> action) {
            MenuItem i{title};
            i.action = std::move(action);
            m.push_back(i);
        };
        // DoWeHaveDisplayDetection: RandR probes the connectors each time it is asked
        item(tr("Detect Displays", "Detect Displays"), [] { Task::launchDetached({"xrandr", "--auto"}); });
        if (list.size() > 1) {
            const bool mirrored = std::all_of(list.begin(), list.end(), [&](const Output &o) { return o.x == list[0].x && o.y == list[0].y; });
            item(mirrored ? tr("Turn Off Mirroring", "Turn Off Mirroring") : tr("Turn On Mirroring", "Turn On Mirroring"),
                 [list, mirrored] { mirror(list, !mirrored); });
        }
        m.push_back(MenuItem::line());
        const int recent = numberOfRecentModes();
        int shown = 0;
        for (size_t d = 0; d < list.size() && recent > 0; ++d) {
            const Output &o = list[d];
            MenuItem header{o.name};
            header.enabled = false;
            m.push_back(header);
            shown = 0;
            for (const Mode &mode : modesToShow(o, recent)) {
                MenuItem i{modeString(mode, o)};
                i.checked = mode.current;
                const std::string connector = o.connector, name = mode.name;
                const double rate = mode.rate;
                i.action = [this, connector, name, rate, mode] {
                    remember(connector, mode);
                    setMode(connector, name, rate);
                };
                m.push_back(i);
                ++shown;
            }
            if (d + 1 < list.size())
                m.push_back(MenuItem::line());
        }
        if (shown > 0)
            m.push_back(MenuItem::line());
        MenuItem count{tr("Recent Modes", "Number of Recent Items")};
        for (int n : {0, 3, 5, 10}) {
            MenuItem c{tr(std::to_string(n).c_str(), std::to_string(n).c_str())};
            c.checked = recent == n;
            c.action = [n] { Settings("displays2").set("numberOfRecentModes", n); };   // _SetNumberOfRecentModesToDisplay
            count.submenu.push_back(c);
        }
        m.push_back(count);
        item(tr("Open Displays", "Displays Preferences…"), [] { openPreferencePane("com.apple.preference.displays"); });
        return m;
    }

private:
    std::string tr(const char *key, const char *fallback) const { return localized(key, fallback); }

    // CopyDisplayName, then -nameDupeDisplays when two are alike: "Main Display", "Display 2"...
    void names(std::vector<Output> &list) const
    {
        for (Output &o : list)
            if (o.name.empty())
                o.name = o.builtIn ? std::string("Color LCD") : o.connector;
        bool dupes = false;
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j)
                dupes = dupes || list[i].name == list[j].name;
        if (!dupes)
            return;
        int n = 1;
        for (size_t i = 0; i < list.size(); ++i) {
            if (i == 0) {
                list[i].name = tr("Main Display", "Main Display");
                continue;
            }
            std::string f = tr("Display n", "Display %d");
            const size_t at = f.find("%d");
            if (at != std::string::npos)
                f.replace(at, 2, std::to_string(++n));
            list[i].name = f;
        }
    }

    // _GetNumberOfRecentModesToDisplay: unset (or negative) is 3
    static int numberOfRecentModes()
    {
        const int n = Settings("displays2").integer("numberOfRecentModes", -1);
        return n < 0 ? 3 : n;
    }

    // _GetRecentModes, _MakeSureOurModeIsAccountedFor, _AddDefaultModes, then the qsort
    std::vector<Mode> modesToShow(const Output &o, int count) const
    {
        std::vector<Mode> chosen;
        for (const std::string &key : Settings("displays2").list("recentModeDescriptions." + o.connector)) {
            int w, h;
            double r;
            if (std::sscanf(key.c_str(), "%dx%d@%lf", &w, &h, &r) != 3)
                continue;
            for (const Mode &m : o.modes)
                if (m.width == w && m.height == h && std::fabs(m.rate - r) < 0.05 && !m.interlaced &&
                    std::find(chosen.begin(), chosen.end(), m) == chosen.end()) {
                    chosen.push_back(m);
                    break;
                }
            if (int(chosen.size()) >= count)
                break;
        }
        auto current = std::find_if(o.modes.begin(), o.modes.end(), [](const Mode &m) { return m.current; });
        if (current != o.modes.end() && std::find(chosen.begin(), chosen.end(), *current) == chosen.end()) {
            if (int(chosen.size()) < count)
                chosen.push_back(*current);
            else
                chosen.back() = *current;
        }
        // the largest safe modes not there yet (the sum of width and height), one rate a size
        const size_t had = chosen.size();
        for (const Mode &m : o.modes) {
            if (m.interlaced || std::find(chosen.begin(), chosen.end(), m) != chosen.end())
                continue;
            if (std::any_of(chosen.begin() + long(had), chosen.end(), [&](const Mode &c) { return c.width == m.width && c.height == m.height; }))
                continue;
            if (int(chosen.size()) < count) {
                chosen.push_back(m);
                continue;
            }
            for (size_t i = had; i < chosen.size(); ++i)
                if (chosen[i].width + chosen[i].height < m.width + m.height) {
                    chosen[i] = m;
                    break;
                }
        }
        std::sort(chosen.begin(), chosen.end(), [](const Mode &a, const Mode &b) {
            if (a.width != b.width)
                return a.width < b.width;
            if (a.height != b.height)
                return a.height < b.height;
            return a.rate < b.rate;
        });
        return chosen;
    }

    // -timingString:forDisplayID: (with -modeString:'s depth left out: millions of colours)
    std::string modeString(const Mode &m, const Output &o) const
    {
        const std::string w = std::to_string(m.width), h = std::to_string(m.height);
        auto fill = [](std::string f, const std::vector<std::string> &v) {
            size_t i = 0;
            for (size_t at; i < v.size() && (at = f.find('%')) != std::string::npos; ++i) {
                size_t end = at + 1;
                while (end < f.size() && !std::isalpha(static_cast<unsigned char>(f[end])))
                    ++end;
                f.replace(at, end + 1 - at, v[i]);
            }
            return f;
        };
        // a built-in panel shows no rate (_displayHasZeroHertz: an LCD's modes at 0 Hz)
        if (o.builtIn || m.rate <= 0)
            return fill(m.interlaced ? tr("InterlacedTimingString0Hz", "%d × %d (interlaced)") : tr("ResolutionTimingString0Hz", "%d × %d"), {w, h});
        const bool whole = std::fabs(m.rate - std::round(m.rate)) < 0.05;
        char rate[32];
        if (whole)
            std::snprintf(rate, sizeof rate, "%d", int(std::lround(m.rate)));
        else
            std::snprintf(rate, sizeof rate, "%.1f", m.rate);
        if (m.interlaced)
            return fill(whole ? tr("InterlacedTimingString", "%d × %d (Interlaced)") : tr("InterlacedTimingStringFract", "%d × %d %.1f Hz (Interlaced)"),
                        {w, h, rate});
        return fill(whole ? tr("ResolutionTimingString", "%d × %d, %d Hz") : tr("ResolutionTimingStringFract", "%d × %d, %.1f Hz"), {w, h, rate});
    }

    // the recent modes kept as the Displays pane keeps them (recentModeDescriptions), latest first
    static void remember(const std::string &connector, const Mode &m)
    {
        Settings s("displays2");
        const std::string key = "recentModeDescriptions." + connector;
        std::vector<std::string> list = s.list(key);
        char d[64];
        std::snprintf(d, sizeof d, "%dx%d@%.2f", m.width, m.height, m.rate);
        list.erase(std::remove(list.begin(), list.end(), std::string(d)), list.end());
        list.insert(list.begin(), d);
        if (list.size() > 10)
            list.resize(10);
        s.set(key, list);
    }

    // -_displayMenuHit:: CGSConfigureDisplayMode (an EDID's mode: safe, no confirmation)
    static void setMode(const std::string &connector, const std::string &mode, double rate)
    {
        char r[32];
        std::snprintf(r, sizeof r, "%.2f", rate);
        if (rate > 0)
            Task::launchDetached({"xrandr", "--output", connector, "--mode", mode, "--rate", r});
        else
            Task::launchDetached({"xrandr", "--output", connector, "--mode", mode});
    }

    // -_mirrorMenuHit:: MirrorAllDisplays / UnmirrorAllDisplays
    static void mirror(const std::vector<Output> &list, bool on)
    {
        std::vector<std::string> args{"xrandr"};
        for (size_t i = 1; i < list.size(); ++i) {
            args.insert(args.end(), {"--output", list[i].connector});
            if (on)
                args.insert(args.end(), {"--same-as", list[0].connector});
            else
                args.insert(args.end(), {"--right-of", list[i - 1].connector});
        }
        Task::launchDetached(args);
    }
};

SYSTEMUI_REGISTER_EXTRA("DisplaysExtra", DisplaysExtra);
