// SPDX-License-Identifier: LGPL-2.1-or-later
#include "goto.h"

#include "files.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"

#include <dirent.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "Localizable", key, fallback); }

std::unique_ptr<Nib> loadNib(const std::string &name)
{
    auto nib = std::make_unique<Nib>();
    const std::string res = std::string(Bundle) + "/Contents/Resources/";
    if (!nib->load(Assets::path(res + "English.lproj/" + name + ".nib/keyedobjects.nib"), Assets::path(res)))
        return nullptr;
    return nib;
}

template <typename T> T *find(const Nib &nib, const std::string &outlet, const std::string &title = {})
{
    if (T *t = nib.outlet<T>(outlet))
        return t;
    if (!title.empty())
        return dynamic_cast<T *>(nib.withTitle(title));
    return nullptr;
}

// the Go to Folder sheet's state (one at a time)
struct GotoState {
    std::unique_ptr<Nib> nib;
    Window *window = nullptr;
    std::unique_ptr<Window> own;   // without a Finder window: a window of its own
    std::function<void(const std::string &)> go;
};
std::unique_ptr<GotoState> s_goto;

void endGoto()
{
    if (!s_goto)
        return;
    if (s_goto->own) {
        s_goto->own->onClose = nullptr;
        s_goto->own->close();
        Window *w = s_goto->own.release();
        App::shared().later([w] { delete w; });
    } else if (s_goto->window && Sheet::isOpen(s_goto->window)) {
        Sheet::end(s_goto->window);
    }
    GotoState *g = s_goto.release();
    App::shared().later([g] { delete g; });
}

} // namespace

std::string GoTo::expand(const std::string &typed)
{
    std::string p = typed;
    while (!p.empty() && (p.back() == ' ' || p.back() == '\n'))
        p.pop_back();
    while (!p.empty() && p.front() == ' ')
        p.erase(0, 1);
    if (p == "~" || p.rfind("~/", 0) == 0)
        p = home() + p.substr(1);
    if (p.rfind("file://", 0) == 0)
        p = p.substr(7);
    // the Mac's own places, as 10.6's users type them
    if (p == "/Applications" && !isDir(p))
        p = "/usr/share/applications";
    if (p.empty())
        return p;
    if (p[0] != '/')
        p = home() + "/" + p;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}

std::string GoTo::complete(const std::string &typed)
{
    const std::string full = expand(typed);
    if (full.empty())
        return typed;
    const bool endsInSlash = !typed.empty() && typed.back() == '/';
    const std::string folder = endsInSlash ? full : (dirName(full).empty() ? "/" : dirName(full));
    const std::string start = endsInSlash ? std::string() : fileName(full);
    std::vector<std::string> names;
    if (DIR *d = opendir(folder.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == ".." || (n[0] == '.' && (start.empty() || start[0] != '.')))
                continue;
            if (strncasecmp(n.c_str(), start.c_str(), start.size()) == 0 && isDir(folder + "/" + n))
                names.push_back(n);
        }
        closedir(d);
    }
    if (names.empty())
        return typed;
    std::sort(names.begin(), names.end(), finderLess);
    // the typed part kept as typed ("~/Doc" -> "~/Documents/")
    std::string prefix = endsInSlash ? typed : typed.substr(0, typed.size() - start.size());
    if (!prefix.empty() && prefix.back() != '/')
        prefix += '/';
    return prefix + names.front() + "/";
}

