// SPDX-License-Identifier: LGPL-2.1-or-later
// TimeMachine.menu's code (AppleTimeMachineExtra, TMExtraView, TMSystemBackup, TMDateFormatter) on
// AquaKit's SystemUIPlugin, loaded by SystemUIServer as on the Mac. Reversed from 10.6's
// TimeMachine.menu (10.6), x86_64, with REA (docs/reverse-status.md, section 31):
//
//   - -initWithBundle: (0x18e8): the menu: the state's item (showErrorInfo:), a line, Back Up Now,
//     Enter Time Machine, Browse Other Time Machine Disks (the Option alternate of the one before), a
//     line, Open Time Machine Preferences…; a TMExtraView the bar's thickness square;
//   - -refreshUI (0x2485): no backup disk (CSBackupSettingsCopyBackupAliasData) is "Time Machine Not
//     Configured", without Back Up Now and Enter Time Machine; otherwise backupd's state: stopping
//     (_refreshBackupStopping), backing up (_refreshBackupInProgress:, the phase's title, "Stop
//     Backing Up"), or idle (_refreshBackupIdle: the last result's);
//   - TMSystemBackup -_backupdStateFromStatusDictionary: (0x396c): not Running 0, Stopping 1, else
//     the BackupPhase: Starting 2, Copying 3, Finishing 4, ThinningPreBackup 5, ThinningPostBackup 6,
//     WaitingForSpotlight 7, CalculatingChanges 8, MountingBackupVol 9, DeletingOldBackups 10;
//   - -_refreshBackupIdle (0x2164): the result (RESULT in .TimeMachine.Results.plist) 0, 13 or 22 is
//     a success ("Latest Backup: %@", else "Waiting to Back Up" or "Automatic Backups Off"), 18, 19,
//     21 "Latest Backup: Delayed…" (TMWarning), anything else "Latest Backup: Failed…" (TMError);
//   - TMExtraView -drawRect: (0x5739), -_drawClockHands (0x5ec3): TMRotatingArrow.pdf as a template
//     with a clock's hands over it, turning while a backup runs (-_startAnimation 0x5a5e: 30 pictures
//     12 degrees apart, 15 frames a second; -_animationTimerDidFire: 0x55da: the minute hand back 10
//     minutes a second, the arrow on 180 degrees a second), TMWarning.pdf or TMError.pdf alone; dimmed
//     and the hands at half alpha with automatic backups off;
//   - TMDateFormatter (0x44af): "Today, 5:40 PM" / "Yesterday, …" (backupd.bundle's MDDateFormatter
//     DATE_TIME_FORMAT with the short time), else the long date.
//
// On Linux there is no backupd yet: its settings (com.apple.TimeMachine's AutoBackup and BackupAlias,
// a backup folder here) in timemachine.conf, its results (RESULT, BACKUP_COMPLETED_DATE as seconds
// since 1970) in TimeMachine.Results.conf, its session's status (Running, Stopping, BackupPhase,
// Percent, bytes, totalBytes, TimeRemaining, pid) in backupd.status.conf, all in the shell's config
// folder, read again every second (the progress notification's place). Back Up Now starts backupd
// when the shell has one; Stop Backing Up asks it to stop (SIGTERM, the server's 'canc' message).
#include "systemuiplugin.h"

#include "foundation.h"
#include "runloop.h"

#include <signal.h>

#include <cerrno>

#include <cmath>
#include <cstdio>
#include <ctime>

using namespace AK;

