// SPDX-License-Identifier: LGPL-2.1-or-later
#include "panels.h"

#include "controls.h"
#include "foundation.h"
#include "text.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace AK {

namespace {

// A chooser's answer on its standard output, without blocking the windows; `ran` false when the
// program isn't there. Its environment without the portals (they would ask another process, which
// the session may not have) and on X11, where WindowServer frames it.
void runDialog(std::vector<std::string> argv, std::function<void(bool ran, std::string)> done)
{
    int out[2];
    if (pipe(out) != 0)
        return done(false, {});
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    std::vector<std::string> env;
    for (char **e = environ; *e; ++e) {
        const std::string v = *e;
        const std::string name = v.substr(0, v.find('='));
        if (name == "QT_QPA_PLATFORM" || name == "QT_QPA_PLATFORMTHEME" || name == "GTK_USE_PORTAL" || name == "GDK_BACKEND" ||
            name == "KDE_FULL_SESSION" || name == "PLASMA_INTEGRATION_USE_PORTAL" || name == "GDK_DEBUG")
            continue;
        env.push_back(v);
    }
    for (const char *v : {"QT_QPA_PLATFORM=xcb", "QT_QPA_PLATFORMTHEME=generic", "PLASMA_INTEGRATION_USE_PORTAL=0", "GTK_USE_PORTAL=0",
                          "GDK_BACKEND=x11", "GDK_DEBUG=no-portals"})
        env.push_back(v);
    std::vector<char *> envp;
    for (std::string &v : env)
        envp.push_back(v.data());
    envp.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        return done(false, {});
    }
    auto answer = std::make_shared<std::string>();
    const int fd = out[0];
    App::shared().watch(fd, [fd, pid, answer, done]() {
        char buf[4096];
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            answer->append(buf, size_t(n));
            return;
        }
        App::shared().unwatch(fd);
        close(fd);
        int status = 0;
        waitpid(pid, &status, 0);
        std::string a = *answer;
        while (!a.empty() && (a.back() == '\n' || a.back() == '\r'))
            a.pop_back();
        // a chooser cancelled answers nothing (kdialog and zenity exit with 1)
        done(true, WIFEXITED(status) && WEXITSTATUS(status) == 0 ? a : std::string());
    });
}

std::string join(const std::vector<std::string> &v, const char *sep)
{
    std::string out;
    for (const std::string &s : v)
        out += (out.empty() ? "" : sep) + s;
    return out;
}

std::vector<std::string> lines(const std::string &s)
{
    std::vector<std::string> out;
    size_t from = 0;
    while (from < s.size()) {
        size_t to = s.find('\n', from);
        if (to == std::string::npos)
            to = s.size();
        if (to > from)
            out.push_back(s.substr(from, to - from));
        from = to + 1;
    }
    return out;
}

// kdialog's filter: "*.rtf *.txt|Name" a line each; zenity's: --file-filter=Name | *.rtf *.txt
std::string kdialogFilters(const std::vector<FileFilter> &filters)
{
    std::vector<std::string> parts;
    for (const FileFilter &f : filters)
        parts.push_back(join(f.patterns, " ") + "|" + f.name);
    return join(parts, "\n");
}

void zenityFilters(std::vector<std::string> &argv, const std::vector<FileFilter> &filters)
{
    for (const FileFilter &f : filters)
        argv.push_back("--file-filter=" + f.name + " | " + join(f.patterns, " "));
}

} // namespace

// ---- NSOpenPanel, NSSavePanel

void OpenPanel::run(const std::string &title, const std::string &directory, const std::vector<FileFilter> &filters, bool multiple,
                    std::function<void(std::vector<std::string>)> done)
{
    static bool running = false;   // one panel at a time, as the Mac's application modal one
    if (running)
        return;
    running = true;
    auto finish = [done](const std::string &answer) {
        running = false;
        done(lines(answer));
    };
    std::vector<std::string> k = {"kdialog", "--title", title};
    if (multiple)
        k.insert(k.end(), {"--multiple", "--separate-output"});
    k.insert(k.end(), {"--getopenfilename", directory});
    if (!filters.empty())
        k.push_back(kdialogFilters(filters));
    runDialog(k, [finish, title, directory, filters, multiple](bool ran, std::string answer) {
        if (ran)
            return finish(answer);
        std::vector<std::string> z = {"zenity", "--file-selection", "--title=" + title, "--filename=" + directory + "/"};
        if (multiple)
            z.insert(z.end(), {"--multiple", "--separator=\n"});
        zenityFilters(z, filters);
        runDialog(z, [finish](bool ran2, std::string a) {
            if (!ran2)
                std::fprintf(stderr, "AppKit: the open panel needs kdialog or zenity\n");
            finish(a);
        });
    });
}

