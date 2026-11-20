// SPDX-License-Identifier: LGPL-2.1-or-later
#include "findermenus.h"

#include "foundation.h"
#include "launchservices.h"
#include "runloop.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace AK;

namespace FinderMenus {

namespace {

struct State {
    bool valid = false;
    std::set<std::string> enabled, checked;
    std::map<std::string, std::string> titles;
    int label = -1;
};

std::string env(const char *name)
{
    const char *v = std::getenv(name);
    return v ? v : "";
}

std::string displayTag()
{
    std::string d = env("DISPLAY");
    std::replace(d.begin(), d.end(), '/', '_');
    std::replace(d.begin(), d.end(), ':', '_');
    return d;
}

std::string runtimeDir()
{
    const std::string r = env("XDG_RUNTIME_DIR");
    return r.empty() ? "/tmp" : r;
}

std::string desktopName() { return "snowleopard-desktop-" + std::to_string(getuid()) + "-" + displayTag(); }

std::string stateFile(bool desktop)
{
    if (desktop)
        return runtimeDir() + "/" + desktopName() + ".state";
    std::string display = env("WAYLAND_DISPLAY");
    if (display.empty())
        display = env("DISPLAY");
    std::replace(display.begin(), display.end(), '/', '_');
    return runtimeDir() + "/snowleopard-finder-" + std::to_string(getuid()) + "-" + display + ".state";
}

State readState(bool desktop)
{
    State s;
    std::ifstream f(stateFile(desktop));
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("pid ", 0) == 0) {
            const int pid = std::atoi(line.c_str() + 4);
            s.valid = pid > 0 && kill(pid, 0) == 0;
        } else if (line.rfind("E ", 0) == 0) {
            s.enabled.insert(line.substr(2));
        } else if (line.rfind("C ", 0) == 0) {
            s.checked.insert(line.substr(2));
        } else if (line.rfind("T ", 0) == 0) {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos && tab > 2)
                s.titles[line.substr(2, tab - 2)] = line.substr(tab + 1);
        } else if (line.rfind("L ", 0) == 0) {
            s.label = std::atoi(line.c_str() + 2);
        }
    }
    return s;
}

bool needsWindow(const std::string &cmd)
{
    static const std::set<std::string> global = {"cmdNewFileViewer:", "cmdNewFolder:", "cmdGoToComputer:", "cmdGoHome:",
                                                 "cmdGoToDesktop:", "cmdGoToNetwork:", "cmdGoToApplications:", "cmdGoToDocuments:",
                                                 "cmdGoToUtilities:", "cmdGoTo:", "cmdConnectTo:", "cmdPreferences:", "cmdEmptyTrash:",
                                                 "cmdEmptyTrashSecure:", "cmdFindFile:", "cmdNewSmartFolder:", "cmdClearRecentFolders:"};
    return !global.count(cmd) && cmd.rfind("cmdRecentFoldersItem:", 0) != 0;
}

// HIServices' label colours (_sCachedColors) and the menus' order
const Color LabelColors[7] = {Color::rgb(0xfb, 0x62, 0x59), Color::rgb(0xf6, 0xa9, 0x41), Color::rgb(0xef, 0xda, 0x44),
                              Color::rgb(0xb3, 0xd6, 0x44), Color::rgb(0x57, 0xa1, 0xff), Color::rgb(0xbf, 0x8c, 0xd8),
                              Color::rgb(0xa8, 0xa8, 0xa8)};
const int LabelOrder[7] = {6, 7, 5, 2, 4, 3, 1};

struct Entry {
    const char *title;   // "-" a separator
    const char *keys;
    const char *command;
};

void newFolderOnDesktop()
{
    std::string desktop = home() + "/Desktop";
    makePath(desktop);
    std::string name = "untitled folder";
    for (int n = 2; isDir(desktop + "/" + name) || isFile(desktop + "/" + name); ++n)
        name = "untitled folder " + std::to_string(n);
    makePath(desktop + "/" + name);
}

std::vector<MenuItem> entries(const std::vector<Entry> &list, const State &s, bool desktop)
{
    std::vector<MenuItem> out;
    const bool window = s.valid;
    for (const Entry &e : list) {
        if (std::string(e.title) == "-") {
            out.push_back(MenuItem::line());
            continue;
        }
        const std::string cmd = e.command;
        std::string title = window && s.titles.count(cmd) ? s.titles.at(cmd) : e.title;
        const bool onDesktop = desktop && cmd == "cmdNewFolder:" && !s.valid;
        bool enabled;
        if (cmd.empty())
            enabled = false;
        else if (onDesktop)
            enabled = true;
        else if (needsWindow(cmd))
            enabled = window && s.enabled.count(cmd);
        else
            enabled = window ? s.enabled.count(cmd) > 0 : cmd != "cmdGoToNetwork:";
        MenuItem item(title, false, enabled, window && s.checked.count(cmd));
        item.keyEquivalent = e.keys;
        item.action = [cmd, onDesktop, desktop] {
            if (onDesktop)
                newFolderOnDesktop();
            else
                send(cmd, desktop);
        };
        out.push_back(std::move(item));
    }
    return out;
}

