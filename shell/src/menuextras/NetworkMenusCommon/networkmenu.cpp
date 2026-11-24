// SPDX-License-Identifier: LGPL-2.1-or-later
// NetworkMenusCommon.framework's AppleNetworkMenuExtra (docs/reverse-status.md, section 34):
//
//   - -initWithBundle: (0x1784): the subclass' images (-loadImages, PDFs as templates), its view
//     (AppleNetworkMenuExtraView, 22 x 22), the services half a second later (-delayedSetup);
//   - -createMenu (0x1fbe): without a service the subclass' "not configured" line, disabled;
//     otherwise each service's header item (-headerMenuItem: "Connect %@", "Disconnect %@" while
//     connecting or connected, "Disconnecting %@" disabled; the time connected at its right when the
//     bar doesn't show it), a line, then each service's name (disabled) over its configurations
//     ("Default", checked; enabled when disconnected or selected) and a line; "Show time
//     connected" (xShowTime, on unless set), or with several services connected a submenu of "Show
//     time for "%@" in menu bar" and "Stop Showing Time in Menu Bar" (xShowSpecificTime, the
//     service's id); "Show status while connecting" (xShowStatus, on unless set); a line; "Open
//     Network Preferences…";
//   - -a_connectOrDisconnect: (0x53b3): disconnected connects, connecting or connected disconnects;
//   - AppleNetworkMenuExtraView: the first image disconnected; connecting or disconnecting the images
//     in turn and, with "Show status while connecting", the status ("Connecting…",
//     "Disconnecting…") scrolling after the image in the system font of 12, the view 80 points wide
//     (-showAnimatedView, -drawAnimated: 0x9318, a frame every 0.02 s, 0.1 s without the status);
//     connected the last image and, with "Show time connected", the time "%2.2d:%2.2d:%2.2d" after
//     it, 5 points apart (-drawConnectedView: 0x9d7a, -getTimeImage:isMenuDown: 0x8862).
//
// Linux: NetworkManager's connections (nmcli), read every two seconds; activating is connecting,
// deactivating disconnecting; nmcli connection up / down. The preferences (com.apple.networkConnect)
// in networkConnect.conf. The view's widening animation (-startSizeTimer:) is left out: it takes its
// width at once.
#include "networkmenu.h"

#include "foundation.h"
#include "runloop.h"

#include <cmath>
#include <cstdio>
#include <map>

using namespace AK;

namespace NetworkMenus {

namespace {

// nmcli -t's fields: ':' between them, "\:" and "\\" inside
std::vector<std::string> fields(const std::string &line)
{
    std::vector<std::string> out(1);
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            out.back() += line[++i];
        } else if (line[i] == ':') {
            out.emplace_back();
        } else {
            out.back() += line[i];
        }
    }
    return out;
}

std::vector<std::vector<std::string>> nmcli(const std::string &args)
{
    std::vector<std::vector<std::string>> rows;
    FILE *p = popen(("nmcli -t " + args + " 2>/dev/null").c_str(), "r");
    if (!p)
        return rows;
    char buf[1024];
    while (fgets(buf, sizeof buf, p)) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            rows.push_back(fields(line));
    }
    pclose(p);
    return rows;
}

// "%2.2d:%2.2d:%2.2d"
std::string timeString(long seconds)
{
    if (seconds < 1)
        seconds = 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%2.2ld:%2.2ld:%2.2ld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    return buf;
}

std::string replaced(std::string f, const std::string &arg)
{
    const size_t at = f.find("%@");
    if (at != std::string::npos)
        f.replace(at, 2, arg);
    return f;
}

// AppleNetworkMenuExtraView
class View : public SystemUI::MenuExtraView {
public:
    explicit View(AppleNetworkMenuExtra *e) : MenuExtraView(e), extra(e) {}
    ~View() override
    {
        if (m_timer)
            App::shared().removeTimer(m_timer);
    }
    AppleNetworkMenuExtra *extra;