namespace {

constexpr int StateIdle = 0, StateStopping = 1, StateCopying = 3, StateCalculating = 8;

// CSBackupCopySessionStatus: backupd's session, as its status file gives it
struct Status {
    int state = StateIdle;
    long long totalBytes = 0, bytesCompleted = 0;
    double percent = 0;
    int pid = 0;
};

Status sessionStatus()
{
    Status s;
    Settings st("backupd.status");
    if (!st.boolean("Running"))
        return s;
    s.pid = st.integer("pid");
    if (s.pid > 0 && kill(s.pid, 0) != 0 && errno == ESRCH)
        return s;   // the daemon died (kCSBackupNotifyDied)
    if (st.boolean("Stopping")) {
        s.state = StateStopping;
        return s;
    }
    static const std::pair<const char *, int> phases[] = {
        {"DeletingOldBackups", 10}, {"ThinningPostBackup", 6}, {"Finishing", 4},           {"WaitingForSpotlight", 7},
        {"ThinningPreBackup", 5},   {"CalculatingChanges", 8}, {"MountingBackupVol", 9}, {"Starting", 2},
        {"Copying", 3}};
    const std::string phase = st.string("BackupPhase");
    s.state = 0;
    for (const auto &[name, value] : phases)
        if (phase == name)
            s.state = value;
    if (phase.empty())
        return s;
    // -_updateFromStatusDictionary: (0x3276): Percent from 0 to 1, the bytes from Progress
    s.percent = std::stod("0" + st.string("Percent", "0"));
    if (s.percent < 0) {
        s.percent = 0;
    } else {
        s.totalBytes = std::stoll("0" + st.string("totalBytes", "0"));
        s.bytesCompleted = std::stoll("0" + st.string("bytes", "0"));
    }
    if (s.percent > 1)
        s.percent = 1;   // "backupd reported percent done > 1.0!"
    return s;
}

// NSLocalizedFileSizeDescription (humanReadableStringForBytes: in TMAdditions, 0x4ba6): 10.6's base
// 1000, the unit's fraction digits, trailing zeros dropped
std::string bytesString(long long bytes)
{
    if (bytes <= 0)
        return "Zero KB";
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
    static const int digits[] = {0, 0, 1, 2, 2, 2, 2};
    double v = double(bytes);
    int u = 0;
    auto rounded = [](double x, int d) { const double f = std::pow(10.0, d); return std::round(x * f) / f; };
    while (u < 6 && rounded(v, digits[u]) >= 1000) {
        v /= 1000;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits[u], rounded(v, digits[u]));
    std::string n = buf;
    if (n.find('.') != std::string::npos) {
        while (n.back() == '0')
            n.pop_back();
        if (n.back() == '.')
            n.pop_back();
    }
    return n + " " + units[u];
}

std::string replaced(std::string s, const std::string &what, const std::string &with)
{
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
        s.replace(at, what.size(), with);
    return s;
}

// "%1$@" and "%@" in turn
std::string format(std::string f, const std::vector<std::string> &args)
{
    size_t next = 0;
    for (size_t i = 0; i < args.size(); ++i)
        f = replaced(f, "%" + std::to_string(i + 1) + "$@", args[i]);
    for (size_t at = f.find("%@"); at != std::string::npos && next < args.size(); at = f.find("%@", at))
        f.replace(at, 2, args[next]), at += args[next++].size();
    return f;
}

} // namespace

class AppleTimeMachineExtra : public SystemUI::MenuExtra {
public:
    explicit AppleTimeMachineExtra(const std::string &bundle) : MenuExtra(bundle)
    {
        m_arrow = templateImage("TMRotatingArrow.pdf");
        m_arrowWhite = templateImage("TMRotatingArrow.pdf", true);
        m_warning = templateImage("TMWarning.pdf");
        m_warningWhite = templateImage("TMWarning.pdf", true);
        m_error = templateImage("TMError.pdf");
        m_errorWhite = templateImage("TMError.pdf", true);
        setView(new View(this));
        // backupd's progress notification and kCSBackupNotifyPrefsChanged: the files read again
        m_poll = App::shared().addTimer(1, true, [this] { refreshUI(); });
        refreshUI();
    }
    ~AppleTimeMachineExtra() override
    {
        App::shared().removeTimer(m_poll);
        stopAnimation();
    }

