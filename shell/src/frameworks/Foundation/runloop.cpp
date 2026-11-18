// SPDX-License-Identifier: LGPL-2.1-or-later
#include "runloop.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace AK {

double now()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// ---- RunLoop

RunLoop &RunLoop::main()
{
    static RunLoop loop;
    return loop;
}

int RunLoop::addTimer(double seconds, bool repeat, std::function<void()> fire)
{
    const int id = m_nextTimer++;
    m_timers.push_back({id, now() + seconds, seconds, repeat, std::move(fire)});
    return id;
}

void RunLoop::removeTimer(int id)
{
    m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(), [id](const Timer &t) { return t.id == id; }), m_timers.end());
}

void RunLoop::watch(int fd, std::function<void()> readable)
{
    unwatch(fd);
    m_watched.emplace_back(fd, std::move(readable));
}

void RunLoop::unwatch(int fd)
{
    m_watched.erase(std::remove_if(m_watched.begin(), m_watched.end(), [fd](const auto &w) { return w.first == fd; }), m_watched.end());
}

int RunLoop::run()
{
    m_running = true;
    while (m_running) {
        // the timers due
        const double t = now();
        std::vector<int> due;
        for (const Timer &tm : m_timers)
            if (tm.due <= t)
                due.push_back(tm.id);
        for (int id : due) {
            auto it = std::find_if(m_timers.begin(), m_timers.end(), [id](const Timer &x) { return x.id == id; });
            if (it == m_timers.end())
                continue;
            std::function<void()> fire = it->fire;
            if (it->repeat)
                it->due = t + std::max(0.001, it->interval);
            else
                m_timers.erase(it);
            fire();
        }
        if (m_source.afterTimers)
            m_source.afterTimers();
        if (!m_running)
            break;
        // the wait: until the next timer, a second at most
        double wait = 1.0;
        for (const Timer &tm : m_timers)
            wait = std::min(wait, std::max(0.0, tm.due - now()));
        if (m_source.prepare && m_source.prepare())
            wait = 0;
        std::vector<pollfd> fds;
        if (m_source.fd >= 0)
            fds.push_back({m_source.fd, POLLIN, 0});
        const size_t first = fds.size();
        for (const auto &w : m_watched)
            fds.push_back({w.first, POLLIN, 0});
        const int n = poll(fds.data(), nfds_t(fds.size()), int(std::ceil(wait * 1000)));
        if (m_source.dispatch && !m_source.dispatch(n > 0 && first > 0 && (fds[0].revents & POLLIN))) {
            m_running = false;
            return 1;
        }
        for (size_t i = first; n > 0 && i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP)))
                continue;
            // the callback may unwatch (itself or another)
            auto it = std::find_if(m_watched.begin(), m_watched.end(), [&](const auto &w) { return w.first == fds[i].fd; });
            if (it != m_watched.end()) {
                std::function<void()> f = it->second;
                f();
            }
        }
    }
    return m_code;
}

void RunLoop::stop(int code)
{
    m_code = code;
    m_running = false;
}

// ---- Task

Task::~Task()
{
    if (m_out >= 0) {
        RunLoop::main().unwatch(m_out);
        close(m_out);
    }
    if (m_in >= 0)
        close(m_in);
    if (m_pid > 0) {
        kill(m_pid, SIGTERM);
        waitpid(m_pid, nullptr, 0);
    }
}

static std::vector<std::string> mergedEnvironment(const std::map<std::string, std::string> &over)
{
    std::vector<std::string> env;
    for (char **e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const std::string key = kv.substr(0, kv.find('='));
        if (!over.count(key))
            env.push_back(kv);
    }
    for (const auto &[k, v] : over)
        env.push_back(k + "=" + v);
    return env;
}

bool Task::launch(const std::vector<std::string> &arguments)
{
    if (arguments.empty() || m_pid > 0)
        return false;
    int out[2], in[2] = {-1, -1};
    if (pipe2(out, O_CLOEXEC) != 0)
        return false;
    if (keepInput && pipe2(in, O_CLOEXEC) != 0) {
        close(out[0]);
        close(out[1]);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    if (errors == Errors::Merge)
        posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    else if (errors == Errors::Discard)
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (keepInput)
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    else
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    std::vector<std::string> args = arguments;
    std::vector<char *> argv;
    for (std::string &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env = mergedEnvironment(environment);
    std::vector<char *> envp;
    for (std::string &e : env)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    const int rc = posix_spawnp(&m_pid, argv[0], &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (keepInput)
        close(in[0]);
    if (rc != 0) {
        m_pid = -1;
        close(out[0]);
        if (keepInput)
            close(in[1]);
        return false;
    }
    m_out = out[0];
    m_in = keepInput ? in[1] : -1;
    fcntl(m_out, F_SETFL, O_NONBLOCK);
    m_buffer.clear();
    m_output.clear();
    RunLoop::main().watch(m_out, [this] { readable(); });
    return true;
}

void Task::readable()
{
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(m_out, buf, sizeof buf);
        if (n > 0) {
            m_output.append(buf, size_t(n));
            m_buffer.append(buf, size_t(n));
            for (size_t nl; (nl = m_buffer.find('\n')) != std::string::npos;) {
                const std::string line = m_buffer.substr(0, nl);
                m_buffer.erase(0, nl + 1);
                if (onLine)
                    onLine(line);
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        break;   // its output's end
    }
    RunLoop::main().unwatch(m_out);
    close(m_out);
    m_out = -1;
    if (!m_buffer.empty() && onLine)
        onLine(m_buffer);
    m_buffer.clear();
    if (m_pid > 0)
        reap();
}

// its end: the output closed, the process reaped as soon as it is gone
void Task::reap()
{
    int status = 0;
    const pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == 0) {
        RunLoop::main().addTimer(0.02, false, [this] { reap(); });   // not yet: again shortly
        return;
    }
    m_pid = -1;
    if (m_in >= 0) {
        close(m_in);
        m_in = -1;
    }
    const int code = r > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::string output = std::move(m_output);
    m_output.clear();
    if (onExit)
        RunLoop::main().later([cb = onExit, code, output = std::move(output)] { cb(code, output); });
}

void Task::write(const std::string &data)
{
    size_t done = 0;
    while (m_in >= 0 && done < data.size()) {
        const ssize_t n = ::write(m_in, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += size_t(n);
    }
}

void Task::closeInput()
{
    if (m_in >= 0)
        close(m_in);
    m_in = -1;
}

void Task::terminate()
{
    if (m_pid > 0)
        kill(m_pid, SIGTERM);
}

bool Task::launchDetached(const std::vector<std::string> &arguments)
{
    if (arguments.empty())
        return false;
    // a child that starts it and leaves at once: the program is init's, never a zombie of ours
    const pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        setsid();
        std::vector<std::string> args = arguments;
        std::vector<char *> argv;
        for (std::string &a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        _exit(rc == 0 ? 0 : 127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace AK