    // -drawSelf: the state's view (-showAnimatedView, -showConnectedView, -showNonAnimatedView)
    void update()
    {
        const Service *s = extra->barService();
        const Status st = s ? s->status : Disconnected;
        double width = 22;
        double interval = 0;
        if (st == Connecting || st == Disconnecting) {
            interval = extra->showAnimatedStatus() ? 0.02 : 0.1;
            if (extra->showAnimatedStatus())
                width = 80;   // mMaxWidth
        } else if (st == Connected && extra->showTimer()) {
            interval = 1;   // -startConnectedTimer
            width = 22 + 5 + std::ceil(textWidth(timeString(std::time(nullptr) - s->start), Font::system(12)));
        }
        extra->setLength(width);
        if (interval != m_interval) {
            if (m_timer)
                App::shared().removeTimer(m_timer);
            m_timer = 0;
            m_interval = interval;
            if (interval > 0)
                m_timer = App::shared().addTimer(interval, true, [this] { tick(); });
        }
        setNeedsDisplay();
    }

    void draw(cairo_t *cr) override
    {
        const double w = frame().w, h = frame().h;
        const bool down = extra->isMenuDown();
        if (down)
            drawHighlight(cr, Rect{0, 0, w, h - 1});
        const std::vector<Image> &images = extra->images(down);
        if (images.empty())
            return;
        const Service *s = extra->barService();
        const Status st = s ? s->status : Disconnected;
        size_t index = 0;
        if (st == Connected)
            index = images.size() - 1;
        else if (st == Connecting || st == Disconnecting)
            index = images.size() > 2 ? 1 + m_frame % (images.size() - 2) : images.size() - 1;
        const Image &img = images[std::min(index, images.size() - 1)];
        // the image cell, 22 x 22 at the left
        img.draw(cr, Point{std::floor((22 - img.width()) / 2), std::floor((h - 1 - img.height()) / 2)});
        TextStyle style;
        style.font = Font::system(12);
        style.color = Color::gray(down ? 255 : 0);
        style.elide = false;
        const double th = lineHeight(style.font);
        const double top = std::floor((h - 1 - th) / 2);
        const double x0 = 22 + 5;
        if (st == Connected && extra->showTimer()) {
            drawText(cr, timeString(std::time(nullptr) - s->start), Rect{x0, top, w - x0, th}, style);
        } else if ((st == Connecting || st == Disconnecting) && extra->showAnimatedStatus()) {
            const std::string text = extra->text(st == Connecting ? "SCROLL_CONNECTING" : "SCROLL_DISCONNECTING");
            const double tw = std::floor(textWidth(text, style.font)) + 10;
            cairo_save(cr);
            cairo_rectangle(cr, x0, 0, std::max(0.0, w - x0), h);
            cairo_clip(cr);
            cairo_push_group(cr);
            const double offset = std::fmod(double(m_offset), tw);
            drawText(cr, text, Rect{x0 - offset, top, tw, th}, style);
            if (x0 - offset + tw < w)
                drawText(cr, text, Rect{x0 - offset + tw, top, tw, th}, style);
            if (!down) {
                // its ends faded: two columns at 0.95 and 0.91 at each side
                static const double alpha[] = {0.95, 0.91};
                cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
                for (int i = 0; i < 2; ++i)
                    for (const double x : {x0 + i, w - i - 1}) {
                        cairo_save(cr);
                        cairo_rectangle(cr, x, top, 1, th);
                        cairo_clip(cr);
                        cairo_set_source_rgba(cr, 1, 1, 1, alpha[i]);
                        cairo_paint(cr);
                        cairo_restore(cr);
                    }
            }
            cairo_pop_group_to_source(cr);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }

private:
    void tick()
    {
        ++m_offset;   // _offset: a point a frame
        if (m_offset % 5 == 0 || !extra->showAnimatedStatus())
            ++m_frame;
        setNeedsDisplay();
    }

    int m_timer = 0;
    double m_interval = 0;
    long m_offset = 0;
    size_t m_frame = 0;
};

} // namespace

