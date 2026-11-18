// SPDX-License-Identifier: LGPL-2.1-or-later
#include "battery.h"

#include "foundation.h"
#include "runloop.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <fstream>

using namespace AK;

namespace {

std::string readSys(const std::string &path)
{
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

long long number(const std::string &path) { return std::atoll(readSys(path).c_str()); }

std::vector<std::string> supplies()
{
    std::vector<std::string> out;
    if (DIR *d = opendir("/sys/class/power_supply")) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.')
                out.push_back(std::string("/sys/class/power_supply/") + e->d_name);
        closedir(d);
    }
    return out;
}

std::string replace(std::string format, const std::string &value)
{
    const size_t at = format.find("%@");
    if (at != std::string::npos)
        format.replace(at, 2, value);
    return format;
}

class BatteryView : public SystemUI::MenuExtraView {
public:
    explicit BatteryView(BatteryExtra *e) : MenuExtraView(e), battery(e) {}
    BatteryExtra *battery;
    void draw(cairo_t *cr) override
    {
        const bool down = battery->isMenuDown();
        const Image img = battery->picture(down);
        double x = 1;
        if (!img.isNull()) {
            img.draw(cr, Point{x, std::floor((frame().h - 1 - img.height()) / 2)});
            x += img.width() + 2;
        }
        const std::string text = battery->barText();
        if (!text.empty()) {
            TextStyle t;
            t.font = SystemUI::menuBarFont();
            t.color = down ? Color::gray(255) : Color::gray(0);
            t.elide = false;
            drawText(cr, text, Rect{x, 0, frame().w - x, frame().h - 1}, t);
        }
    }
};

} // namespace

BatteryExtra::BatteryExtra(const std::string &bundle) : MenuExtra(bundle)
{
    Settings prefs("menuextra.battery");
    m_showTime = prefs.boolean("ShowTime", false);
    m_showPercent = prefs.boolean("ShowPercent", false);
    setView(new BatteryView(this));
    update();
    m_timer = App::shared().addTimer(10, true, [this] { update(); });
}

BatteryExtra::~BatteryExtra() { willUnload(); }

void BatteryExtra::willUnload()
{
    if (m_timer)
        App::shared().removeTimer(m_timer);
    m_timer = 0;
}

void BatteryExtra::update()
{
    State s;
    double now = 0, full = 0, rate = 0;
    bool any = false, allFull = true;
    for (const std::string &p : supplies()) {
        const std::string type = readSys(p + "/type");
        if (type == "Mains") {
            s.plugged = s.plugged || readSys(p + "/online") == "1";
            continue;
        }
        if (type != "Battery" || readSys(p + "/present") == "0" || readSys(p + "/scope") == "Device")
            continue;
        any = true;
        const std::string status = readSys(p + "/status");
        s.charging = s.charging || status == "Charging";
        allFull = allFull && status == "Full";
        // energy (µWh) or charge (µAh), and the rate
        const char *nowKey = AK::isFile(p + "/energy_now") ? "/energy_now" : "/charge_now";
        const char *fullKey = AK::isFile(p + "/energy_full") ? "/energy_full" : "/charge_full";
        const char *rateKey = AK::isFile(p + "/power_now") ? "/power_now" : "/current_now";
        now += double(number(p + nowKey));
        full += double(number(p + fullKey));
        rate += std::fabs(double(number(p + rateKey)));
        if (full <= 0 && AK::isFile(p + "/capacity"))
            now = double(number(p + "/capacity")), full = 100;
    }
    s.present = any;
    s.charged = any && allFull;
    if (any && full > 0)
        s.percent = std::min(100.0, now / full * 100);
    if (s.charging || !s.plugged) {
        if (rate > 0)
            s.minutes = int(std::lround((s.charging ? full - now : now) / rate * 60));
        else
            s.calculating = true;
    }
    s.warning = !s.plugged && s.percent <= 10;   // the low battery warning
    m_state = s;
    // the length: the picture and its text (BatteryViewInMenu -resizeIfNecessary)
    const std::string text = barText();
    setLength(1 + 22 + (text.empty() ? 1 : 2 + std::ceil(textWidth(text, SystemUI::menuBarFont())) + 2));
    setNeedsDisplay();
}