MenuItem command(const std::string &title, const std::string &keys, const std::string &cmd, bool enabled, bool desktop)
{
    MenuItem i(title, false, enabled);
    i.keyEquivalent = keys;
    i.action = [cmd, desktop] { send(cmd, desktop); };
    return i;
}

} // namespace

void send(const std::string &cmd, bool desktop)
{
    if (desktop) {
        // the desktop's icons take it (and hand the Finder's windows what isn't theirs)
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            const std::string name = desktopName();
            std::memcpy(a.sun_path + 1, name.data(), name.size());
            if (connect(fd, reinterpret_cast<sockaddr *>(&a), socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size())) == 0) {
                const std::string msg = "cmd:" + cmd + "\n";
                const bool ok = ::write(fd, msg.data(), msg.size()) == ssize_t(msg.size());
                ::close(fd);
                if (ok)
                    return;
            } else {
                ::close(fd);
            }
        }
    }
    const std::string exe = LaunchServices::appsDir() + "/Finder.app/Contents/MacOS/FinderWindows";
    if (isFile(exe))
        Task::launchDetached({exe, "--command", cmd});
}

std::vector<std::string> titles() { return {"File", "Edit", "View", "Go"}; }

std::vector<MenuItem> menu(const std::string &title, bool desktop)
{
    const State s = readState(desktop);
    const bool window = s.valid;
    if (title == "File") {
        std::vector<MenuItem> items = entries({{"New Finder Window", "⌘N", "cmdNewFileViewer:"},
                                               {"New Folder", "⇧⌘N", "cmdNewFolder:"},
                                               {"New Smart Folder", "⌥⌘N", "cmdNewSmartFolder:"},
                                               {"New Burn Folder", "", ""},
                                               {"Open", "⌘O", "cmdOpen:"},
                                               {"Print", "", ""},
                                               {"Close Window", "⌘W", "performClose:"},
                                               {"-", "", ""},
                                               {"Get Info", "⌘I", "cmdShowStaticInfo:"},
                                               {"-", "", ""},
                                               {"Compress", "", "cmdArchive:"},
                                               {"-", "", ""},
                                               {"Duplicate", "⌘D", "cmdDuplicate:"},
                                               {"Make Alias", "⌘L", "cmdMakeAlias:"},
                                               {"Quick Look", "⌘Y", "cmdQuickLook:"},
                                               {"Show Original", "⌘R", "cmdShowOriginal:"},
                                               {"Add to Sidebar", "⌘T", "cmdAddToSidebar:"},
                                               {"-", "", ""},
                                               {"Move to Trash", "⌘⌫", "cmdMoveToTrash:"},
                                               {"Eject", "⌘E", "cmdEject:"},
                                               {"Burn Disc", "", ""},
                                               {"-", "", ""},
                                               {"Find", "⌘F", "cmdFindFile:"},
                                               {"-", "", ""}},
                                              s, desktop);
        // Label: and its swatches (TLabelViewForMenu), the first the "none" one
        MenuItem label("Label:", false, window && s.enabled.count("cmdSetLabel:"));
        label.swatches.push_back({});
        int checked = -1;
        for (int k = 0; k < 7; ++k) {
            label.swatches.push_back(LabelColors[k]);
            if (LabelOrder[k] == s.label)
                checked = k + 1;
        }
        label.swatchChecked = s.label == 0 ? 0 : checked;
        label.swatchChosen = [desktop](int sw) { send("cmdSetLabel" + std::to_string(sw == 0 ? 0 : LabelOrder[sw - 1]) + ":", desktop); };
        items.push_back(label);
        return items;
    }
    if (title == "Edit")
        return entries({{"Undo", "⌘Z", "undo:"},
                        {"Redo", "⇧⌘Z", ""},
                        {"-", "", ""},
                        {"Cut", "⌘X", ""},
                        {"Copy", "⌘C", "copy:"},
                        {"Paste", "⌘V", "paste:"},
                        {"Select All", "⌘A", "selectAll:"},
                        {"-", "", ""},
                        {"Show Clipboard", "", ""}},
                       s, desktop);
    if (title == "View") {
        std::vector<MenuItem> items = entries({{"as Icons", "⌘1", "cmdViewAsIcons:"},
                                               {"as List", "⌘2", "cmdViewAsList:"},
                                               {"as Columns", "⌘3", "cmdViewAsColumns:"},
                                               {"as Cover Flow", "⌘4", "cmdViewAsFlow:"},
                                               {"-", "", ""},
                                               {"Clean Up", "", "cmdCleanUp:"}},
                                              s, desktop);
        MenuItem arrange("Arrange By", false, window && s.enabled.count("cmdArrange:"));
        static const char *const by[][3] = {{"Name", "⌃⌘1", "Name"},          {"Date Modified", "⌃⌘2", "ModDate"},
                                            {"Date Created", "⌃⌘3", "CreationDate"}, {"Size", "⌃⌘4", "PhysicalSize"},
                                            {"Kind", "⌃⌘5", "Kind"},           {"Label", "⌃⌘6", "Label"}};
        for (const auto &b : by) {
            MenuItem i = command(b[0], b[1], std::string("cmdArrangeBy") + b[2] + ":", true, desktop);
            i.checked = s.checked.count(std::string("cmdKeepArrangedBy") + b[2] + ":") > 0;
            arrange.submenu.push_back(i);
        }
        items.push_back(arrange);
        items.push_back(MenuItem::line());
        auto titled = [&](const char *cmd, const char *fallback) {
            return window && s.titles.count(cmd) ? s.titles.at(cmd) : std::string(fallback);
        };
        items.push_back(command(titled("cmdToggleBottombar:", "Show Path Bar"), "", "cmdToggleBottombar:", window, desktop));
        items.push_back(command(titled("cmdToggleStatusBar:", "Hide Status Bar"), "", "cmdToggleStatusBar:", window, desktop));
        std::string sidebar = titled("cmdToggleSidebar:", "Hide Sidebar");
        if (sidebar.find('\t') != std::string::npos)
            sidebar = sidebar.substr(0, sidebar.find('\t'));
        items.push_back(command(sidebar, "⌥⌘S", "cmdToggleSidebar:", window, desktop));
        items.push_back(MenuItem::line());
        items.push_back(command(titled("cmdToggleToolbar:", "Hide Toolbar"), "⌥⌘T", "cmdToggleToolbar:", window, desktop));
        items.emplace_back("Customize Toolbar…", false, false);
        items.push_back(MenuItem::line());
        items.push_back(command(titled("cmdToggleViewOptions:", "Show View Options"), "⌘J", "cmdToggleViewOptions:", window, desktop));
        return items;
    }
    if (title == "Go") {
        std::vector<MenuItem> items = entries({{"Back", "⌘[", "cmdBack:"},
                                               {"Forward", "⌘]", "cmdForward:"},
                                               {"Enclosing Folder", "⌘↑", "cmdOpenParent:"},
                                               {"-", "", ""},
                                               {"Computer", "⇧⌘C", "cmdGoToComputer:"},
                                               {"Home", "⇧⌘H", "cmdGoHome:"},
                                               {"Desktop", "⇧⌘D", "cmdGoToDesktop:"},
                                               {"Network", "⇧⌘K", "cmdGoToNetwork:"},
                                               {"iDisk", "", ""},
                                               {"Applications", "⇧⌘A", "cmdGoToApplications:"},
                                               {"Documents", "⇧⌘O", "cmdGoToDocuments:"},
                                               {"Utilities", "⇧⌘U", ""},
                                               {"-", "", ""}},
                                              s, desktop);
        // Recent Folders (FXRecentFolders), then Go to Folder… and Connect to Server…
        MenuItem recent("Recent Folders");
        const std::vector<std::string> folders = Settings("finder").list("FXRecentFolders");
        for (const std::string &f : folders) {
            if (f.empty())
                continue;
            recent.submenu.push_back(command(f == "/" ? "/" : fileName(f), "", "cmdRecentFoldersItem:" + f, true, desktop));
        }
        recent.submenu.push_back(MenuItem::line());
        recent.submenu.push_back(command("Clear Menu", "", "cmdClearRecentFolders:", !folders.empty(), desktop));
        items.push_back(recent);
        items.push_back(MenuItem::line());
        items.push_back(command("Go to Folder…", "⇧⌘G", "cmdGoTo:", true, desktop));
        items.push_back(command("Connect to Server…", "⌘K", "cmdConnectTo:", true, desktop));
        return items;
    }
    return {};
}

} // namespace FinderMenus
