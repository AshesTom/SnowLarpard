// SPDX-License-Identifier: LGPL-2.1-or-later
#include "logout.h"

#include <spawn.h>

#include "controls.h"
#include "foundation.h"
#include "nib.h"
#include "panels.h"

#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace AK;

namespace LoginWindow {

namespace {

const char *const Bundle = "/System/Library/CoreServices/loginwindow.app";

std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "loginwindow", key, fallback); }

std::string replaceAll(std::string s, const std::string &what, const std::string &with)
{
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
        s.replace(at, what.size(), with);
    return s;
}

// -[NSString stringWithFormat:] for the strings' own formats: "%d", "%1$d" / "%2$d", "%@"
std::string format(std::string f, long a, long b = 0)
{
    f = replaceAll(f, "%1$d", std::to_string(a));
    f = replaceAll(f, "%2$d", std::to_string(b));
    const size_t d = f.find("%d");
    if (d != std::string::npos)
        f.replace(d, 2, std::to_string(a));
    return f;
}

// ---- the applications and the session, through the window manager (an X session's)

struct Display {
    xcb_connection_t *conn = nullptr;
    xcb_window_t root = 0;
    Display()
    {
        int screen = 0;
        conn = xcb_connect(nullptr, &screen);
        if (xcb_connection_has_error(conn)) {
            xcb_disconnect(conn);
            conn = nullptr;
            return;
        }
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
        for (int i = 0; i < screen; ++i)
            xcb_screen_next(&it);
        root = it.data->root;
    }
    ~Display()
    {
        if (conn)
            xcb_disconnect(conn);
    }
    xcb_atom_t atom(const char *name) const
    {
        auto *r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, uint16_t(std::strlen(name)), name), nullptr);
        const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
        free(r);
        return a;
    }
    std::vector<uint32_t> cardinals(xcb_window_t w, xcb_atom_t property, xcb_atom_t type) const
    {
        std::vector<uint32_t> out;
        auto *r = xcb_get_property_reply(conn, xcb_get_property(conn, 0, w, property, type, 0, 4096), nullptr);
        if (r && r->format == 32) {
            const auto *v = static_cast<const uint32_t *>(xcb_get_property_value(r));
            out.assign(v, v + xcb_get_property_value_length(r) / 4);
        }
        free(r);
        return out;
    }
    std::string wmClass(xcb_window_t w) const
    {
        auto *r = xcb_get_property_reply(conn, xcb_get_property(conn, 0, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 256), nullptr);
        std::string out;
        if (r) {
            const char *v = static_cast<const char *>(xcb_get_property_value(r));
            const int n = xcb_get_property_value_length(r);
            const size_t first = strnlen(v, size_t(n));
            if (int(first) + 1 < n)
                out.assign(v + first + 1, strnlen(v + first + 1, size_t(n) - first - 1));   // the class
            else
                out.assign(v, first);
        }
        free(r);
        return out;
    }
    // The applications' windows: the window manager's list, without the session's own (the menu bar,
    // the Dock, the desktop: docks and desktops) nor this program's
    std::vector<xcb_window_t> applicationWindows() const
    {
        std::vector<xcb_window_t> out;
        const xcb_atom_t dock = atom("_NET_WM_WINDOW_TYPE_DOCK"), desktop = atom("_NET_WM_WINDOW_TYPE_DESKTOP");
        const xcb_atom_t pid = atom("_NET_WM_PID");
        for (uint32_t w : cardinals(root, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW)) {
            const std::vector<uint32_t> types = cardinals(w, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM);
            if (std::find(types.begin(), types.end(), dock) != types.end() || std::find(types.begin(), types.end(), desktop) != types.end())
                continue;
            const std::vector<uint32_t> p = cardinals(w, pid, XCB_ATOM_CARDINAL);
            if (!p.empty() && pid_t(p[0]) == getpid())
                continue;
            out.push_back(w);
        }
        return out;
    }
    // The application asked to quit by its windows: _NET_CLOSE_WINDOW, as the close button (from a
    // pager: source 2)
    void close(xcb_window_t w) const
    {
        xcb_client_message_event_t ev = {};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = w;
        ev.type = atom("_NET_CLOSE_WINDOW");
        ev.data.data32[0] = XCB_CURRENT_TIME;
        ev.data.data32[1] = 2;
        xcb_send_event(conn, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&ev));
        xcb_flush(conn);
    }
};

