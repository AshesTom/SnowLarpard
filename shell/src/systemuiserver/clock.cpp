// SPDX-License-Identifier: LGPL-2.1-or-later
#include "clock.h"

#include "coreui.h"
#include "foundation.h"
#include "text.h"

#include <clocale>
#include <cmath>
#include <ctime>
#include <langinfo.h>
#include <sys/time.h>

using namespace AK;

namespace {

// strftime with one conversion
std::string strf(const char *fmt, const std::tm &tm)
{
    char buf[128];
    const size_t n = std::strftime(buf, sizeof buf, fmt, &tm);
    return std::string(buf, n);
}

// The locale's clock: a 12-hour one when its time format has AM/PM
bool twelveHours()
{
    const char *t = nl_langinfo(T_FMT);
    const std::string f = t ? t : "";
    return f.find("%p") != std::string::npos || f.find("%r") != std::string::npos || f.find("%I") != std::string::npos;
}

class DigitalView : public SystemUI::MenuExtraView {
public:
    explicit DigitalView(AppleClockExtra *e) : MenuExtraView(e), clock(e) {}
    AppleClockExtra *clock;
    void draw(cairo_t *cr) override
    {
        TextStyle t;
        t.font = SystemUI::menuBarFont();
        t.color = clock->isMenuDown() ? Color::gray(255) : Color::gray(0);
        t.align = Align::Center;
        t.elide = false;
        // -cellFrame: the cell one point down (the bar's bottom line out of it)
        drawText(cr, clock->text(), Rect{0, 0, frame().w, frame().h - 1}, t);
    }
};

class AnalogView : public SystemUI::MenuExtraView {
public:
    explicit AnalogView(AppleClockExtra *e) : MenuExtraView(e) {}
    void draw(cairo_t *cr) override
    {
        const bool white = m_extra->isMenuDown();
        static Image face, faceWhite;
        if (face.isNull()) {
            face = m_extra->templateImage("Clock.pdf");
            faceWhite = m_extra->templateImage("Clock.pdf", true);
        }
        const Image &img = white ? faceWhite : face;
        const double w = frame().w, h = frame().h;
        if (!img.isNull())
            img.draw(cr, Point{std::floor((w - img.width()) / 2), std::floor((h - img.height()) / 2)});
        // the centre, (w / 2 - 0.5, h / 2 + 0.5) up from the bottom
        const double cx = w / 2 - 0.5, cy = h / 2 - 0.5;
        const time_t now = time(nullptr);
        std::tm tm;
        localtime_r(&now, &tm);
        auto hand = [&](double length, double degrees, double width) {
            const double a = degrees * M_PI / 180;
            cairo_move_to(cr, cx, cy);
            cairo_line_to(cr, cx + length * std::sin(a), cy - length * std::cos(a));
            cairo_set_line_width(cr, width);
            cairo_set_source_rgb(cr, white, white, white);
            cairo_stroke(cr);
        };
        hand(h / 5.5, (tm.tm_hour % 12) / 12.0 * 360 + tm.tm_min / 60.0 * 30, 1.0);
        hand(h * 0.25, tm.tm_min / 60.0 * 360, 0.75);
    }
};

} // namespace

std::string formatDate(const std::string &pattern, time_t when)
{
    std::tm tm;
    localtime_r(&when, &tm);
    std::string out;
    for (size_t i = 0; i < pattern.size();) {
        const char c = pattern[i];
        if (c == '\'') {   // quoted text; '' is a quote
            size_t j = i + 1;
            if (j < pattern.size() && pattern[j] == '\'') {
                out += '\'';
                i += 2;
                continue;
            }
            while (j < pattern.size() && pattern[j] != '\'')
                out += pattern[j++];
            i = j + 1;
            continue;
        }
        if (!std::isalpha((unsigned char)c)) {
            out += c;
            ++i;
            continue;
        }
        size_t n = 1;
        while (i + n < pattern.size() && pattern[i + n] == c)
            ++n;
        i += n;
        switch (c) {
        case 'E': out += strf(n >= 4 ? "%A" : "%a", tm); break;
        case 'H': out += n >= 2 ? strf("%H", tm) : std::to_string(tm.tm_hour); break;
        case 'h': {
            const int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
            out += n >= 2 && h < 10 ? "0" + std::to_string(h) : std::to_string(h);
            break;
        }
        case 'm': out += n >= 2 ? strf("%M", tm) : std::to_string(tm.tm_min); break;
        case 's': out += n >= 2 ? strf("%S", tm) : std::to_string(tm.tm_sec); break;
        case 'a': out += tm.tm_hour < 12 ? "AM" : "PM"; break;
        case 'd': out += n >= 2 ? strf("%d", tm) : std::to_string(tm.tm_mday); break;
        case 'M': out += n >= 4 ? strf("%B", tm) : n == 3 ? strf("%b", tm) : n == 2 ? strf("%m", tm) : std::to_string(tm.tm_mon + 1); break;
        case 'y': out += n == 2 ? strf("%y", tm) : strf("%Y", tm); break;
        default: break;
        }
    }
    return out;
}

