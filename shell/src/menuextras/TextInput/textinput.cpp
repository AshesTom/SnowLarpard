// SPDX-License-Identifier: LGPL-2.1-or-later
// TextInput.menu's code (AppleTextInputExtra, its TIMPrivate and TIMExtraView) on AquaKit's
// SystemUIPlugin, loaded by SystemUIServer as on the Mac. Reversed from 10.6's TextInput.menu 1.2.0,
// x86_64, with REA (docs/reverse-status.md, section 28):
//
//   - its words are its SharedSupport/TIMCore.bundle's (-bundleForResources, -createStringTemplates);
//   - -recreateMenu: an item an enabled input source (its icon, its name, -activate:), the current
//     one checked; a line; "Show / Hide Input Source Name" (ModeNameVisible in
//     com.apple.menuextra.textinput, tag -3); a line; "Open Keyboard…" (tag -2) with a palette among the
//     sources, "Open Language & Text…" (tag -2) otherwise;
//   - TIMExtraView -drawRect:: the current source's icon centred in 22 points; with its name shown, the
//     name after it (the system font of 11, black, white while the menu is down; wider than 90: the
//     system font of 9 kerned -0.8, still wider: cut in the middle), the view 22 + 1 + its width.
//
// The sources' names and icons are the keyboard layouts' (AppleKeyboardLayouts.bundle:
// AppleKeyboardLayouts-L.dat read below, its English.lproj names). On Linux the sources are XKB's
// layouts (setxkbmap -query), each shown as the Mac layout closest to it; choosing one makes it the
// first group (X11; the Wayland window server takes its keyboard from XKB_DEFAULT_*).
#include "systemuiplugin.h"

#include "foundation.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

using namespace AK;

namespace {

const char *const LayoutsBundle = "/System/Library/Keyboard Layouts/AppleKeyboardLayouts.bundle";

// AppleKeyboardLayouts-L.dat (little endian): u32 0xABCDEF02, u32 count, u32 the entries' offset
// (0x20), u32 0, u32 two tables' offsets (the layouts by ID); then an entry of twelve u32 a layout:
// [1] its name's offset (a C string: "French"), [2] its ID (KLID, a signed 16-bit value: 1),
// [4] its language's offset ("fr"), [6] [7] its 'uchr' size and offset, [10] [11] its icon's size and
// offset (an icns)
struct Layout {
    std::string name, language;
    int id = 0;
    std::vector<unsigned char> icon;
};

const std::map<std::string, Layout> &macLayouts()
{
    static const std::map<std::string, Layout> layouts = [] {
        std::map<std::string, Layout> out;
        std::ifstream in(Assets::path(std::string(LayoutsBundle) + "/Contents/Resources/AppleKeyboardLayouts-L.dat"), std::ios::binary);
        const std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), {});
        auto u32 = [&](size_t at) -> uint32_t {
            return at + 4 <= d.size() ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24 : 0;
        };
        auto cstr = [&](size_t at) {
            std::string s;
            while (at < d.size() && d[at])
                s += char(d[at++]);
            return s;
        };
        if (u32(0) != 0xABCDEF02)
            return out;
        const uint32_t count = u32(4), entries = u32(8);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t e = entries + 48 * size_t(i);
            Layout l;
            l.name = cstr(u32(e + 4));
            l.id = int16_t(uint16_t(u32(e + 8)));
            l.language = cstr(u32(e + 16));
            const uint32_t size = u32(e + 40), at = u32(e + 44);
            if (at + size <= d.size())
                l.icon.assign(d.begin() + at, d.begin() + at + size);
            out[l.name] = l;
        }
        return out;
    }();
    return layouts;
}