AppleNetworkMenuExtra::AppleNetworkMenuExtra(const std::string &bundle) : MenuExtra(bundle)
{
    setView(new View(this));
    // -delayedSetup: the services half a second later, then on the dynamic store's changes
    m_poll = App::shared().addTimer(0.5, false, [this] {
        m_poll = App::shared().addTimer(2, true, [this] { refresh(); });
        refresh();
    });
}

AppleNetworkMenuExtra::~AppleNetworkMenuExtra()
{
    if (m_poll)
        App::shared().removeTimer(m_poll);
}

const Service *AppleNetworkMenuExtra::barService() const
{
    for (const Service &s : m_services)
        if (s.uuid == m_barUuid)
            return &s;
    return nullptr;
}

void AppleNetworkMenuExtra::refresh()
{
    if (m_images.empty())
        for (const std::string &name : imageNameArray()) {   // -loadImages
            m_images.push_back(templateImage(name));
            m_white.push_back(templateImage(name, true));
        }
    std::map<std::string, Service> old;
    for (const Service &s : m_services)
        old[s.uuid] = s;
    std::map<std::string, std::string> active;
    for (const auto &row : nmcli("-f UUID,STATE connection show --active"))
        if (row.size() >= 2)
            active[row[0]] = row[1];
    m_services.clear();
    for (const auto &row : nmcli("-f NAME,UUID,TYPE connection show")) {
        if (row.size() < 3 || !acceptsType(row[2]))
            continue;
        Service s;
        s.name = row[0];
        s.uuid = row[1];
        s.type = row[2];
        const auto a = active.find(s.uuid);
        const std::string state = a == active.end() ? std::string() : a->second;
        s.status = state == "activated" ? Connected : state == "activating" ? Connecting : state == "deactivating" ? Disconnecting : Disconnected;
        const auto o = old.find(s.uuid);
        if (s.status == Connected)
            s.start = o != old.end() && o->second.status == Connected && o->second.start ? o->second.start : std::time(nullptr);
        m_services.push_back(s);
    }
    prefsChanged();
}

// the preferences and which service the bar follows (-createMenu's and -timeMenuForServiceControllers:'s)
void AppleNetworkMenuExtra::prefsChanged()
{
    Settings prefs("networkConnect");
    m_showStatus = prefs.boolean(keyForShowStatus(), true);
    const bool showTime = prefs.boolean(keyForShowTime(), true);
    int connected = 0;
    for (const Service &s : m_services)
        connected += s.status != Disconnected;
    m_barUuid.clear();
    m_showTimer = false;
    if (connected >= 2) {
        const std::string specific = prefs.string(keyForShowSpecificTime());
        for (Service &s : m_services) {
            s.shouldShowTimer = true;
            if (!specific.empty() && s.uuid == specific && s.status != Disconnected) {
                m_barUuid = s.uuid;
                m_showTimer = true;
                s.shouldShowTimer = false;
            }
        }
        if (m_barUuid.empty())
            for (const Service &s : m_services)
                if (s.status != Disconnected) {
                    m_barUuid = s.uuid;   // the status shown for the first one
                    break;
                }
    } else {
        for (Service &s : m_services) {
            s.shouldShowTimer = !showTime;
            if (s.status != Disconnected)
                m_barUuid = s.uuid;
        }
        m_showTimer = showTime;
    }
    if (m_barUuid.empty() && !m_services.empty())
        m_barUuid = m_services.front().uuid;
    static_cast<View *>(view())->update();
}

void AppleNetworkMenuExtra::storeValue(const std::string &key, const std::string &value)
{
    Settings prefs("networkConnect");
    prefs.set(key, value);
}