std::string environValue(pid_t pid, const std::string &name)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
    std::string var;
    while (std::getline(in, var, '\0'))
        if (var.compare(0, name.size() + 1, name + "=") == 0)
            return var.substr(name.size() + 1);
    return {};
}

std::string exeName(pid_t pid)
{
    char exe[4096];
    const ssize_t n = readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), exe, sizeof exe - 1);
    if (n <= 0)
        return {};
    exe[n] = 0;
    const char *base = std::strrchr(exe, '/');
    return base ? base + 1 : exe;
}

// The session's SystemUIServer: the session (session/snowleopard-session) ends with it. Ours, on this
// display: another session of the same user (a window of sl.sh) keeps its own
pid_t sessionLeader()
{
    const char *display = std::getenv("DISPLAY");
    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;
    pid_t found = 0;
    while (dirent *e = readdir(proc)) {
        const pid_t pid = pid_t(std::atoi(e->d_name));
        if (pid <= 0)
            continue;
        if (exeName(pid) != "SystemUIServer" || kill(pid, 0) != 0)
            continue;
        if (environValue(pid, "DISPLAY") == (display ? display : "")) {
            found = pid;
            break;
        }
    }
    closedir(proc);
    return found;
}

// The applications to quit: the windows' processes, and AquaKit's on this display without a window
// (their menus' sockets, "@snowleopard-menus-<pid>"); not the session's own programs
std::vector<pid_t> applicationProcesses(const Display &x, const std::vector<xcb_window_t> &windows)
{
    std::vector<pid_t> out;
    auto add = [&](pid_t pid) {
        static const char *const session[] = {"SystemUIServer", "Dock", "Finder", "WindowServer", "loginwindow"};
        if (pid <= 1 || pid == getpid() || std::find(out.begin(), out.end(), pid) != out.end())
            return;
        const std::string name = exeName(pid);
        if (name.empty() || std::find(std::begin(session), std::end(session), name) != std::end(session))
            return;
        out.push_back(pid);
    };
    if (x.conn) {
        const xcb_atom_t pid = x.atom("_NET_WM_PID");
        for (xcb_window_t w : windows)
            for (uint32_t p : x.cardinals(w, pid, XCB_ATOM_CARDINAL))
                add(pid_t(p));
    }
    const char *display = std::getenv("DISPLAY");
    std::ifstream sockets("/proc/net/unix");
    std::string line;
    while (std::getline(sockets, line)) {
        const size_t at = line.find("@snowleopard-menus-");
        if (at == std::string::npos)
            continue;
        const pid_t pid = pid_t(std::atoi(line.c_str() + at + 19));
        if (pid > 0 && kill(pid, 0) == 0 && environValue(pid, "DISPLAY") == (display ? display : ""))
            add(pid);
    }
    return out;
}

void endSession()
{
    if (const pid_t pid = sessionLeader())
        kill(pid, SIGTERM);
    else
        std::fprintf(stderr, "loginwindow: no SystemUIServer on this display: no session to end\n");
}

// displayCurrentCountdownTime's switch on the TIME_FORMAT_ index: the seconds (2, 12), the minutes
// (20, 21), both (22), none (the others: "1 minute", "0 seconds"...)
std::string countdownTime(long index, const std::string &format_, long countdown)
{
    switch (index) {
    case 2:
    case 12: return format(format_, countdown % 60);
    case 20:
    case 21: return format(format_, countdown / 60);
    case 22: return format(format_, countdown / 60, countdown % 60);
    default: return format_;
    }
}

// ---- -[LogOutAlert showLogoutPanel]

class LogOutAlert {
public:
    bool load()
    {
        const std::string res = Assets::path(std::string(Bundle) + "/Contents/Resources");
        if (!m_nib.load(res + "/English.lproj/LogOut.nib/keyedobjects.nib", res))
            return false;
        m_content = m_nib.instantiate("logoutPanel");
        if (!m_content)
            return false;
        m_size = m_nib.size();
        m_message = m_nib.outlet<Label>("logoutMessageField");
        m_timedOut = m_nib.outlet<Label>("logoutTimedOutField");
        m_guest = m_nib.outlet<Label>("guestAccountMessageField");
        m_default = m_nib.outlet<Button>("defaultButton");
        m_cancel = m_nib.outlet<Button>("cancelButton");
        return m_message && m_timedOut && m_guest && m_default && m_cancel;
    }

