// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aquakitmenus.h"

#include "session.h"

#include <algorithm>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace AquaKitMenus {

namespace {

struct Entry {
    int depth = 0;
    char kind = 'I';   // M submenu, I item, S separator
    bool enabled = false, checked = false;
    std::string keys, action, path, title;
};

int connectTo(int pid)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    const std::string name = "snowleopard-menus-" + std::to_string(pid);
    std::memcpy(a.sun_path + 1, name.data(), name.size());   // abstract
    if (connect(fd, reinterpret_cast<sockaddr *>(&a), socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size())) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t at; (at = s.find(sep, from)) != std::string::npos; from = at + 1)
        out.push_back(s.substr(from, at - from));
    out.push_back(s.substr(from));
    return out;
}

std::vector<Entry> fetch(int pid)
{
    std::vector<Entry> entries;
    const int fd = connectTo(pid);
    if (fd < 0)
        return entries;
    const char req[] = "menus\n";
    if (::write(fd, req, sizeof req - 1) < 0) {
        ::close(fd);
        return entries;
    }
    // the application answers between two of its events: a menu waits for it a little
    std::string data;
    char buf[8192];
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 400) <= 0)
            break;
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        data.append(buf, size_t(n));
    }
    ::close(fd);
    for (const std::string &line : split(data, '\n')) {
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 8)
            continue;
        Entry e;
        e.depth = std::atoi(f[0].c_str());
        e.kind = f[1].empty() ? 'I' : f[1][0];
        e.enabled = f[2] == "1";
        e.checked = f[3] == "1";
        e.keys = f[4];
        e.action = f[5];
        e.path = f[6];
        e.title = f[7];
        entries.push_back(e);
    }
    return entries;
}

void send(int pid, const std::string &path)
{
    const int fd = connectTo(pid);
    if (fd < 0)
        return;
    const std::string req = "do " + path + "\n";
    if (::write(fd, req.data(), req.size()) < 0) {
    }
    ::close(fd);
}

// the items after `from` one level deeper
std::vector<AK::MenuItem> fill(const std::vector<Entry> &entries, size_t from, int pid, const LaunchServices::App &app)
{
    std::vector<AK::MenuItem> out;
    const int depth = entries[from].depth + 1;
    for (size_t i = from + 1; i < entries.size() && entries[i].depth >= depth; ++i) {
        const Entry &e = entries[i];
        if (e.depth > depth)
            continue;
        if (e.kind == 'S') {
            out.push_back(AK::MenuItem::line());
        } else if (e.kind == 'M') {
            AK::MenuItem m(e.title, false, e.enabled);
            m.submenu = fill(entries, i, pid, app);
            if (m.submenu.empty())
                m.submenu.push_back(AK::MenuItem("", false, false));
            out.push_back(std::move(m));
        } else {
            AK::MenuItem m(e.title, false, e.enabled, e.checked);
            m.keyEquivalent = e.keys;
            const std::string action = e.action, path = e.path;
            m.action = [pid, app, action, path] {
                // the windows' business, done by the menu bar as for every application
                if (action == "hide:")
                    Session::hideApp(app);
                else if (action == "hideOtherApplications:")
                    Session::hideOthers(app);
                else if (action == "unhideAllApplications:")
                    Session::showAll();
                else
                    send(pid, path);
            };
            out.push_back(std::move(m));
        }
    }
    return out;
}

} // namespace

bool available(int pid)
{
    if (pid <= 0)
        return false;
    const int fd = connectTo(pid);
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

std::vector<Top> topLevel(int pid)
{
    static const std::vector<std::string> standard = {"performMiniaturize:", "miniaturize:", "performZoom:", "zoom:",
                                                      "arrangeInFront:", "makeKeyAndOrderFront:", "performClose:", ""};
    std::vector<Top> out;
    const std::vector<Entry> entries = fetch(pid);
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry &top = entries[i];
        if (top.depth != 0 || top.kind != 'M')
            continue;
        Top t{top.title, top.path, false};
        if (top.title == "Window") {
            bool own = false;
            for (size_t j = i + 1; j < entries.size() && entries[j].depth > 0; ++j)
                if (entries[j].kind == 'I' && std::find(standard.begin(), standard.end(), entries[j].action) == standard.end())
                    own = true;
            t.windowMenu = !own;
        }
        out.push_back(t);
    }
    return out;
}

std::vector<AK::MenuItem> menu(int pid, const std::string &path, const LaunchServices::App &app)
{
    const std::vector<Entry> now = fetch(pid);
    for (size_t j = 0; j < now.size(); ++j)
        if (now[j].depth == 0 && now[j].path == path)
            return fill(now, j, pid, app);
    return {};
}

} // namespace AquaKitMenus
