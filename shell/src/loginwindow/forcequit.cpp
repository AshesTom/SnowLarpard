// SPDX-License-Identifier: LGPL-2.1-or-later
// loginwindow's Force Quit Applications: ProcessPanel.nib's panel (processTable, forceQuitButton,
// _messageTextField, _infoTextField), the running applications with their icons, Force Quit asking
// first (FORCE_QUIT_ALERT_MESSAGE, FORCE_QUIT_INFO_MESSAGE, as the nib's _confirmationPanel sheet asks).
// The applications are those with windows (the window manager's list), killed by their processes.
#include "logout.h"

#include "controls.h"
#include "foundation.h"
#include "launchservices.h"
#include "nib.h"
#include "panels.h"
#include "text.h"
#include "windows.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <cstdio>
#include <map>
#include <unistd.h>

using namespace AK;

namespace LoginWindow {

namespace {

const char *const Bundle = "/System/Library/CoreServices/loginwindow.app";
std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "loginwindow", key, fallback); }

struct Running {
    LaunchServices::App app;
    std::vector<pid_t> pids;
    Image icon;
};

std::vector<Running> runningApplications()
{
    std::vector<Running> out;
    std::map<std::string, size_t> byId;
    for (xcb_window_t w : Windows::clientWindows()) {
        const pid_t pid = pid_t(Windows::pid(w));
        if (pid <= 0 || pid == getpid())
            continue;
        const LaunchServices::App app = LaunchServices::forWindow(Windows::wmClass(w), Windows::wmInstance(w), uint32_t(pid));
        auto it = byId.find(app.id);
        if (it == byId.end()) {
            byId[app.id] = out.size();
            out.push_back({app, {pid}, LaunchServices::icon(app, 16)});
        } else if (std::find(out[it->second].pids.begin(), out[it->second].pids.end(), pid) == out[it->second].pids.end()) {
            out[it->second].pids.push_back(pid);
        }
    }
    // and the shell's AquaKit applications without a window (Preview launched alone): their menus'
    // sockets ("@snowleopard-menus-<pid>") of this display
    const char *display = std::getenv("DISPLAY");
    std::ifstream sockets("/proc/net/unix");
    std::string line;
    while (std::getline(sockets, line)) {
        const size_t at = line.find("@snowleopard-menus-");
        if (at == std::string::npos)
            continue;
        const pid_t pid = pid_t(std::atoi(line.c_str() + at + 19));
        if (pid <= 0 || pid == getpid() || kill(pid, 0) != 0)
            continue;
        std::ifstream env("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
        std::string var, theirs;
        while (std::getline(env, var, '\0'))
            if (var.rfind("DISPLAY=", 0) == 0)
                theirs = var.substr(8);
        if (theirs != (display ? display : ""))
            continue;
        const LaunchServices::App app = LaunchServices::forPid(uint32_t(pid));
        if (!app.valid())
            continue;
        auto it = byId.find(app.id);
        if (it == byId.end()) {
            byId[app.id] = out.size();
            out.push_back({app, {pid}, LaunchServices::icon(app, 16)});
        } else if (std::find(out[it->second].pids.begin(), out[it->second].pids.end(), pid) == out[it->second].pids.end()) {
            out[it->second].pids.push_back(pid);
        }
    }
    return out;
}

} // namespace

int forceQuit()
{
    App &app = App::shared();
    if (!app.open() || !Windows::open()) {
        std::fprintf(stderr, "loginwindow: no display (X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    const std::string res = Assets::path(std::string(Bundle) + "/Contents/Resources");
    Nib nib;
    if (!nib.load(res + "/English.lproj/ProcessPanel.nib/keyedobjects.nib", res)) {
        std::fprintf(stderr, "loginwindow: ProcessPanel.nib not found (extract the DVD's files: tools/extract.py)\n");
        return 1;
    }
    View *content = nib.instantiate("panel");
    auto *table = nib.outlet<TableView>("processTable");
    auto *button = nib.outlet<Button>("forceQuitButton");
    auto *message = nib.outlet<Label>("_messageTextField");
    if (!content || !table || !button) {
        std::fprintf(stderr, "loginwindow: ProcessPanel.nib: not what was expected\n");
        return 1;
    }
    if (message)
        message->setText(tr("NORMAL_MESSAGE_TEXT", message->text()));
    // Resume: for an application paused (in the debugger), which Linux's list doesn't tell
    if (auto *resume = nib.outlet<Button>("_pauseResumeButton"))
        resume->setHidden(true);
    table->header = false;
    std::vector<Running> running;
    auto reload = [&] {
        std::string selectedId = table->selected >= 0 && size_t(table->selected) < running.size() ? running[size_t(table->selected)].app.id : "";
        running = runningApplications();
        table->rows.clear();
        table->selected = -1;
        for (size_t i = 0; i < running.size(); ++i) {
            table->rows.push_back(std::vector<std::string>(std::max<size_t>(1, table->columns.size()), running[i].app.name));
            if (running[i].app.id == selectedId)
                table->selected = int(i);
        }
        if (table->selected < 0 && !running.empty())
            table->selected = 0;
        table->reload();
        button->setEnabled(table->selected >= 0);
    };
    // each row: its application's icon, then its name (the nib's cells: an image and a text)
    table->drawCell = [&](cairo_t *cr, int row, int column, const Rect &cell, bool highlighted) {
        if (column != 0 || row < 0 || size_t(row) >= running.size())
            return false;
        const Running &r = running[size_t(row)];
        if (!r.icon.isNull())
            r.icon.draw(cr, Rect{cell.x + 2, cell.y + std::floor((cell.h - 16) / 2), 16, 16});
        TextStyle t;
        t.color = highlighted ? Color::gray(255) : Color::gray(0);
        drawText(cr, r.app.name, Rect{cell.x + 22, cell.y, cell.w - 24, cell.h}, t);
        return true;
    };
    table->onSelect = [&](int) { button->setEnabled(table->selected >= 0); };
    Window window(nib.size(), "Force Quit Applications");
    window.contentView()->addSubview(content);
    window.setStyleMask(2);   // closable only (NSClosableWindowMask)
    window.setFloatingPanel(false);
    window.onClose = [&app] { app.quit(); };
    button->action = [&] {
        if (table->selected < 0 || size_t(table->selected) >= running.size())
            return;
        const Running r = running[size_t(table->selected)];
        std::string text = tr("FORCE_QUIT_ALERT_MESSAGE", "Do you want to force %@ to quit?");
        const size_t at = text.find("%@");
        if (at != std::string::npos)
            text.replace(at, 2, "“" + r.app.name + "”");
        Alert::beginSheet(&window, r.icon, text, tr("FORCE_QUIT_INFO_MESSAGE", "You will lose any unsaved changes."),
                          {tr("FORCEQUIT_BUTTON", "Force Quit"), "Cancel"}, [&, r](int choice) {
                              if (choice != 0)
                                  return;
                              for (pid_t pid : r.pids)
                                  kill(pid, SIGKILL);
                              app.addTimer(0.5, false, reload);
                          });
    };
    reload();
    window.show();
    app.addTimer(2, true, reload);
    return app.run();
}

} // namespace LoginWindow