    // Not a guest account (Linux has none): the guest's message hidden, the countdown shown
    void show(App &app, std::function<void(bool)> done)
    {
        m_done = std::move(done);
        // the space between the message and the guest's message (DAT_1000907f0), kept under the message
        m_gap = m_guest->frame().y - m_message->frame().bottom();
        m_message->setText(tr("LOGOUT_MESSAGE", m_message->text()));
        displayCurrentCountdownTime();
        m_guest->setHidden(true);
        sizeWindowToFit();

        // the nib's NSPanel, style 129 (titled, non-activating: no buttons), no title, at
        // CGWindowLevelForKey(kCGScreenSaverWindowLevelKey) (13), centred, key
        m_window = std::make_unique<Window>(m_size, "");
        m_window->contentView()->addSubview(m_content);
        m_window->setStyleMask(129);
        m_window->setFloatingPanel(false);
        m_window->onClose = [this] { finish(false); };
        m_window->onUnhandledKey = [this](Event &e) {
            if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter)
                finish(true);
            else if (e.keysym == XKB_KEY_Escape)
                finish(false);
        };
        m_default->action = [this] { finish(true); };   // logout: (the Return key's)
        m_cancel->action = [this] { finish(false); };   // cancel: (Escape's)
        m_window->show();
        // countDownToLogout:, every second; at 0, logout:
        m_timer = app.addTimer(1.0, true, [this] {
            if (--m_countdown > 0)
                displayCurrentCountdownTime();
            else
                finish(true);
        });
        m_app = &app;
    }

private:
    // -[LogOutAlert updateTimeFormatString]: TIME_FORMAT_<minutes, at most 2><seconds, at most 2>
    void updateTimeFormatString()
    {
        const long index = std::min(m_countdown / 60, 2L) * 10 + std::min(m_countdown % 60, 2L);
        if (index == m_formatIndex)
            return;
        m_timeFormat = tr("TIME_FORMAT_" + std::to_string(index), "%d seconds");
        m_formatIndex = index;
    }

    // -[LogOutAlert displayCurrentCountdownTime]: WILL_LOGOUT_INFO with the time left
    void displayCurrentCountdownTime()
    {
        if (m_base.empty())   // loadBaseFormatString
            m_base = tr("WILL_LOGOUT_INFO", "If you do nothing, you will be logged out automatically in %@.");
        updateTimeFormatString();
        m_timedOut->setText(replaceAll(m_base, "%@", countdownTime(m_formatIndex, m_timeFormat, m_countdown)));
    }

    // -[LogOutAlert sizeWindowToFit], a user's account: the panel's height less the guest's message and
    // the space over it, and the countdown's field to its text's height ([NSCell cellSizeForBounds:],
    // the cell's 2 points each side); the fields under the message (the countdown, the buttons) are
    // held at the bottom (NSViewMaxYMargin), the others at the top
    void sizeWindowToFit()
    {
        const Rect timed = m_timedOut->frame();
        const double text = std::ceil(wrappedHeight(m_timedOut->text(), timed.w - 4, m_timedOut->style.font));
        const double change = (text - timed.h) - (m_guest->frame().h + m_gap);
        m_size.h += change;
        m_content->setFrame({0, 0, m_size.w, m_size.h});
        for (View *v : {static_cast<View *>(m_timedOut), static_cast<View *>(m_default), static_cast<View *>(m_cancel)}) {
            const Rect f = v->frame();
            v->setFrame({f.x, f.y + change, f.w, f.h});
        }
    }

    void finish(bool logOut)
    {
        if (m_finished)
            return;
        m_finished = true;
        if (m_app && m_timer)
            m_app->removeTimer(m_timer);
        m_timer = 0;
        if (m_window) {
            m_window->onClose = nullptr;
            m_window->close();
        }
        if (m_done)
            m_done(logOut);
    }

    Nib m_nib;
    View *m_content = nullptr;
    Size m_size;
    Label *m_message = nullptr, *m_timedOut = nullptr, *m_guest = nullptr;
    Button *m_default = nullptr, *m_cancel = nullptr;
    std::unique_ptr<Window> m_window;
    App *m_app = nullptr;
    int m_timer = 0;
    double m_gap = 8;
    long m_countdown = 60;   // DAT_1000903e8's 60 seconds
    long m_formatIndex = -1;
    std::string m_base, m_timeFormat;
    std::function<void(bool)> m_done;
    bool m_finished = false;
};