void OpenPanel::chooseDirectory(const std::string &title, const std::string &directory, std::function<void(std::string)> done)
{
    static bool running = false;
    if (running)
        return;
    running = true;
    auto finish = [done](const std::string &answer) {
        running = false;
        const std::vector<std::string> l = lines(answer);
        done(l.empty() ? std::string() : l.front());
    };
    runDialog({"kdialog", "--title", title, "--getexistingdirectory", directory}, [finish, title, directory](bool ran, std::string answer) {
        if (ran)
            return finish(answer);
        runDialog({"zenity", "--file-selection", "--directory", "--title=" + title, "--filename=" + directory + "/"},
                  [finish](bool ran2, std::string a) {
                      if (!ran2)
                          std::fprintf(stderr, "AppKit: the open panel needs kdialog or zenity\n");
                      finish(a);
                  });
    });
}

void SavePanel::run(const std::string &title, const std::string &start, const std::vector<FileFilter> &filters,
                    std::function<void(std::string)> done)
{
    static bool running = false;
    if (running)
        return;
    running = true;
    auto finish = [done](const std::string &answer) {
        running = false;
        done(answer);
    };
    std::vector<std::string> k = {"kdialog", "--title", title, "--getsavefilename", start};
    if (!filters.empty())
        k.push_back(kdialogFilters(filters));
    runDialog(k, [finish, title, start, filters](bool ran, std::string answer) {
        if (ran)
            return finish(answer);
        std::vector<std::string> z = {"zenity", "--file-selection", "--save", "--confirm-overwrite", "--title=" + title, "--filename=" + start};
        zenityFilters(z, filters);
        runDialog(z, [finish](bool ran2, std::string a) {
            if (!ran2)
                std::fprintf(stderr, "AppKit: the save panel needs kdialog or zenity\n");
            finish(a);
        });
    });
}

// ---- NSAlert

namespace {

constexpr double AlertWidth = 420;

// The alert's views (NSAlert's layout: the 64 point icon at 24, 15; the text from 105, 20 from the
// right; the buttons 32 high from the right, 20 in, a third at the left under the text) in `c`, its
// height returned; `finish` given the button's index
double buildAlert(View *c, const Image &icon, const std::string &message, const std::string &informative,
                  const std::vector<std::string> &buttons, std::function<void(int)> finish)
{
    const double width = AlertWidth;
    const double textWidth = width - 105 - 20;
    const double mh = std::ceil(wrappedHeight(message, textWidth, Font::boldSystem(13)));
    const double ih = informative.empty() ? 0 : std::ceil(wrappedHeight(informative, textWidth, Font::system(11)));
    const double height = std::max(15 + 64.0, 15 + mh + (ih ? 8 + ih : 0)) + 20 + 32 + 10;
    auto *iv = c->add<ImageView>();
    iv->image = icon.isNull() ? icon : icon.scaled(64, 64);
    iv->setFrame({24, 15, 64, 64});
    auto *m = c->add<Label>(message, Font::boldSystem(13));
    m->wrap = true;
    m->setFrame({105, 15, textWidth, mh});
    if (!informative.empty()) {
        auto *i = c->add<Label>(informative, Font::system(11));
        i->wrap = true;
        i->setFrame({105, 15 + mh + 8, textWidth, ih});
    }
    double x = width - 20;
    for (size_t b = 0; b < buttons.size(); ++b) {
        auto *button = c->add<Button>(buttons[b]);
        const double w = std::max(82.0, std::ceil(AK::textWidth(buttons[b], Font::system())) + 28);
        const double bx = b == 2 ? 105 - 6 : x - w;
        button->setFrame({bx, height - 32 - 10, w, 32});
        button->isDefault = b == 0;
        button->action = [finish, b] { finish(int(b)); };
        if (b != 2)
            x -= w;
    }
    return height;
}

// Return: the default button; Escape: Cancel (the second of two or three)
int keyAnswer(const Event &e, size_t buttons)
{
    if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter)
        return 0;
    if (e.keysym == XKB_KEY_Escape && buttons > 1)
        return 1;
    return -1;
}

class AlertWindow {
public:
    AlertWindow(const Image &icon, const std::string &message, const std::string &informative, std::vector<std::string> buttons,
                std::function<void(int)> done)
        : m_done(std::move(done)), m_buttons(buttons.size())
    {
        m_window = std::make_unique<Window>(Size{AlertWidth, 100}, "");
        const double h = buildAlert(m_window->contentView(), icon, message, informative, buttons, [this](int b) { finish(b); });
        m_window->setContentSize({AlertWidth, h});
        m_window->onClose = [this] { finish(m_buttons > 1 ? 1 : 0); };
        m_window->onUnhandledKey = [this](Event &e) {
            if (const int b = keyAnswer(e, m_buttons); b >= 0)
                finish(b);
        };
        m_window->show();
    }

private:
    void finish(int button)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_window->onClose = nullptr;
        m_window->close();
        auto done = std::move(m_done);
        App::shared().later([this] { delete this; });
        if (done)
            done(button);
    }
    std::unique_ptr<Window> m_window;
    std::function<void(int)> m_done;
    size_t m_buttons;
    bool m_finished = false;
};

