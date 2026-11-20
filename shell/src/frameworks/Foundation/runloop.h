// SPDX-License-Identifier: LGPL-2.1-or-later
// NSRunLoop and NSTask for AquaKit: the thread's loop of timers and watched file descriptors, which
// AppKit's App runs with the display as one more source, and child processes whose output and end it
// reports. Foundation's, as on the Mac, so that what isn't a window (a menu extra's state, a helper
// program) needs no AppKit.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace AK {

double now();   // seconds, monotonic

class RunLoop {
public:
    static RunLoop &main();

    // NSTimer: in seconds; the callback may remove its own
    int addTimer(double seconds, bool repeat, std::function<void()> fire);
    void removeTimer(int id);
    void later(std::function<void()> f) { addTimer(0, false, std::move(f)); }
    // a file descriptor: `readable` when there is something to read (or it is closed)
    void watch(int fd, std::function<void()> readable);
    void unwatch(int fd);

    // The loop: the timers due, then a wait for the descriptors until the next timer. A source of
    // its own (AppKit's display connection) is turned with it: `prepare` before each wait (drawing,
    // flushing; true when it already has events), `dispatch` after it (false: the source is gone and
    // run() returns 1).
    struct Source {
        int fd = -1;
        std::function<bool()> prepare;
        std::function<bool(bool readable)> dispatch;
        std::function<void()> afterTimers;   // after the timers fired (the windows redrawn)
    };
    void setSource(Source s) { m_source = std::move(s); }
    int run();
    void stop(int code = 0);
    bool isRunning() const { return m_running; }

private:
    struct Timer {
        int id;
        double due, interval;
        bool repeat;
        std::function<void()> fire;
    };
    std::vector<Timer> m_timers;
    std::vector<std::pair<int, std::function<void()>>> m_watched;
    Source m_source;
    int m_nextTimer = 1;
    bool m_running = false;
    int m_code = 0;
};

// NSTask: a program started with its arguments (looked up on the PATH), its standard output read on
// the run loop line by line or whole, its end reported; its standard input written to
class Task {
public:
    Task() = default;
    ~Task();   // a task still running is terminated
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    std::function<void(const std::string &line)> onLine;     // each line of its output, without '\n'
    // its exit status (-1: a signal) and all it wrote, on the next turn of the run loop (the callback
    // may delete the task)
    std::function<void(int status, const std::string &output)> onExit;
    enum class Errors { Discard, Inherit, Merge };
    Errors errors = Errors::Discard;   // its standard error: to /dev/null, ours, or with its output
    bool keepInput = false;        // a pipe to its standard input (write()), else /dev/null
    std::map<std::string, std::string> environment;   // set over the inherited ones

    bool launch(const std::vector<std::string> &arguments);
    void write(const std::string &data);
    void closeInput();
    void terminate();
    bool isRunning() const { return m_pid > 0; }
    pid_t pid() const { return m_pid; }

    // A fire-and-forget program (QProcess::startDetached): its end not waited for
    static bool launchDetached(const std::vector<std::string> &arguments);

private:
    void readable();
    void reap();
    pid_t m_pid = -1;
    int m_out = -1, m_in = -1;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);   // its callbacks on the run loop check it
    std::string m_buffer, m_output;
};

} // namespace AK