// ---- -[ShutDownAlert showPanel] (0x1000354a3): ShutDownAlert.nib, the Apple menu's Shut Down… and
// Restart… (ApplicationManager -postShutdownDialogRequest, -postRestartDialogRequest: the shared
// alerts of type 1 and 2) and the power key's (-sharedPowerAlert, type 0); the scheduled ones (3 shut
// down, 4 sleep, 5 restart: Energy Saver's schedule) are the same panel with their strings

enum class PowerAction { Cancel, ShutDown, Restart, Sleep };

class ShutDownAlert {
public:
    enum Type { Power = 0, ShutDown = 1, Restart = 2, ScheduledShutDown = 3, ScheduledSleep = 4, ScheduledRestart = 5 };
    explicit ShutDownAlert(Type type) : m_type(type) {}

    bool load()
    {
        const std::string res = Assets::path(std::string(Bundle) + "/Contents/Resources");
        if (!m_nib.load(res + "/English.lproj/ShutDownAlert.nib/keyedobjects.nib", res))
            return false;
        m_content = m_nib.instantiate("shutdownPanel");
        if (!m_content)
            return false;
        m_size = m_nib.size();
        m_image = m_nib.outlet<ImageView>("_alertImageView");
        m_info = m_nib.outlet<Label>("_infoText");
        m_timedOut = m_nib.outlet<Label>("_timedOutField");
        m_guest = m_nib.outlet<Label>("_guestAccountMessageField");
        m_default = m_nib.outlet<Button>("_defaultButton");
        m_cancel = m_nib.outlet<Button>("_cancelButton");
        m_restart = m_nib.outlet<Button>("_restartButton");
        m_sleep = m_nib.outlet<Button>("_sleepButton");
        return m_info && m_timedOut && m_guest && m_default && m_cancel;
    }

    void show(App &app, std::function<void(PowerAction)> done)
    {
        m_done = std::move(done);
        m_app = &app;
        // the space between the message and the guest's message (DAT_100090798)
        m_gap = m_guest->frame().y - m_info->frame().bottom();
        const std::string res = Assets::path(std::string(Bundle) + "/Contents/Resources/");
        std::string button;
        switch (m_type) {
        case Power:
            // the managed preferences RestartDisabledWhileLoggedIn and ShutDownDisabledWhileLoggedIn
            // (none here) would take the Restart and Shut Down buttons away; no countdown
            m_timedOut->setText("");
            m_timedOut->setHidden(true);
            button = tr("SHUTDOWN_BUTTON", "Shut Down");
            break;
        case ShutDown:
        case ScheduledShutDown:
            removeButton(m_sleep);
            removeButton(m_restart);
            button = tr("SHUTDOWN_BUTTON", "Shut Down");   // the nib's ShutDown picture kept
            break;
        case Restart:
        case ScheduledRestart:
            removeButton(m_sleep);
            removeButton(m_restart);
            setImage(res + "Restart.tif");
            button = tr("RESTART_BUTTON", "Restart");
            break;
        case ScheduledSleep:
            removeButton(m_sleep);
            removeButton(m_restart);
            setImage(res + "Sleep.tif");
            button = tr("SLEEP_BUTTON", "Sleep");
            break;
        }
        renameDefaultButtonAndResetUI(button);
        sizeWindowToFit();
        if (m_type != Power)
            displayCurrentCountdownTime();

        // the nib's panel, at CGWindowLevelForKey(13) while logged in, centred, key
        m_window = std::make_unique<Window>(m_size, "");
        m_window->contentView()->addSubview(m_content);
        m_window->setStyleMask(129);
        m_window->setFloatingPanel(false);
        m_window->onClose = [this] { finish(PowerAction::Cancel); };
        m_window->onUnhandledKey = [this](Event &e) {
            if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter)
                doDefaultAction();
            else if (e.keysym == XKB_KEY_Escape)
                finish(PowerAction::Cancel);
        };
        m_default->action = [this] { doDefaultAction(); };
        m_cancel->action = [this] { finish(PowerAction::Cancel); };
        if (m_restart)
            m_restart->action = [this] { finish(PowerAction::Restart); };   // restart: ('rest')
        if (m_sleep)
            m_sleep->action = [this] { finish(PowerAction::Sleep); };       // sleep: (Sleep())
        m_window->show();
        // countDownToAction:, every second (0x100067b20: 1.0), but for the power key's
        if (m_type != Power)
            m_timer = app.addTimer(1.0, true, [this] {
                if (--m_countdown < 1)
                    doDefaultAction();
                else
                    displayCurrentCountdownTime();
            });
    }