// The sheet's content: it answers Return and Escape while it has the keyboard
class AlertSheetView : public View {
public:
    std::function<void(Event &)> onKey;
    bool acceptsFirstResponder() const override { return true; }
    void keyDown(Event &e) override
    {
        if (onKey)
            onKey(e);
    }
};

} // namespace

void Alert::run(const Image &icon, const std::string &message, const std::string &informative, std::vector<std::string> buttons,
                std::function<void(int)> done)
{
    new AlertWindow(icon, message, informative, std::move(buttons), std::move(done));
}

void Alert::beginSheet(Window *window, const Image &icon, const std::string &message, const std::string &informative,
                       std::vector<std::string> buttons, std::function<void(int)> done)
{
    if (!window || Sheet::isOpen(window))
        return run(icon, message, informative, std::move(buttons), std::move(done));
    auto *content = new AlertSheetView;
    auto finished = std::make_shared<bool>(false);
    auto finish = [window, done, finished](int b) {
        if (*finished)
            return;
        *finished = true;
        Sheet::end(window);
        // after the sheet's views are gone
        App::shared().later([done, b] {
            if (done)
                done(b);
        });
    };
    const size_t count = buttons.size();
    const double h = buildAlert(content, icon, message, informative, buttons, finish);
    content->onKey = [finish, count](Event &e) {
        if (const int b = keyAnswer(e, count); b >= 0)
            finish(b);
    };
    Sheet::begin(window, content, {AlertWidth, h}, 0);
    window->makeFirstResponder(content);
}

// ---- NSWorkspace

void Workspace::openFile(const std::string &path)
{
    std::vector<std::string> argv = {"xdg-open", path};
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) == 0)
        App::shared().addTimer(10, false, [pid] { waitpid(pid, nullptr, WNOHANG); });
}

bool Workspace::inPath(const char *program)
{
    const char *path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos)
            end = dirs.size();
        if (access((dirs.substr(start, end - start) + "/" + program).c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

// ---- one instance: an abstract socket by user and display; a launch's files a path a line

namespace {

sockaddr_un instanceAddress(const std::string &program, socklen_t &length)
{
    const char *display = std::getenv("DISPLAY");
    const char *wayland = std::getenv("WAYLAND_DISPLAY");
    const std::string name = "snowleopard-" + program + "-" + std::to_string(getuid()) + "-" +
                             (wayland && *wayland ? wayland : display ? display : "");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const size_t n = std::min(name.size(), sizeof addr.sun_path - 1);
    std::memcpy(addr.sun_path + 1, name.data(), n);   // sun_path[0] = 0: abstract
    length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n);
    return addr;
}

} // namespace

bool Instance::handToRunning(const std::string &name, const std::vector<std::string> &files)
{
    socklen_t length = 0;
    const sockaddr_un addr = instanceAddress(name, length);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    if (connect(fd, reinterpret_cast<const sockaddr *>(&addr), length) != 0) {
        close(fd);
        return false;
    }
    std::string msg = "open\n";
    for (const std::string &f : files) {
        // the path whole: the running one has its own working directory
        char *whole = realpath(f.c_str(), nullptr);
        msg += (whole ? std::string(whole) : f) + "\n";
        std::free(whole);
    }
    for (size_t done = 0; done < msg.size();) {
        const ssize_t n = write(fd, msg.data() + done, msg.size() - done);
        if (n <= 0)
            break;
        done += size_t(n);
    }
    close(fd);
    return true;
}

void Instance::listen(const std::string &name, std::function<void(std::vector<std::string>)> open)
{
    socklen_t length = 0;
    const sockaddr_un addr = instanceAddress(name, length);
    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server < 0)
        return;
    if (bind(server, reinterpret_cast<const sockaddr *>(&addr), length) != 0 || ::listen(server, 4) != 0) {
        close(server);
        return;
    }
    App::shared().watch(server, [server, open] {
        for (int c; (c = accept4(server, nullptr, nullptr, SOCK_CLOEXEC)) >= 0;) {
            std::string text;
            char buf[4096];
            for (ssize_t n; (n = read(c, buf, sizeof buf)) > 0;)
                text.append(buf, size_t(n));
            close(c);
            std::vector<std::string> paths = lines(text);
            if (!paths.empty() && paths.front() == "open")
                paths.erase(paths.begin());
            open(paths);
        }
    });
}

} // namespace AK