Image BatteryExtra::picture(bool white) const
{
    // -singleBatteryImageForBattery:partialOpacity:
    const State &s = m_state;
    Image canvas(22, 12);
    cairo_t *cr = cairo_create(canvas.surface());
    const Image empty = Image::load(resourcePath(s.present ? "BatteryEmpty.pdf" : "BatteryNone.pdf"));
    if (!empty.isNull())
        empty.draw(cr, Point{0, 0});
    const bool red = s.warning && s.present;
    if (s.present && s.plugged && (s.charging || s.charged)) {
        const Image glyph = Image::load(resourcePath(s.charging && !s.charged ? "glyphCharging.pdf" : "glyphCharged.pdf"));
        if (!glyph.isNull())
            glyph.draw(cr, Point{std::floor((22 - glyph.width()) / 2.0) - 1 + 1, std::floor((12 - glyph.height()) / 2.0)});
    } else if (s.present) {
        // the juice: 13 points full, from 3 points in
        const double width = std::round(13 * std::clamp(s.percent, 0.0, 100.0) / 100);
        if (width >= 1) {
            const std::string c = red ? "R" : "B";
            const Image l = Image::load(resourcePath("BatteryLevelCap" + c + "-L.pdf")), m = Image::load(resourcePath("BatteryLevelCap" + c + "-M.pdf")),
                        r = Image::load(resourcePath("BatteryLevelCap" + c + "-R.pdf"));
            if (!l.isNull() && !m.isNull() && !r.isNull()) {
                l.draw(cr, Point{3, 3});
                if (width > 2)
                    m.draw(cr, Rect{4, 3, width - 2, 6});
                if (width > 1)
                    r.draw(cr, Point{3 + width - 1, 3});
            } else {
                fill(cr, Rect{3, 3, width, 6}, red ? Color::rgb(200, 0, 0) : Color::gray(0));
            }
        }
    }
    cairo_destroy(cr);
    // a template unless red: white while the menu is down
    return white && !red ? canvas.tinted(Color::gray(255)) : canvas;
}

std::string BatteryExtra::timeString(int minutes) const
{
    char m[8];
    std::snprintf(m, sizeof m, "%02d", minutes % 60);
    return std::to_string(minutes / 60) + ":" + m;
}

std::string BatteryExtra::barText() const
{
    const State &s = m_state;
    if ((!m_showTime && !m_showPercent) || !s.present)
        return {};
    if (s.charged && m_showTime)
        return localized("CHARGED", "Charged");
    if (s.calculating && m_showTime)
        return localized("CALCULATING", "Calculating…");
    if (s.plugged && !s.charging && !s.charged)
        return localized("BAT_NOT_CHARGING_MENU_BAR", "Not Charging");
    if (!m_showTime)
        return "(" + std::to_string(int(std::lround(s.percent))) + "%)";
    if (s.minutes < 0)
        return localized("CHARGED", "Charged");
    return "(" + timeString(s.minutes) + ")";
}

std::string BatteryExtra::statusTitle() const
{
    const State &s = m_state;
    std::string title;
    if (!s.present) {
        title = localized("NO_BATS", "No Batteries Available");
    } else if (s.charged) {
        title = localized("BAT_CHARGED", "Battery Is Charged");
    } else if (s.calculating && !m_showTime) {
        title = s.plugged ? localized("CALCULATING_TIME_UNTIL_FULL", "Calculating Time Until Full…")
                          : localized("CALCULATING_TIME_REMAINING", "Calculating Time Remaining…");
    } else if (s.plugged && !s.charging) {
        title = localized("BAT_NOT_CHARGING", "Battery Is Not Charging");
    } else if (!m_showTime) {
        title = replace(s.plugged ? localized("TIME_UNTIL_FULL", "%@ Until Full") : localized("TIME_REMAINING", "%@ Remaining"),
                        timeString(std::max(0, s.minutes)));
    } else {
        const std::string pct = std::to_string(int(std::lround(s.percent))) + "%";
        title = replace(s.plugged ? localized("PERCENT_FULL", "%@ Full") : localized("TIME_REMAINING", "%@ Remaining"), pct);
    }
    if (!m_showTime && !m_showPercent)
        title = replace(localized("ICON_ONLY_STATUS_FORMAT", "%@"), title);
    return title;
}

std::vector<MenuItem> BatteryExtra::menu()
{
    update();
    std::vector<MenuItem> m;
    m.emplace_back(statusTitle(), false, false);
    m.emplace_back(m_state.plugged ? localized("POWER_SOURCE_ADAPTER", "Power Source: Power Adapter")
                                   : localized("POWER_SOURCE_BATTERY", "Power Source: Battery"),
                   false, false);
    m.push_back(MenuItem::line());
    MenuItem show(localized("SHOW", "Show"));
    auto choice = [this](const std::string &title, int tag, bool on) {
        MenuItem i(title, false, true, on);
        i.tag = tag;
        i.action = [this, tag] {
            // -selectShowMenuItem:
            m_showTime = tag == 1;
            m_showPercent = tag == 2;
            Settings prefs("menuextra.battery");
            prefs.set("ShowTime", m_showTime);
            prefs.set("ShowPercent", m_showPercent);
            update();
        };
        return i;
    };
    show.submenu.push_back(choice(localized("NONE", "Icon Only"), 3, !m_showTime && !m_showPercent));
    show.submenu.push_back(choice(localized("DISPLAY_TIME", "Time"), 1, m_showTime));
    show.submenu.push_back(choice(localized("DISPLAY_PERCENT", "Percentage"), 2, m_showPercent));
    m.push_back(show);
    m.push_back(MenuItem::line());
    MenuItem es(localized("OPEN_ES", "Energy Saver Preferences…"));
    es.action = [] {
        Task::launchDetached({dirName(dirName(dirName(dirName(executablePath())))) + "/System Preferences.app/Contents/MacOS/System Preferences",
                              "--pane", "com.apple.preference.energysaver"});
    };
    m.push_back(es);
    return m;
}

SYSTEMUI_REGISTER_EXTRA("BatteryExtra", BatteryExtra);
