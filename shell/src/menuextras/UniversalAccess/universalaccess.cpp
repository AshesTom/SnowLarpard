// SPDX-License-Identifier: LGPL-2.1-or-later
// UniversalAccess.menu's code (UAMenu) on AquaKit's SystemUIPlugin, loaded by SystemUIServer as on the
// Mac. Reversed from 10.6's UniversalAccess.menu, x86_64, with REA (docs/reverse-status.md, section 32):
//
//   - -initWithBundle: (0x2c9a): -setupUAMenuView: animation supported (an NSMenuExtraAnimationView),
//     UniversalAccess.pdf as a template, sized to the view's height less 4 points, square;
//   - -setupUAMenu (0x1b87, again each time the menu is asked for, -menu 0x2d01): a disabled line a
//     setting, on or off: Cursor Magnification (CGSGetCursorScale over 1), Black on White / White on
//     Black (CGDisplayUsesInvertedPolarity), Flash Screen (the audio services' 'avon'), Mouse Keys
//     (mouseDriver), Slow Keys (slowKey), Sticky Keys (stickyKey), VoiceOver (voiceOverOnOffKey), Zoom
//     (closeViewDriver), the last five com.apple.universalaccess's; a line; Universal Access
//     Preferences… (UniversalAccessPref.prefPane);
//   - -handleNotification: (0x29a0), -updateStatus: (0x24bd): a change of the keyboard's, the
//     mouse's or Zoom's settings (UniversalAccessDomain…SettingsDidChangeNotification, the changed keys
//     in its userInfo) shows the new state in the bar for 15 seconds, "%@…" each, joined by " %@"
//     (-setAnimationText:, -startTextAnimation), then -stopTextAnimation.
//
// On Linux: Mouse Keys, Slow Keys and Sticky Keys are XKB's controls (the X server's AccessX), read on
// the display; the others universalaccess.conf's keys (mouseDriverCursorSize, whiteOnBlack,
// flashScreen, voiceOverOnOffKey, closeViewDriver), VoiceOver also on while Orca runs. Read every
// second, a change standing for the notification.
#include "systemuiplugin.h"

#include "foundation.h"

#include <xcb/xcb.h>
// xkb.h names a member "explicit", a C++ keyword
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit

#include <dirent.h>

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>

using namespace AK;