    std::vector<MenuItem> menu() override
    {
        refreshUI();   // -validateMenuItem: (0x1839) refreshes it as the menu opens
        std::vector<MenuItem> m;
        MenuItem state{m_stateTitle};
        state.enabled = m_stateShowsError;   // its target only for a failure: showErrorInfo:
        if (m_stateShowsError)
            state.action = [] { openPreferencePane("com.apple.prefs.backup"); };   // TMShowError
        m.push_back(state);
        m.push_back(MenuItem::line());
        if (m_configured) {
            MenuItem startStop{m_inProgress ? tr("Stop Backing Up") : tr("Back Up Now")};
            startStop.enabled = m_status.state != StateStopping;
            if (m_inProgress)
                startStop.action = [this] { stopBackingUp(); };
            else
                startStop.action = [this] { backupNow(); };
            m.push_back(startStop);
            MenuItem enter{tr("Enter Time Machine")};
            enter.action = [] { enterTimeMachine(false); };
            // Browse Other Time Machine Disks: its alternate with Option (setAlternate:, 0x80000)
            if (modifierFlags() & 0x80000) {
                enter = MenuItem{tr("Browse Other Time Machine Disks")};
                enter.action = [] { enterTimeMachine(true); };
            }
            m.push_back(enter);
        } else if (modifierFlags() & 0x80000) {
            // not configured: no longer an alternate, shown on its own with Option
            MenuItem browse{tr("Browse Other Time Machine Disks")};
            browse.action = [] { enterTimeMachine(true); };
            m.push_back(browse);
        }
        m.push_back(MenuItem::line());
        MenuItem prefs{tr("Open Time Machine Preferences\\U2026", "Open Time Machine Preferences…")};
        prefs.action = [] { openPreferencePane("com.apple.prefs.backup"); };
        m.push_back(prefs);
        return m;
    }

    bool backupEnabled() const { return m_enabled; }

private:
    std::string tr(const std::string &key, const std::string &fallback = {}) const
    {
        return localized(key, fallback.empty() ? key : fallback);
    }

    // -refreshUI
    void refreshUI()
    {
        Settings prefs("timemachine");
        m_configured = !prefs.string("BackupAlias").empty() || !prefs.string("BackupPath").empty();
        m_enabled = prefs.boolean("AutoBackup", false);
        m_status = sessionStatus();
        m_inProgress = false;
        m_stateShowsError = false;
        int indicator = 0;
        if (!m_configured) {
            m_stateTitle = tr("Time Machine Not Configured");
        } else if (m_status.state == StateStopping) {
            indicator = 1;   // _refreshBackupStopping
            m_stateTitle = tr("Stopping Backup\\U2026", "Stopping Backup…");
            m_inProgress = true;
        } else if (m_status.state != StateIdle) {
            indicator = 1;
            m_inProgress = true;
            m_stateTitle = inProgressTitle();
        } else {
            indicator = idle();
        }
        setIndicatorState(indicator);
        setNeedsDisplay();
    }

    // -_refreshBackupInProgress: (0x11e3)
    std::string inProgressTitle() const
    {
        switch (m_status.state) {
        case 1: return tr("Stopping Backup\\U2026", "Stopping Backup…");
        case StateCopying:
            if (m_status.totalBytes >= 1 && m_status.totalBytes >= m_status.bytesCompleted)
                return format(tr("Backing Up: %@ of %@", "Backing Up: %1$@ of %2$@"),
                              {bytesString(m_status.bytesCompleted), bytesString(m_status.totalBytes)});
            return m_stateTitle;   // the title left as it was
        case 4: return tr("Finishing Backup\\U2026", "Finishing Backup…");
        case 5:
        case 6: return tr("Cleaning Up\\U2026", "Cleaning Up…");
        case 7: return tr("Indexing Backup\\U2026", "Indexing Backup…");
        case StateCalculating:
            if (m_status.percent > 0 && m_status.percent <= 100) {
                // "%@ (%@)": NSNumberFormatter's percent style (3)
                char p[32];
                std::snprintf(p, sizeof p, "%.0f%%", m_status.percent * 100);
                return tr("Calculating Changes\\U2026", "Calculating Changes…") + " (" + p + ")";
            }
            return tr("Calculating Changes\\U2026", "Calculating Changes…");
        case 9: return tr("Making Backup Disk Available\\U2026", "Making Backup Disk Available…");
        case 10: return tr("Deleting Old Backups\\U2026", "Deleting Old Backups…");
        default: return tr("Preparing Backup\\U2026", "Preparing Backup…");
        }
    }