// XKB's layout (and variant) → the Mac's layout most like it (the shell's own table)
std::string macLayoutFor(const std::string &layout, const std::string &variant)
{
    static const std::map<std::string, std::string> withVariant = {
        {"us(intl)", "USInternational-PC"}, {"us(alt-intl)", "USInternational-PC"}, {"us(dvorak)", "Dvorak"},
        {"us(dvorak-l)", "Dvorak - Left"}, {"us(dvorak-r)", "Dvorak - Right"}, {"us(dvp)", "Dvorak"},
        {"ca(eng)", "Canadian"}, {"ca(multix)", "Canadian - CSA"}, {"ch(fr)", "Swiss French"}, {"ch(de)", "Swiss German"},
        {"cz(qwerty)", "Czech-QWERTY"}, {"sk(qwerty)", "Slovak-QWERTY"}, {"ru(phonetic)", "Russian - Phonetic"},
        {"bg(phonetic)", "Bulgarian - Phonetic"}, {"rs(latin)", "Serbian-Latin"}, {"tr(f)", "Turkish"},
        {"es(cat)", "Spanish - ISO"}, {"ie(ga)", "Irish Extended"}, {"no(smi)", "Northern Sami"},
        {"fi(smi)", "FinnishSami-PC"}, {"se(smi)", "SwedishSami-PC"}, {"gr(polytonic)", "Greek Polytonic"},
        {"il(phonetic)", "Hebrew-QWERTY"}, {"ara(qwerty)", "Arabic-QWERTY"}, {"in(guj)", "Gujarati"},
        {"in(guru)", "Gurmukhi"}, {"us(colemak)", "U.S."}};
    static const std::map<std::string, std::string> plain = {
        {"us", "U.S."}, {"gb", "British"}, {"fr", "French"}, {"de", "German"}, {"ch", "Swiss German"}, {"be", "Belgian"},
        {"ca", "Canadian - CSA"}, {"es", "Spanish - ISO"}, {"it", "Italian - Pro"}, {"pt", "Portuguese"}, {"br", "Brazilian"},
        {"nl", "Dutch"}, {"dk", "Danish"}, {"no", "Norwegian"}, {"se", "Swedish - Pro"}, {"fi", "Finnish"},
        {"at", "Austrian"}, {"ie", "Irish"}, {"au", "Australian"}, {"ru", "RussianWin"}, {"ua", "Ukrainian"},
        {"by", "Byelorussian"}, {"bg", "Bulgarian"}, {"cz", "Czech"}, {"sk", "Slovak"}, {"hu", "Hungarian"},
        {"pl", "Polish Pro"}, {"ee", "Estonian"}, {"lv", "Latvian"}, {"lt", "Lithuanian"}, {"gr", "Greek"},
        {"il", "Hebrew"}, {"ara", "Arabic PC"}, {"tr", "Turkish-QWERTY-PC"}, {"ro", "Romanian-Standard"},
        {"si", "Slovenian"}, {"hr", "Croatian-PC"}, {"rs", "Serbian"}, {"mk", "Macedonian"}, {"is", "Icelandic"},
        {"th", "Thai"}, {"in", "Devanagari"}, {"ir", "Persian"}, {"af", "Afghan Dari"}, {"am", "Armenian-HM QWERTY"},
        {"az", "Azeri"}, {"kz", "Kazakh"}, {"mt", "Maltese"}, {"fo", "Faroese"}, {"np", "Nepali"}, {"mao", "Maori"},
        {"cn", "U.S."}, {"jp", "U.S."}, {"kr", "U.S."}, {"tw", "U.S."}};
    auto v = withVariant.find(layout + "(" + variant + ")");
    if (!variant.empty() && v != withVariant.end())
        return v->second;
    auto p = plain.find(layout);
    return p == plain.end() ? std::string() : p->second;
}