private:
    void removeButton(Button *&b)
    {
        if (!b)
            return;
        b->setHidden(true);   // -removeFromSuperview
        b = nullptr;
    }

    void setImage(const std::string &path)
    {
        const Image img = Image::load(path);
        if (m_image && !img.isNull()) {
            m_image->image = img;
            m_image->setNeedsDisplay();
        }
    }

    // -renameDefaultButtonAndResetUI: (0x100035fe4), a user's account (not the guest's): the guest's
    // message hidden; the message for the type (SHUTDOWN_MESSAGE, RESTART_MESSAGE, the scheduled
    // ones); the default button retitled and sized to fit, its right edge kept, Cancel moved to keep
    // the space between them
    void renameDefaultButtonAndResetUI(const std::string &title)
    {
        m_guest->setHidden(true);
        std::string message;
        switch (m_type) {
        case Restart: message = tr("RESTART_MESSAGE", "Are you sure you want to restart your computer now?"); break;
        case ScheduledShutDown: message = tr("SCHEDULED_SHUTDOWN_MESSAGE", "This computer has been scheduled to shut down automatically."); break;
        case ScheduledSleep: message = tr("SCHEDULED_SLEEP_MESSAGE", "This computer has been scheduled to go to sleep automatically."); break;
        case ScheduledRestart: message = tr("SCHEDULED_RESTART_MESSAGE", "This computer has been scheduled to restart automatically."); break;
        default: message = tr("SHUTDOWN_MESSAGE", "Are you sure you want to shut down your computer now?"); break;
        }
        m_info->setText(message);
        const Rect before = m_default->frame(), cancel = m_cancel->frame();
        // -sizeToFit: the title's width changes the button's by as much (the nib's own padding kept)
        const double grow = std::ceil(textWidth(title, m_default->font)) - std::ceil(textWidth(m_default->title, m_default->font));
        m_default->title = title;
        Rect after{before.x - grow, before.y, before.w + grow, before.h};   // NSOffsetRect: the same right edge
        m_default->setFrame(after);
        const double space = before.x - cancel.right();
        m_cancel->setFrame({after.x - space - cancel.w, cancel.y, cancel.w, cancel.h});
    }

    // -sizeWindowToFit (0x100035dee), a user's account: the panel's height changes by the countdown's
    // text height ([NSCell cellSizeForBounds:]) less its field's, less the guest's message and the
    // space over it; what is under the message (the countdown, the buttons) held at the bottom
    void sizeWindowToFit()
    {
        const Rect timed = m_timedOut->frame();
        const std::string text = m_timedOut->text();
        // (an empty string's cell is still a line high: the power key's panel)
        const double h = std::ceil(wrappedHeight(text.empty() ? std::string(" ") : text, timed.w - 4, m_timedOut->style.font));
        const double change = (h - timed.h) - (m_guest->frame().h + m_gap);
        m_size.h += change;
        m_content->setFrame({0, 0, m_size.w, m_size.h});
        const double below = m_info->frame().bottom();
        for (View *v : m_content->subviews()) {
            if (v == m_info || v == m_guest || v->frame().y < below)
                continue;
            const Rect f = v->frame();
            v->setFrame({f.x, f.y + change, f.w, f.h});
        }
    }

    // -loadBaseFormatString (0x100035038) by type, -updateTimeFormatString, -displayCurrentCountdownTime
    void displayCurrentCountdownTime()
    {
        if (m_base.empty()) {
            switch (m_type) {
            case Restart: m_base = tr("WILL_RESTART_INFO", "If you do nothing, the computer will restart automatically in %@."); break;
            case ScheduledShutDown: m_base = tr("WILL_SCHEDULED_SHUTDOWN_INFO", "If you do nothing, the computer will shut down automatically in %@."); break;
            case ScheduledSleep: m_base = tr("WILL_SCHEDULED_SLEEP_INFO", "If you do nothing, the computer will go to sleep automatically in %@."); break;
            case ScheduledRestart: m_base = tr("WILL_SCHEDULED_RESTART_INFO", "If you do nothing, the computer will restart automatically in %@."); break;
            default: m_base = tr("WILL_SHUTDOWN_INFO", "If you do nothing, the computer will shut down automatically in %@."); break;
            }
        }
        const long index = std::min(m_countdown / 60, 2L) * 10 + std::min(m_countdown % 60, 2L);
        if (index != m_formatIndex) {
            m_timeFormat = tr("TIME_FORMAT_" + std::to_string(index), "%d seconds");
            m_formatIndex = index;
        }
        m_timedOut->setText(replaceAll(m_base, "%@", countdownTime(m_formatIndex, m_timeFormat, m_countdown)));
    }

    // -doDefaultAction: (0x1000353b6): types 2 and 5 restart:, 4 sleep:, the others shut down (the
    // panel closed, ApplicationManager's 'shut')
    void doDefaultAction()
    {
        finish(m_type == Restart || m_type == ScheduledRestart ? PowerAction::Restart
               : m_type == ScheduledSleep                      ? PowerAction::Sleep
                                                               : PowerAction::ShutDown);
    }

    void finish(PowerAction action)
    {
        if (m_finished)
            return;
        m_finished = true;
        if (m_app && m_timer)
            m_app->removeTimer(m_timer);   // -invalidateTimers
        m_timer = 0;
        if (m_window) {
            m_window->onClose = nullptr;
            m_window->close();
        }
        if (m_done)
            m_done(action);
    }

    Type m_type;
    Nib m_nib;
    View *m_content = nullptr;
    Size m_size;
    ImageView *m_image = nullptr;
    Label *m_info = nullptr, *m_timedOut = nullptr, *m_guest = nullptr;
    Button *m_default = nullptr, *m_cancel = nullptr, *m_restart = nullptr, *m_sleep = nullptr;
    std::unique_ptr<Window> m_window;
    App *m_app = nullptr;
    int m_timer = 0;
    double m_gap = 8;
    long m_countdown = 60;   // DAT_1000903d8's 60 seconds (-setTimerLength: for the scheduled ones)
    long m_formatIndex = -1;
    std::string m_base, m_timeFormat;
    std::function<void(PowerAction)> m_done;
    bool m_finished = false;
};