void GoTo::show(Window *window, double sheetTop, std::function<void(const std::string &)> go)
{
    if (s_goto)
        endGoto();
    auto state = std::make_unique<GotoState>();
    state->nib = loadNib("GotoWindow");
    if (!state->nib)
        return;
    const auto windows = state->nib->windows();
    if (windows.empty())
        return;
    Size size;
    View *content = state->nib->instantiateWindow(windows.front().object, &size);
    state->window = window;
    state->go = std::move(go);
    Nib &nib = *state->nib;

    auto *field = find<TextField>(nib, "_pathFld");
    auto *message = find<Label>(nib, "_msgFld");
    if (message)
        message->setHidden(true);
    if (auto *p = nib.outlet<View>("_progress"))
        p->setHidden(true);
    Settings conf("finder");
    if (field)
        field->setText(conf.string("GoToField", "~/"));
    auto accept = [field, message] {
        if (!s_goto || !field)
            return;
        const std::string path = GoTo::expand(field->text);
        if (!isDir(path)) {
            if (message) {
                message->setText(tr("A14.1", "The folder can’t be found."));
                message->setHidden(false);
            }
            return;
        }
        Settings("finder").set("GoToField", field->text);
        auto go = s_goto->go;
        endGoto();
        if (go)
            go(path);
    };
    if (auto *b = find<Button>(nib, "_goBtn", "Go")) {
        b->isDefault = true;
        b->action = accept;
    }
    if (auto *b = find<Button>(nib, "_cancelBtn", "Cancel"))
        b->action = [] { endGoto(); };
    if (field) {
        field->onEnter = accept;
        field->onChange = [message](const std::string &) {
            if (message)
                message->setHidden(true);
        };
    }

    if (window) {
        Sheet::begin(window, content, size, sheetTop, [] {
            // Escape or the window gone: the sheet's state goes
            if (s_goto) {
                GotoState *g = s_goto.release();
                App::shared().later([g] { delete g; });
            }
        });
    } else {
        state->own = std::make_unique<Window>(size, tr("N83", "Go to Folder…"), Window::Titled);
        state->own->contentView()->addSubview(content);
        content->setFrame({0, 0, size.w, size.h});
        state->own->onClose = [] { App::shared().later([] { endGoto(); }); };
        state->own->show();
    }
    if (field) {
        Window *w = window ? window : state->own.get();
        // Tab completes the path (TGotoWindowController -autocompletePath:)
        field->onKey = [field](Event &e) {
            if (e.keysym != XKB_KEY_Tab)
                return false;
            field->setText(GoTo::complete(field->text));
            field->selectRange(field->text.size(), field->text.size());
            return true;
        };
        w->makeFirstResponder(field);
        field->selectAll();
    }
    s_goto = std::move(state);
}

// ---- Connect to Server