    // -_refreshBackupIdle (0x2164): the indicator for the last result
    int idle()
    {
        Settings results("TimeMachine.Results");
        const int code = results.integer("RESULT", 0);
        const unsigned bit = code >= 0 && code < 23 ? 1u << code : 0;
        if (bit & 0x402001) {   // 0, 13, 22: a success
            const long long completed = std::stoll("0" + results.string("BACKUP_COMPLETED_DATE", "0"));
            if (completed <= 0)
                m_stateTitle = m_enabled ? tr("Waiting to Back Up") : tr("Automatic Backups Off");
            else
                m_stateTitle = format(tr("Latest Backup: %@"), {dateString(std::time_t(completed))});
            return 0;
        }
        m_stateShowsError = true;
        if (bit & 0x2c0000) {   // 18, 19, 21
            m_stateTitle = tr("Latest Backup: Delayed\\U2026", "Latest Backup: Delayed…");
            return 2;
        }
        m_stateTitle = tr("Latest Backup: Failed\\U2026", "Latest Backup: Failed…");
        return 3;
    }

    // TMDateFormatter -stringFromDate:attributes:maxWidth: (1024 points: the first format fits)
    std::string dateString(std::time_t when) const
    {
        std::tm t{};
        localtime_r(&when, &t);
        std::time_t now = std::time(nullptr);
        std::tm today{};
        localtime_r(&now, &today);
        today.tm_hour = today.tm_min = today.tm_sec = 0;
        const std::time_t midnight = std::mktime(&today);
        char buf[96];
        std::string relative;   // -relativeDayStringForDate: today from midnight, yesterday a day before
        if (when < midnight + 86400) {
            if (when >= midnight)
                relative = backupdString("TODAY", "Today");
            else if (when >= midnight - 86400)
                relative = backupdString("YESTERDAY", "Yesterday");
        }
        if (!relative.empty()) {
            // DATE_TIME_FORMAT "%1$@, %2$@" with the short time style ("h:mm a")
            std::strftime(buf, sizeof buf, "%l:%M %p", &t);
            std::string time = buf;
            while (!time.empty() && time[0] == ' ')
                time.erase(0, 1);
            return format(backupdString("DATE_TIME_FORMAT", "%1$@, %2$@"), {relative, time});
        }
        // NSDateFormatterLongStyle, no time: "November 24, 2026"
        std::strftime(buf, sizeof buf, "%B %e, %Y", &t);
        return replaced(buf, "  ", " ");
    }

    // /System/Library/CoreServices/backupd.bundle's MDDateFormatter.strings
    static std::string backupdString(const std::string &key, const std::string &fallback)
    {
        return Assets::tr("/System/Library/CoreServices/backupd.bundle", "MDDateFormatter", key, fallback);
    }

    // TMSystemBackup -backupNow: (CSBackupServerProxyBackupNow): the shell's backupd, if any
    void backupNow()
    {
        const std::string apps = dirName(dirName(dirName(dirName(executablePath()))));
        const std::string backupd = apps + "/backupd.bundle/Contents/Resources/backupd";
        if (isFile(backupd))
            Task::launchDetached({backupd, "--now"});
        refreshUI();
    }

    // -stopBackingUp ('canc' to the server): stopping until backupd says otherwise
    void stopBackingUp()
    {
        if (m_status.pid > 0 && kill(m_status.pid, SIGTERM) == 0) {
            m_status.state = StateStopping;
            Settings st("backupd.status");
            st.set("Stopping", true);
        }
        refreshUI();
    }

    // com.apple.peabody.show / browseOtherDisks: Time Machine's own application (not ported yet)
    static void enterTimeMachine(bool browseOther)
    {
        const std::string apps = dirName(dirName(dirName(dirName(executablePath()))));
        const std::string tm = apps + "/Time Machine.app/Contents/MacOS/Time Machine";
        if (isFile(tm))
            Task::launchDetached(browseOther ? std::vector<std::string>{tm, "--browse-other-disks"} : std::vector<std::string>{tm});
    }