// XKB's own name of a layout (evdev.lst), for one without a Mac counterpart
std::string xkbDescription(const std::string &layout, const std::string &variant)
{
    std::ifstream in("/usr/share/X11/xkb/rules/evdev.lst");
    std::string section, line;
    while (std::getline(in, line)) {
        if (line.rfind("! ", 0) == 0) {
            section = line.substr(2);
            continue;
        }
        std::istringstream l(line);
        std::string code, rest;
        l >> code;
        std::getline(l, rest);
        rest.erase(0, rest.find_first_not_of(' '));
        if (section == "layout" && variant.empty() && code == layout)
            return rest;
        if (section == "variant" && !variant.empty() && code == variant && rest.rfind(layout + ":", 0) == 0)
            return rest.substr(rest.find(':') + 2);
    }
    return layout;
}

std::string run(const std::string &command)
{
    std::string out;
    if (FILE *p = popen(command.c_str(), "r")) {
        char buf[1024];
        while (fgets(buf, sizeof buf, p))
            out += buf;
        pclose(p);
    }
    return out;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\n' && c != ' ') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

struct Source {
    std::string layout, variant;   // XKB's
    std::string name;              // the shown name
    Image icon;
};

std::string cutMiddle(const std::string &text, double width, const Font &font)
{
    if (textWidth(text, font) <= width)
        return text;
    for (size_t keep = text.size(); keep-- > 1;) {
        size_t head = (keep + 1) / 2, tail = keep / 2;
        while (head > 0 && (static_cast<unsigned char>(text[head]) & 0xC0) == 0x80)
            --head;
        size_t from = text.size() - tail;
        while (from < text.size() && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80)
            ++from;
        const std::string s = text.substr(0, head) + "…" + text.substr(from);
        if (textWidth(s, font) <= width)
            return s;
    }
    return "…";
}

} // namespace

class AppleTextInputExtra : public SystemUI::MenuExtra {
public:
    explicit AppleTextInputExtra(const std::string &bundle) : MenuExtra(bundle)
    {
        setView(new View(this));
        refresh();
        // the sources change with the keyboard's settings (TSM's notifications on the Mac)
        m_timer = App::shared().addTimer(2, true, [this] { refresh(); });
    }
    ~AppleTextInputExtra() override
    {
        if (m_timer)
            App::shared().removeTimer(m_timer);
    }

    std::vector<MenuItem> menu() override
    {
        // -recreateMenu
        refresh();
        std::vector<MenuItem> m;
        for (size_t i = 0; i < m_sources.size(); ++i) {
            MenuItem item{m_sources[i].name};
            item.image = m_sources[i].icon;
            item.checked = i == 0;
            item.action = [this, i] { activate(i); };
            m.push_back(item);
        }
        m.push_back(MenuItem::line());
        // the source's name in the bar (tag -3), -showHideSourceNameInMenuBar:
        const bool visible = nameVisible();
        MenuItem name{visible ? tr("Hide Name of Source in Menu Bar", "Hide Input Source Name") : tr("Show Name of Source in Menu Bar", "Show Input Source Name")};
        name.tag = -3;
        name.action = [this, visible] {
            Settings("textinput").set("ModeNameVisible", !visible);
            updateLength();
        };
        m.push_back(name);
        m.push_back(MenuItem::line());
        // no palettes (Character Viewer, Keyboard Viewer) among the sources: Language & Text's
        MenuItem prefs{tr("Open IntlPrefs", "Open Language & Text…")};
        prefs.tag = -2;
        prefs.action = [] { openPreferencePane("com.apple.Localization"); };
        m.push_back(prefs);
        return m;
    }

private:
    class View : public SystemUI::MenuExtraView {
    public:
        explicit View(AppleTextInputExtra *e) : MenuExtraView(e), extra(e) {}
        AppleTextInputExtra *extra;
        void draw(cairo_t *cr) override
        {
            // TIMExtraView -drawRect:
            const Rect r{0, 0, frame().w, frame().h - 1};
            const bool down = extra->isMenuDown();
            if (down)
                drawHighlight(cr, r);
            if (extra->m_sources.empty())
                return;
            const Source &s = extra->m_sources.front();
            if (!s.icon.isNull())
                s.icon.draw(cr, Point{std::floor((22 - s.icon.width()) / 2.0), std::floor((r.h - s.icon.height()) / 2)});
            if (!extra->nameVisible())
                return;
            TextStyle t;
            t.font = extra->nameFont(s.name);
            t.color = down ? Color::gray(255) : Color::gray(0);
            t.elide = false;
            drawText(cr, extra->shownName(s.name), Rect{22, 0, r.w - 22, r.h}, t);
        }
    };