namespace {

struct ConnectState {
    std::unique_ptr<Nib> nib;
    std::unique_ptr<Window> window;
    std::function<void(const std::string &)> opened;
    TextField *field = nullptr;
    TableView *favorites = nullptr;
    View *progress = nullptr;
    Button *connect = nullptr;
    std::thread worker;
    std::atomic<bool> done{false};
    std::string result, error;   // the mounted folder, or why not
    int timer = 0;
    ~ConnectState()
    {
        if (worker.joinable())
            worker.join();
        if (timer)
            App::shared().removeTimer(timer);
    }
};
std::unique_ptr<ConnectState> s_connect;

std::string run(const std::vector<std::string> &argv, int &status)
{
    // a child's output, its status (no shell)
    int fds[2];
    if (pipe(fds) != 0) {
        status = -1;
        return {};
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        std::vector<char *> args;
        for (const std::string &a : argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
        out.append(buf, size_t(n));
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}

// "smb://host/share", "afp://host", "host" (smb:// by default, as 10.6's AFP is gone)
std::string normalized(std::string url)
{
    while (!url.empty() && url.back() == ' ')
        url.pop_back();
    while (!url.empty() && url.front() == ' ')
        url.erase(0, 1);
    if (!url.empty() && url.find("://") == std::string::npos)
        url = "smb://" + url;
    return url;
}

void refreshFavorites()
{
    if (!s_connect || !s_connect->favorites)
        return;
    TableView *t = s_connect->favorites;
    t->rows.clear();
    for (const std::string &f : Settings("finder").list("FavoriteServers"))
        t->rows.push_back({f});
    t->reload();
}

void connectNow()
{
    ConnectState *c = s_connect.get();
    if (!c || !c->field || c->worker.joinable())
        return;
    const std::string url = normalized(c->field->text);
    if (url.empty())
        return;
    // the recent servers (the clock menu's), latest first, ten at most
    Settings conf("finder");
    std::vector<std::string> recent = conf.list("RecentServers");
    recent.erase(std::remove(recent.begin(), recent.end(), url), recent.end());
    recent.insert(recent.begin(), url);
    if (recent.size() > 10)
        recent.resize(10);
    conf.set("RecentServers", recent);
    if (c->progress)
        c->progress->setHidden(false);
    if (c->connect)
        c->connect->setEnabled(false);
    c->done = false;
    c->worker = std::thread([c, url] {
        int status = 0;
        std::string out = run({"gio", "mount", url}, status);
        // already mounted is fine
        if (status != 0 && out.find("already mounted") == std::string::npos) {
            c->error = out;
        } else {
            std::string info = run({"gio", "info", url}, status);
            const std::string key = "local path: ";
            const size_t at = info.find(key);
            if (at != std::string::npos) {
                const size_t end = info.find('\n', at);
                c->result = info.substr(at + key.size(), end - at - key.size());
            } else {
                c->error = info;
            }
        }
        c->done = true;
    });
    c->timer = App::shared().addTimer(0.2, true, [] {
        ConnectState *c = s_connect.get();
        if (!c || !c->done)
            return;
        App::shared().removeTimer(c->timer);
        c->timer = 0;
        c->worker.join();
        if (c->progress)
            c->progress->setHidden(true);
        if (c->connect)
            c->connect->setEnabled(true);
        if (!c->result.empty()) {
            const std::string folder = c->result;
            auto opened = c->opened;
            c->result.clear();
            c->window->close();
            if (opened)
                opened(folder);
            return;
        }
        // "Unable to connect to the server at the address you specified (^0)." (A20)
        std::string msg = tr("A20", "Unable to connect to the server at the address you specified (^0).");
        const size_t mark = msg.find("^0");
        if (mark != std::string::npos)
            msg.replace(mark, 2, normalized(c->field->text));
        std::fprintf(stderr, "Finder: %s\n%s", msg.c_str(), c->error.c_str());
        c->error.clear();
        if (auto *l = c->nib->outlet<Label>("_promptLabel"))
            l->setText(msg);
    });
}

} // namespace

void ConnectToServer::show(std::function<void(const std::string &)> opened)
{
    if (s_connect && s_connect->window) {
        s_connect->opened = std::move(opened);
        s_connect->window->show();
        return;
    }
    auto c = std::make_unique<ConnectState>();
    c->nib = loadNib("ConnectToWindow");
    if (!c->nib)
        return;
    Nib &nib = *c->nib;
    const int top = nib.outletObject("_connectToWindow");
    Size size;
    View *content = nib.instantiateWindow(top >= 0 ? top : nib.windows().front().object, &size);
    c->window = std::make_unique<Window>(size, tr("PW28", "Connect to Server"), Window::Titled);
    c->window->contentView()->addSubview(content);
    content->setFrame({0, 0, size.w, size.h});
    c->opened = std::move(opened);
    c->field = nib.outlet<TextField>("_urlField");
    c->favorites = nib.outlet<TableView>("_favoritesTable");
    c->progress = nib.outlet<View>("_progressIndicator");
    if (c->progress)
        c->progress->setHidden(true);
    c->connect = find<Button>(nib, "_connectToButton", "Connect");
    if (c->connect) {
        c->connect->isDefault = true;
        c->connect->action = [] { connectNow(); };
    }
    Settings conf("finder");
    if (c->field) {
        const std::vector<std::string> recent = conf.list("RecentServers");
        c->field->setText(recent.empty() ? std::string() : recent.front());
        c->field->onEnter = [] { connectNow(); };
    }
    if (c->favorites) {
        if (c->favorites->columns.empty())
            c->favorites->columns.push_back({"", 440, false});
        c->favorites->header = false;
        c->favorites->onSelect = [](int row) {
            if (!s_connect || row < 0 || size_t(row) >= s_connect->favorites->rows.size() || !s_connect->field)
                return;
            s_connect->field->setText(s_connect->favorites->rows[size_t(row)].front());
        };
    }
    // + adds the address to the favorites; Remove takes the selected one out
    for (View *v : nib.withAction("addBookmark:"))
        if (auto *b = dynamic_cast<Button *>(v))
            b->action = [] {
                if (!s_connect || !s_connect->field)
                    return;
                const std::string url = normalized(s_connect->field->text);
                Settings conf("finder");
                std::vector<std::string> favs = conf.list("FavoriteServers");
                if (!url.empty() && std::find(favs.begin(), favs.end(), url) == favs.end()) {
                    favs.push_back(url);
                    conf.set("FavoriteServers", favs);
                }
                refreshFavorites();
            };
    for (View *v : nib.withAction("removeSelectedBookmarks:"))
        if (auto *b = dynamic_cast<Button *>(v))
            b->action = [] {
                if (!s_connect || !s_connect->favorites || s_connect->favorites->selected < 0)
                    return;
                Settings conf("finder");
                std::vector<std::string> favs = conf.list("FavoriteServers");
                const size_t row = size_t(s_connect->favorites->selected);
                if (row < favs.size())
                    favs.erase(favs.begin() + long(row));
                conf.set("FavoriteServers", favs);
                s_connect->favorites->selected = -1;
                refreshFavorites();
            };
    // Browse: the network's servers, as the sidebar's SHARED would show them (GIO's network://)
    for (View *v : nib.withAction("browseServers:"))
        if (auto *b = dynamic_cast<Button *>(v))
            b->action = [] {
                int status = 0;
                const std::string info = run({"gio", "info", "network:///"}, status);
                (void)info;
                if (s_connect && s_connect->opened) {
                    const std::string gvfs = "/run/user/" + std::to_string(getuid()) + "/gvfs";
                    if (isDir(gvfs))
                        s_connect->opened(gvfs);
                }
            };
    c->window->onClose = [] {
        App::shared().later([] { s_connect.reset(); });
    };
    s_connect = std::move(c);
    refreshFavorites();
    s_connect->window->show();
    if (s_connect->field) {
        s_connect->window->makeFirstResponder(s_connect->field);
        s_connect->field->selectAll();
    }
}

} // namespace Finder