    // TMExtraView -setIndicatorState:
    void setIndicatorState(int s)
    {
        if (s == m_indicator)
            return;
        if (m_indicator == 1)
            stopAnimation();
        m_indicator = s;
        if (s == 1)
            startAnimation();
    }
    void startAnimation()
    {
        if (m_animation)
            return;
        // 15 frames a second (_frameRate)
        m_animation = App::shared().addTimer(1.0 / FrameRate, true, [this] {
            m_minute -= 10.0f / FrameRate;
            if (m_minute < 0) {
                m_hour = m_hour - 1 < 0 ? 11 : m_hour - 1;
                m_minute += 60;
            }
            m_rotation += 180.0f / FrameRate;
            if (m_rotation >= 360)
                m_rotation -= 360;
            setNeedsDisplay();
        });
    }
    void stopAnimation()
    {
        if (m_animation)
            App::shared().removeTimer(m_animation);
        m_animation = 0;
    }

    class View : public SystemUI::MenuExtraView {
    public:
        explicit View(AppleTimeMachineExtra *e) : MenuExtraView(e), extra(e) {}
        AppleTimeMachineExtra *extra;

        void draw(cairo_t *cr) override
        {
            const double w = frame().w, h = frame().h;
            const bool down = extra->isMenuDown();
            if (down)
                drawHighlight(cr, Rect{0, 0, w, h - 1});
            // the hands' centre (_drawClockHands: floor(size / 2) + 0.5 from the bottom left)
            const double cx = std::floor(w / 2) + 0.5, cy = h - (std::floor(h / 2) + 0.5);
            const bool enabled = extra->backupEnabled();
            const Image *img = nullptr;
            double turn = 0;
            switch (extra->m_indicator) {
            case 0: img = down ? &extra->m_arrowWhite : &extra->m_arrow; break;
            case 1:
                img = down ? &extra->m_arrowWhite : &extra->m_arrow;
                // the picture among 30, each 12 degrees further anticlockwise
                turn = std::floor(extra->m_rotation / 12) * 12;
                break;
            case 2: img = down ? &extra->m_warningWhite : &extra->m_warning; break;
            case 3: img = down ? &extra->m_errorWhite : &extra->m_error; break;
            default: return;
            }
            if (img->isNull())
                return;
            // _drawImageAsTemplate: the cell's picture, centred on the hands, dimmed when disabled
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_rotate(cr, -turn * M_PI / 180);
            img->draw(cr, Rect{-img->width() / 2.0, -img->height() / 2.0, double(img->width()), double(img->height())},
                      enabled ? 1 : 0.5);
            cairo_restore(cr);
            if (extra->m_indicator > 1)
                return;
            // _drawClockHands
            const double gray = down ? 1 : 0;
            cairo_set_source_rgba(cr, gray, gray, gray, enabled ? 1 : 0.5);
            cairo_arc(cr, cx, cy, 0.5, 0, 2 * M_PI);   // the oval (-0.5, -0.5, 1, 1)
            cairo_fill(cr);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
            auto hand = [&](double degrees, double length, double width) {
                const double a = degrees * M_PI / 180;   // clockwise from twelve
                cairo_move_to(cr, cx, cy);
                cairo_line_to(cr, cx + length * std::sin(a), cy - length * std::cos(a));
                cairo_set_line_width(cr, width);
                cairo_stroke(cr);
            };
            hand(extra->m_minute * 0.5 + (extra->m_hour % 12) * 30.0, 25.0 / 7, 1);   // the hour hand
            hand(extra->m_minute * 6.0, 50.0 / 11, 0.75);                             // the minute hand
        }
    };

    static constexpr float FrameRate = 15;

    Image m_arrow, m_arrowWhite, m_warning, m_warningWhite, m_error, m_errorWhite;
    Status m_status;
    std::string m_stateTitle;
    bool m_configured = false, m_enabled = false, m_inProgress = false, m_stateShowsError = false;
    int m_indicator = 0;   // _indicatorState: 0 idle, 1 backing up, 2 warning, 3 error
    // -initWithFrame:menuExtra: (0x5d4a): four o'clock
    float m_minute = 0, m_rotation = 0;
    int m_hour = 4;
    int m_poll = 0, m_animation = 0;
};

SYSTEMUI_REGISTER_EXTRA("AppleTimeMachineExtra", AppleTimeMachineExtra);