    std::string tr(const char *key, const char *fallback) const
    {
        // -bundleForResources: SharedSupport/TIMCore.bundle
        return Assets::tr(bundlePath() + "/Contents/SharedSupport/TIMCore.bundle", "Localizable", key, fallback);
    }

    static bool nameVisible() { return Settings("textinput").boolean("ModeNameVisible", false); }

    // -imageOfString:onMenuDown:: the system font of 11; past 90 points, of 9 (kerned -0.8: not
    // applied here); still past 90, cut in the middle
    Font nameFont(const std::string &name) const
    {
        return textWidth(name, Font::system(11)) > 90 ? Font::system(9) : Font::system(11);
    }
    std::string shownName(const std::string &name) const { return cutMiddle(name, 90, nameFont(name)); }

    void updateLength()
    {
        double length = 22;
        if (nameVisible() && !m_sources.empty())
            length = 22 + 1 + std::ceil(textWidth(shownName(m_sources.front().name), nameFont(m_sources.front().name)));
        setLength(length);
        setNeedsDisplay();
    }

    // the enabled sources: XKB's layouts, the active one first
    void refresh()
    {
        const std::string q = run("setxkbmap -query 2>/dev/null");
        std::string layouts, variants;
        std::istringstream in(q);
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("layout:", 0) == 0)
                layouts = line.substr(7);
            else if (line.rfind("variant:", 0) == 0)
                variants = line.substr(8);
        }
        if (layouts.empty()) {
            const char *env = std::getenv("XKB_DEFAULT_LAYOUT");
            layouts = env ? env : "us";
            const char *v = std::getenv("XKB_DEFAULT_VARIANT");
            variants = v ? v : "";
        }
        const std::string key = layouts + "|" + variants;
        if (key == m_key)
            return;
        m_key = key;
        m_sources.clear();
        const std::vector<std::string> ls = split(layouts, ','), vs = split(variants, ',');
        for (size_t i = 0; i < ls.size(); ++i) {
            if (ls[i].empty())
                continue;
            Source s;
            s.layout = ls[i];
            s.variant = i < vs.size() ? vs[i] : std::string();
            const std::string mac = macLayoutFor(s.layout, s.variant);
            auto it = macLayouts().find(mac);
            if (it != macLayouts().end()) {
                // the bundle's English.lproj names (InfoPlist.strings), the icon from the .dat
                s.name = Assets::tr(LayoutsBundle, "InfoPlist", mac, mac);
                s.icon = Image::fromData(it->second.icon, {16, 16});
            } else {
                s.name = xkbDescription(s.layout, s.variant);
            }
            m_sources.push_back(s);
        }
        updateLength();
    }

    // -activate: (TISSelectInputSource): the layout made the first group, the others kept after it
    void activate(size_t index)
    {
        if (index >= m_sources.size() || index == 0)
            return;
        std::vector<Source> order = m_sources;
        std::rotate(order.begin(), order.begin() + long(index), order.begin() + long(index) + 1);
        std::string layouts, variants;
        for (size_t i = 0; i < order.size(); ++i) {
            layouts += (i ? "," : "") + order[i].layout;
            variants += (i ? "," : "") + order[i].variant;
        }
        Task::launchDetached({"setxkbmap", "-layout", layouts, "-variant", variants});
        m_sources = order;
        m_key.clear();
        updateLength();
    }

    std::vector<Source> m_sources;
    std::string m_key;
    int m_timer = 0;
};

SYSTEMUI_REGISTER_EXTRA("AppleTextInputExtra", AppleTextInputExtra);
