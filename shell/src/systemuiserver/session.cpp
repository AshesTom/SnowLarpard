// SPDX-License-Identifier: LGPL-2.1-or-later
#include "session.h"

#include "foundation.h"
#include "runloop.h"
#include "windows.h"

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace Session {

App appForWindow(xcb_window_t w)
{
    return LaunchServices::forWindow(Windows::wmClass(w), Windows::wmInstance(w), Windows::pid(w));
}

std::vector<xcb_window_t> windowsOf(const App &app)
{
    std::vector<xcb_window_t> out;
    for (xcb_window_t w : Windows::clientWindows())
        if (appForWindow(w).id == app.id)
            out.push_back(w);
    return out;
}

void hideApp(const App &app)
{
    for (xcb_window_t w : windowsOf(app))
        if (!Windows::minimized(w))
            Windows::hide(w);
}

void hideOthers(const App &app)
{
    for (xcb_window_t w : Windows::clientWindows())
        if (appForWindow(w).id != app.id && !Windows::minimized(w))
            Windows::hide(w);
}

void showAll()
{
    for (xcb_window_t w : Windows::clientWindows())
        if (Windows::hidden(w))   // the hidden ones; the minimized stay in the Dock
            Windows::activate(w);
}

void bringToFront(const App &app)
{
    for (xcb_window_t w : windowsOf(app))
        Windows::activate(w);
}

void quitApp(const App &app)
{
    for (xcb_window_t w : windowsOf(app))
        Windows::close(w);
}

std::string userFullName()
{
    if (passwd *pw = getpwuid(getuid())) {
        std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
        gecos = gecos.substr(0, gecos.find(','));
        return gecos.empty() ? pw->pw_name : gecos;
    }
    const char *u = std::getenv("USER");
    return u ? u : "";
}

namespace {

void loginwindow(const std::string &arg)
{
    const std::string exe = LaunchServices::appsDir() + "/loginwindow.app/Contents/MacOS/loginwindow";
    AK::Task::launchDetached({exe, arg});
}

} // namespace

void aboutThisMac()
{
    // brought forward when open (out of the client list's applications: a panel), else started
    for (uint32_t w : Windows::allClientWindows())
        if (Windows::wmClass(w) == "loginwindow") {
            Windows::activate(w);
            return;
        }
    loginwindow("--about");
}

void forceQuit() { loginwindow("--force-quit"); }
void restart() { loginwindow("--restart"); }
void shutDown() { loginwindow("--shutdown"); }
void logOut(bool ask) { loginwindow(ask ? "--logout" : "--logout-now"); }

void sleep()
{
    // a whole session only (SNOWLEOPARD_POWER=1), not a window of another desktop
    const char *p = std::getenv("SNOWLEOPARD_POWER");
    if (p && std::string(p) == "1")
        AK::Task::launchDetached({"systemctl", "suspend"});
}

} // namespace Session