// ---- 'rlgo': the applications asked to quit, then the session ended; an application still there
// after a while keeps the session (TIMED_OUT_ALERT_MESSAGE)

class LogOut {
public:
    explicit LogOut(App &app) : m_app(app) {}
    // Restart and Shut Down: the computer's, once the applications have quit (a whole session only:
    // SNOWLEOPARD_POWER=1, not a window of another desktop)
    std::function<void()> beforeEnd;

    void start()
    {
        if (!m_x.conn) {
            for (pid_t pid : applicationProcesses(m_x, {}))
                kill(pid, SIGTERM);
            if (beforeEnd)
                beforeEnd();
            endSession();
            m_app.quit(0);
            return;
        }
        m_windows = m_x.applicationWindows();
        m_processes = applicationProcesses(m_x, m_windows);
        const xcb_atom_t pidAtom = m_x.atom("_NET_WM_PID");
        for (xcb_window_t w : m_windows)
            for (uint32_t p : m_x.cardinals(w, pidAtom, XCB_ATOM_CARDINAL))
                if (std::find(m_processes.begin(), m_processes.end(), pid_t(p)) != m_processes.end())
                    m_windowProcesses.push_back(pid_t(p));
        for (xcb_window_t w : m_windows)
            m_x.close(w);
        m_started = 0;
        m_timer = m_app.addTimer(0.25, true, [this] { check(); });
    }

private:
    void check()
    {
        m_started += 0.25;
        const std::vector<xcb_window_t> now = m_x.applicationWindows();
        std::vector<xcb_window_t> left;
        for (xcb_window_t w : m_windows)
            if (std::find(now.begin(), now.end(), w) != now.end())
                left.push_back(w);
        if (left.empty()) {
            // their windows closed (what they had to save, saved): the applications quit, as the quit
            // event ('quit') does on the Mac. Those that had windows quit by themselves once their last
            // one is gone, writing what they keep (a browser its cookies and its session): they are
            // given the time to (DrainTime), and only those still there then are told to; the others
            // (the shell's applications in front without a window) at once
            if (m_draining < 0) {
                m_draining = 0;
                for (pid_t pid : m_processes)
                    if (std::find(m_windowProcesses.begin(), m_windowProcesses.end(), pid) == m_windowProcesses.end())
                        kill(pid, SIGTERM);
            }
            m_draining += 0.25;
            const bool alive = std::any_of(m_windowProcesses.begin(), m_windowProcesses.end(), [](pid_t pid) { return running(pid); });
            if (alive && m_draining < DrainTime)
                return;
            m_app.removeTimer(m_timer);
            for (pid_t pid : m_windowProcesses)
                if (running(pid))
                    kill(pid, SIGTERM);
            if (beforeEnd)
                beforeEnd();
            endSession();
            m_app.quit(0);
            return;
        }
        if (m_started < TimeOut)
            return;
        m_app.removeTimer(m_timer);
        const std::string name = m_x.wmClass(left.front());
        Alert::run({}, replaceAll(tr("TIMED_OUT_ALERT_MESSAGE", "You haven’t been logged out because the application %@ failed to quit. "
                                                                   "To try logging out again, quit %@ and choose Apple menu > Log Out."),
                                  "%@", name.empty() ? std::string("?") : name),
                   {}, {"OK"}, [this](int) { m_app.quit(1); });
    }