AppleClockExtra::AppleClockExtra(const std::string &bundle) : MenuExtra(bundle)
{
    readPreferences();
    setup();
    schedule();
    // Date & Time's Clock tab writes them: -_clockSettingsDidChange:
    m_prefsTimer = App::shared().addTimer(1, true, [this] {
        if (modified(configDir() + "/menuextra.clock.conf") == m_prefsStamp)
            return;
        const bool analog = m_analog;
        readPreferences();
        if (analog != m_analog)
            setup();
        update();
        if (m_timer)
            App::shared().removeTimer(m_timer);
        schedule();
    });
}

void AppleClockExtra::readPreferences()
{
    Settings prefs("menuextra.clock");
    m_prefsStamp = modified(configDir() + "/menuextra.clock.conf");
    // 10.6's default: the day and the time, in the locale's clock
    m_format = prefs.string("DateFormat", twelveHours() ? "EEE h:mm a" : "EEE H:mm");
    m_analog = prefs.boolean("IsAnalog", false);
    m_flash = prefs.boolean("FlashDateSeparators", false);
}

AppleClockExtra::~AppleClockExtra() { willUnload(); }

void AppleClockExtra::willUnload()
{
    for (int *t : {&m_timer, &m_prefsTimer})
        if (*t) {
            App::shared().removeTimer(*t);
            *t = 0;
        }
}

void AppleClockExtra::setup()
{
    if (m_analog)
        setView(new AnalogView(this));
    else
        setView(new DigitalView(this));
    update();
}

std::string AppleClockExtra::text() const
{
    const time_t now = time(nullptr);
    std::string s = formatDate(m_format, now);
    // -_attributedStringByHidingSeparatorsWithDate: on even seconds the separators are hidden (spaces
    // of their width, so that the text doesn't move)
    if (m_flash && now % 2 == 0 && !isMenuDown())
        for (char &c : s)
            if (c == ':')
                c = ' ';
    return s;
}

void AppleClockExtra::update()
{
    // -_resize: the length the cell's size (its text, 2 points each side), the analog one the bar's height
    const double length = m_analog ? SystemUI::MenuBarHeight : std::ceil(textWidth(text(), SystemUI::menuBarFont()) + 4);
    setLength(length);
    setNeedsDisplay();
}

void AppleClockExtra::schedule()
{
    // every second with seconds or flashing separators, else at the next minute
    const bool everySecond = !m_analog && (m_flash || m_format.find('s') != std::string::npos);
    timeval tv;
    gettimeofday(&tv, nullptr);
    const double intoSecond = tv.tv_usec / 1e6;
    std::tm tm;
    localtime_r(&tv.tv_sec, &tm);
    const double wait = everySecond ? 1 - intoSecond : 60 - tm.tm_sec - intoSecond;
    m_timer = App::shared().addTimer(wait + 0.01, false, [this] {
        m_timer = 0;
        update();
        schedule();
    });
}

std::vector<MenuItem> AppleClockExtra::menu()
{
    std::vector<MenuItem> items;
    // -_dateFormatterForMenu: NSDateFormatterFullStyle, the time too (short) when analog
    const time_t now = time(nullptr);
    // the full style's order: English's "Wednesday, November 18, 2026", else the day before the month
    const char *locale = std::setlocale(LC_TIME, nullptr);
    const std::string l = locale ? locale : "C";
    const bool english = l == "C" || l == "POSIX" || l.rfind("en", 0) == 0;
    std::string date = formatDate(english ? "EEEE, MMMM d, y" : "EEEE d MMMM y", now);
    if (m_analog)
        date += " " + formatDate(twelveHours() ? "h:mm a" : "H:mm", now);
    items.emplace_back(date, false, false);
    items.push_back(MenuItem::line());
    MenuItem view(m_analog ? localized("ViewAsDigital", "View as Digital") : localized("ViewAsAnalog", "View as Analog"));
    view.action = [this] {
        m_analog = !m_analog;
        Settings("menuextra.clock").set("IsAnalog", m_analog);
        setup();
        if (m_timer)
            App::shared().removeTimer(m_timer);
        schedule();
    };
    items.push_back(view);
    items.push_back(MenuItem::line());
    MenuItem prefs(localized("OpenDateTimePref", "Open Date & Time…"));
    prefs.action = [] {
        // System Preferences.app beside SystemUIServer.app, on Date & Time
        const std::string exe = dirName(dirName(dirName(dirName(executablePath())))) + "/System Preferences.app/Contents/MacOS/System Preferences";
        Task::launchDetached({exe, "--pane", "datetime"});
    };
    items.push_back(prefs);
    return items;
}

SYSTEMUI_REGISTER_EXTRA("AppleClockExtra", AppleClockExtra);