namespace {

// XKB's enabled controls, on a connection of the extra's own
struct Xkb {
    xcb_connection_t *c = nullptr;
    bool ok = false;
    Xkb()
    {
        c = xcb_connect(nullptr, nullptr);
        if (xcb_connection_has_error(c))
            return;
        xcb_xkb_use_extension_reply_t *r =
            xcb_xkb_use_extension_reply(c, xcb_xkb_use_extension(c, XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION), nullptr);
        ok = r && r->supported;
        free(r);
    }
    ~Xkb()
    {
        if (c)
            xcb_disconnect(c);
    }
    uint32_t enabled() const
    {
        if (!ok)
            return 0;
        xcb_xkb_get_controls_reply_t *r = xcb_xkb_get_controls_reply(c, xcb_xkb_get_controls(c, XCB_XKB_ID_USE_CORE_KBD), nullptr);
        const uint32_t e = r ? r->enabledControls : 0;
        free(r);
        return e;
    }
};

bool orcaRunning()
{
    DIR *d = opendir("/proc");
    if (!d)
        return false;
    bool found = false;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        std::ifstream comm(std::string("/proc/") + e->d_name + "/comm");
        std::string name;
        if (std::getline(comm, name) && name == "orca") {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

} // namespace

class UAMenu : public SystemUI::MenuExtra {
public:
    explicit UAMenu(const std::string &bundle) : MenuExtra(bundle)
    {
        // -setupUAMenuView
        setSupportsAnimation(true);
        Image img = Image::load(resourcePath("UniversalAccess.pdf"), {64, 64});
        const int side = int(SystemUI::MenuBarHeight - 4);
        if (!img.isNull())
            setImage(img.scaled(side, side));
        view();
        m_last = keys();
        // -registerForNotifications: the settings read again every second
        m_poll = App::shared().addTimer(1, true, [this] { poll(); });
    }
    ~UAMenu() override
    {
        App::shared().removeTimer(m_poll);
        if (m_statusTimer)
            App::shared().removeTimer(m_statusTimer);
    }

    // -menu: -setupUAMenu again
    std::vector<MenuItem> menu() override
    {
        std::vector<MenuItem> m;
        auto line = [&](bool on, const char *yes, const char *no) {
            MenuItem item{tr(on ? yes : no)};
            item.enabled = false;
            m.push_back(item);
        };
        Settings ua("universalaccess");
        const auto k = keys();
        line(std::stod("0" + ua.string("mouseDriverCursorSize", "1")) > 1, "CURSOR_MAG_ON", "CURSOR_MAG_OFF");
        line(ua.boolean("whiteOnBlack"), "DISPLAY_WB", "DISPLAY_BW");
        line(ua.boolean("flashScreen"), "FLASH_SCREEN_ON", "FLASH_SCREEN_OFF");
        line(k.at("mouseDriver"), "MOUSE_KEYS_ON", "MOUSE_KEYS_OFF");
        line(k.at("slowKey"), "SLOW_KEYS_ON", "SLOW_KEYS_OFF");
        line(k.at("stickyKey"), "STICKY_KEYS_ON", "STICKY_KEYS_OFF");
        line(k.at("voiceOverOnOffKey"), "VOICEOVER_ON", "VOICEOVER_OFF");
        line(k.at("closeViewDriver"), "ZOOM_ON", "ZOOM_OFF");
        m.push_back(MenuItem::line());
        MenuItem prefs{tr("UA_PREFERENCES")};
        prefs.action = [] { openPreferencePane("com.apple.preference.universalaccess"); };
        m.push_back(prefs);
        return m;
    }

private:
    std::string tr(const std::string &key) const { return localized(key, key); }

    // the five keys the notifications carry
    std::map<std::string, bool> keys() const
    {
        Settings ua("universalaccess");
        const uint32_t xkb = m_xkb.enabled();
        return {{"mouseDriver", (xkb & XCB_XKB_BOOL_CTRL_MOUSE_KEYS) != 0},
                {"slowKey", (xkb & XCB_XKB_BOOL_CTRL_SLOW_KEYS) != 0},
                {"stickyKey", (xkb & XCB_XKB_BOOL_CTRL_STICKY_KEYS) != 0},
                {"voiceOverOnOffKey", ua.boolean("voiceOverOnOffKey") || orcaRunning()},
                {"closeViewDriver", ua.boolean("closeViewDriver")}};
    }

    void poll()
    {
        const auto now = keys();
        bool changed = false;
        for (const auto &[key, on] : now)
            if (m_last[key] != on) {
                handleChange(key, on);
                changed = true;
            }
        m_last = now;
        if (changed)
            updateStatus();
    }

    // -handleNotification: a key's status string (-stringFromValue:forKey:), shown until 15 s from now
    void handleChange(const std::string &key, bool on)
    {
        static const std::map<std::string, std::pair<const char *, const char *>> names = {
            {"mouseDriver", {"MOUSE_KEYS_ON", "MOUSE_KEYS_OFF"}}, {"slowKey", {"SLOW_KEYS_ON", "SLOW_KEYS_OFF"}},
            {"stickyKey", {"STICKY_KEYS_ON", "STICKY_KEYS_OFF"}},  {"voiceOverOnOffKey", {"VOICEOVER_ON", "VOICEOVER_OFF"}},
            {"closeViewDriver", {"ZOOM_ON", "ZOOM_OFF"}}};
        const auto it = names.find(key);
        if (it == names.end())
            return;
        m_status[key] = {tr(on ? it->second.first : it->second.second), std::time(nullptr) + 15};
    }

    // -updateStatus: the statuses not yet over, in the bar; the timer set to the earliest end
    void updateStatus()
    {
        const std::time_t now = std::time(nullptr);
        std::string text;
        std::time_t earliest = 0;
        for (auto it = m_status.begin(); it != m_status.end();) {
            if (it->second.second <= now) {
                it = m_status.erase(it);
                continue;
            }
            const std::string one = format(tr("STATUS_STRING"), it->second.first);
            text = text.empty() ? one : text + " " + one;   // stringByAppendingFormat:@" %@"
            earliest = earliest ? std::min(earliest, it->second.second) : it->second.second;
            ++it;
        }
        if (text.empty()) {
            stopTextAnimation();
        } else {
            setAnimationText(text);
            startTextAnimation();
        }
        if (m_statusTimer)
            App::shared().removeTimer(m_statusTimer);
        m_statusTimer = 0;
        if (earliest)
            m_statusTimer = App::shared().addTimer(double(earliest - now), false, [this] {
                m_statusTimer = 0;
                updateStatus();
            });
    }

    static std::string format(std::string f, const std::string &arg)
    {
        const size_t at = f.find("%@");
        if (at != std::string::npos)
            f.replace(at, 2, arg);
        return f;
    }

    Xkb m_xkb;
    std::map<std::string, bool> m_last;
    std::map<std::string, std::pair<std::string, std::time_t>> m_status;   // _statusDict: Status, EndTime
    int m_poll = 0, m_statusTimer = 0;
};

SYSTEMUI_REGISTER_EXTRA("UAMenu", UAMenu);