std::vector<MenuItem> AppleNetworkMenuExtra::menu()
{
    refresh();
    std::vector<MenuItem> m;
    if (m_services.empty()) {
        MenuItem none{text(noConfigsKey())};   // tag 0x37, noopAction:
        none.enabled = false;
        m.push_back(none);
    } else {
        // -headerMenuItem, -refreshHeaderView
        for (const Service &s : m_services) {
            MenuItem header;
            const char *fmt = s.status == Disconnected ? "NOT_CONNECTED_FORMAT" : s.status == Disconnecting ? "DISCONNECTING_FORMAT" : "CONNECTED_FORMAT";
            header.title = replaced(text(fmt), s.name);
            header.enabled = s.status != Disconnecting;
            if (s.status == Connected && s.shouldShowTimer)
                header.keyEquivalent = timeString(std::time(nullptr) - s.start);   // AppleNetworkMenuHeaderView's time
            const std::string uuid = s.uuid;
            const Status st = s.status;
            header.action = [uuid, st] {
                // -a_connectOrDisconnect:
                if (st == Disconnected)
                    Task::launchDetached({"nmcli", "connection", "up", "uuid", uuid});
                else if (st == Connecting || st == Connected)
                    Task::launchDetached({"nmcli", "connection", "down", "uuid", uuid});
            };
            m.push_back(header);
        }
        m.push_back(MenuItem::line());
        for (const Service &s : m_services) {
            MenuItem name{s.name};
            name.enabled = false;
            m.push_back(name);
            // -refreshMenuItems: the configurations; a NetworkManager connection has one, "Default"
            MenuItem def{text("DEFAULT")};
            def.checked = true;
            def.enabled = true;
            m.push_back(def);
            m.push_back(MenuItem::line());
        }
        Settings prefs("networkConnect");
        int connected = 0;
        for (const Service &s : m_services)
            connected += s.status == Connected;
        MenuItem showTime{text("SHOW_TIMER")};
        if (connected >= 2) {
            // -timeMenuForServiceControllers:
            const std::string specific = prefs.string(keyForShowSpecificTime());
            for (const Service &s : m_services) {
                if (s.status != Connected)
                    continue;
                MenuItem one{replaced(text("SHOW_TIME_FORMAT"), s.name)};
                one.checked = s.uuid == specific;
                const std::string key = keyForShowSpecificTime(), uuid = s.uuid;
                one.action = [this, key, uuid] {
                    storeValue(key, uuid);
                    prefsChanged();
                };
                showTime.submenu.push_back(one);
            }
            showTime.checked = !specific.empty();
            showTime.submenu.push_back(MenuItem::line());
            MenuItem stop{text("STOP_SHOWING_TIME")};
            const std::string key = keyForShowSpecificTime();
            stop.action = [this, key] {
                storeValue(key, "");
                prefsChanged();
            };
            showTime.submenu.push_back(stop);
        } else {
            showTime.checked = prefs.boolean(keyForShowTime(), true);
            const std::string key = keyForShowTime();
            const bool on = showTime.checked;
            showTime.action = [this, key, on] {   // -a_showTimeInMenuBar:
                storeValue(key, on ? "false" : "true");
                prefsChanged();
            };
        }
        m.push_back(showTime);
        MenuItem showStatus{text("SHOW_ANIMATED_STATUS")};
        showStatus.checked = prefs.boolean(keyForShowStatus(), true);
        const std::string key = keyForShowStatus();
        const bool on = showStatus.checked;
        showStatus.action = [this, key, on] {   // -a_showStatusInMenuBar:
            storeValue(key, on ? "false" : "true");
            prefsChanged();
        };
        m.push_back(showStatus);
    }
    m.push_back(MenuItem::line());
    MenuItem prefsItem{text("OPEN_NP")};
    prefsItem.action = [] { openPreferencePane("network"); };   // -a_launchNetworkPrefs:
    m.push_back(prefsItem);
    return m;
}

} // namespace NetworkMenus