    // a process still running, not a zombie waiting for its parent
    static bool running(pid_t pid)
    {
        if (kill(pid, 0) != 0)
            return false;
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        std::getline(stat, line);
        const size_t close = line.rfind(')');
        return close == std::string::npos || close + 2 >= line.size() || line[close + 2] != 'Z';
    }

    static constexpr double TimeOut = 30;   // the applications' time to quit (not read from 10.6 yet)
    static constexpr double DrainTime = 20;   // once their windows are gone, to quit by themselves
    std::vector<pid_t> m_windowProcesses;
    double m_draining = -1;
    App &m_app;
    Display m_x;
    std::vector<xcb_window_t> m_windows;
    std::vector<pid_t> m_processes;
    int m_timer = 0;
    double m_started = 0;
};

} // namespace

namespace {

// The computer's own Restart, Shut Down and Sleep (a whole session only: SNOWLEOPARD_POWER=1, not a
// window of another desktop)
void power(const char *what)
{
    const char *on = std::getenv("SNOWLEOPARD_POWER");
    if (!on || std::string(on) != "1")
        return;
    const char *const argv[] = {"systemctl", what, nullptr};
    pid_t pid;
    posix_spawnp(&pid, argv[0], nullptr, nullptr, const_cast<char *const *>(argv), environ);
}

} // namespace

int restart(int type)
{
    App &app = App::shared();
    if (!app.open()) {
        std::fprintf(stderr, "loginwindow: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    LogOut logout(app);
    ShutDownAlert alert{ShutDownAlert::Type(type)};
    if (!alert.load()) {
        std::fprintf(stderr, "loginwindow: ShutDownAlert.nib not found (extract the DVD's files: tools/extract.py)\n");
        return 1;
    }
    alert.show(app, [&](PowerAction action) {
        switch (action) {
        case PowerAction::Cancel:
            app.quit(1);   // cancel: (the session goes on)
            break;
        case PowerAction::Sleep:
            power("suspend");   // sleep: Sleep(), the session goes on
            app.quit(0);
            break;
        case PowerAction::ShutDown:
        case PowerAction::Restart:
            // ApplicationManager's 'shut' and 'rest': the applications asked to quit, then the computer
            // (in a window of another desktop the computer isn't the session's: the session ends alone)
            logout.beforeEnd = [action] { power(action == PowerAction::ShutDown ? "poweroff" : "reboot"); };
            logout.start();
            break;
        }
    });
    return app.run();
}

int logOut(bool askFirst)
{
    App &app = App::shared();
    if (!app.open()) {
        std::fprintf(stderr, "loginwindow: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    LogOut logout(app);
    if (!askFirst) {
        logout.start();
        return app.run();
    }
    LogOutAlert alert;
    if (!alert.load()) {
        std::fprintf(stderr, "loginwindow: LogOut.nib not found (extract the DVD's files: tools/extract.py): logging out at once\n");
        logout.start();
        return app.run();
    }
    alert.show(app, [&](bool yes) {
        if (yes)
            logout.start();
        else
            app.quit(1);   // cancel: (the session goes on)
    });
    return app.run();
}

} // namespace LoginWindow
