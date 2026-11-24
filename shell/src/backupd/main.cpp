// SPDX-License-Identifier: LGPL-2.1-or-later
// backupd: Time Machine's engine, reversed from 10.6's backupd (/System/Library/CoreServices/
// backupd.bundle, its __TEXT protected and decrypted with tools/deprotect.py) with REA
// (docs/reverse-status.md, section 37):
//
//   - the backup database: <disk>/Backups.backupdb/<computer name>/<yyyy-MM-dd-HHmmss>/<volume>/…,
//     the snapshot made as "<date>.inProgress" (SnapshotInProgressContainer, -stampAsInProgress)
//     and renamed when complete, "Latest" a symbolic link to the newest
//     (-updateLatestSnapshotSymlinkForSnapshot:);
//   - each snapshot whole: what didn't change since the latest one linked to it (hard links: "Linked
//     source."), what did copied ("Copied %lld files (%@) from volume %@.");
//   - the exclusions: StdExclusions.plist's (PathsExcluded, ContentsExcluded, FileContentsExcluded,
//     UserPathsExcluded in each home), com.apple.TimeMachine's ExcludeByPath and SkipPaths, the
//     backup disk itself;
//   - the phases told in the session's status (kCSBackupdKeyIsRunning, kCSBackupPhaseKey: Starting,
//     ThinningPreBackup, CalculatingChanges, Copying, Finishing, ThinningPostBackup; Percent,
//     Progress' bytes and totalBytes), the result in .TimeMachine.Results.plist (RESULT,
//     BACKUP_COMPLETED_DATE), the result codes of backupd.bundle's Localizable.strings (17 no
//     backup disk, 18 not found, 2 the folder not made, 7 too large, 11 copy failed, 22 canceled);
//   - thinning (-pruneExpiredSnapshots, thinning.cpp): the hourly ones of the last 24 hours, the
//     daily ones of the last month, the weekly ones before; pre-backup thinning when the disk is too
//     full, then the oldest ones while space is still needed;
//   - com.apple.backupd-auto (launchd, StartInterval 3600): backupd-helper -auto every hour.
//
// On Linux: the user's home is the source (a session's backupd isn't root), under the volume holding
// it, named by its label ("Macintosh HD" without one); the shell's settings in timemachine.conf
// (AutoBackup, BackupPath for BackupAlias, ExcludeByPath), the results in TimeMachine.Results.conf,
// the status in backupd.status.conf, all in the config folder. "backupd --now" backs up at once,
// "backupd --auto" when automatic backups are on and the last one is an hour old, "backupd --daemon"
// does --auto every hour (the session starts it).
#include "thinning.h"

#include "foundation.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <set>

using namespace AK;

namespace {

volatile sig_atomic_t g_cancel = 0;   // the server's 'canc' message: SIGTERM here

// backupd.bundle's result codes
enum Result { OK = 0, FolderError = 2, TooLarge = 7, CopyError = 11, NoDisk = 17, DiskNotFound = 18, Canceled = 22 };

std::string isoNow(std::time_t t)
{
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d-%H%M%S", &tm);   // yyyy-MM-dd-HHmmss
    return buf;
}

// the session's status (CSBackupCopySessionStatus' dictionary)
struct Status {
    void phase(const std::string &p)
    {
        Settings s("backupd.status");
        s.set("Running", true);
        s.set("Stopping", g_cancel != 0);
        s.set("pid", int(getpid()));
        s.set("BackupPhase", p);
    }
    void progress(double percent, long long bytes, long long total)
    {
        Settings s("backupd.status");
        char p[32];
        std::snprintf(p, sizeof p, "%.4f", percent);
        s.set("Percent", std::string(p));
        s.set("bytes", std::to_string(bytes));
        s.set("totalBytes", std::to_string(total));
    }
    void done()
    {
        Settings s("backupd.status");
        s.set("Running", false);
        s.set("Stopping", false);
        s.set("BackupPhase", "");
        s.set("pid", 0);
    }
};

void writeResult(int code, bool completed)
{
    Settings r("TimeMachine.Results");
    r.set("RESULT", code);
    if (completed)
        r.set("BACKUP_COMPLETED_DATE", std::to_string(std::time(nullptr)));
}

std::string uuid()
{
    std::ifstream in("/proc/sys/kernel/random/uuid");
    std::string u;
    std::getline(in, u);
    for (char &c : u)
        c = char(std::toupper((unsigned char)c));
    return u.empty() ? std::to_string(getpid()) : u;
}

std::string computerName()
{
    char host[256] = {};
    gethostname(host, sizeof host - 1);
    std::string n = host;
    const size_t dot = n.find('.');
    return dot == std::string::npos ? n : n.substr(0, dot);
}

// the volume holding `path`: its label (/dev/disk/by-label), else the Mac's startup disk's name
std::string volumeName(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return "Macintosh HD";
    if (DIR *d = opendir("/dev/disk/by-label")) {
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.')
                continue;
            struct stat dev;
            if (stat((std::string("/dev/disk/by-label/") + e->d_name).c_str(), &dev) == 0 && dev.st_rdev == st.st_dev) {
                std::string label;
                // udev escapes as \x20
                for (const char *c = e->d_name; *c; ++c) {
                    if (c[0] == '\\' && c[1] == 'x' && c[2] && c[3]) {
                        label += char(std::stoi(std::string(c + 2, 2), nullptr, 16));
                        c += 3;
                    } else {
                        label += *c;
                    }
                }
                closedir(d);
                return label;
            }
        }
        closedir(d);
    }
    return "Macintosh HD";
}

// StdExclusions.plist's UserPathsExcluded, in the home; and their Linux counterparts
const char *const UserPathsExcluded[] = {"Library/Caches", "Library/Logs", "Library/Mirrors", ".cache", ".local/share/Trash",
                                         ".thumbnails", ".ccache", ".local/state/snowleopard"};

struct Plan {
    std::string source;                 // the home
    std::string volume;                 // its volume's name
    std::set<std::string> excluded;     // absolute paths
};

bool endsWith(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isExcluded(const Plan &plan, const std::string &path)
{
    return plan.excluded.count(path) != 0;
}

// a file unchanged since the latest snapshot: the same size, the same modification time, a file
bool unchanged(const struct stat &now, const std::string &previous)
{
    struct stat old;
    return lstat(previous.c_str(), &old) == 0 && S_ISREG(old.st_mode) && old.st_size == now.st_size &&
           old.st_mtim.tv_sec == now.st_mtim.tv_sec && old.st_mtim.tv_nsec == now.st_mtim.tv_nsec;
}

bool copyFile(const std::string &from, const std::string &to, const struct stat &st, long long &copied, const std::function<void()> &tick)
{
    const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return errno == EACCES || errno == ENOENT;   // unreadable or gone: skipped, as backupd logs and goes on
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        close(in);
        return false;
    }
    char buf[1 << 16];
    bool ok = true;
    for (;;) {
        const ssize_t n = read(in, buf, sizeof buf);
        if (n == 0)
            break;
        if (n < 0 || write(out, buf, size_t(n)) != n) {
            ok = false;
            break;
        }
        copied += n;
        tick();
        if (g_cancel) {
            ok = false;
            break;
        }
    }
    const struct timespec times[2] = {st.st_atim, st.st_mtim};
    futimens(out, times);
    close(in);
    close(out);
    return ok;
}

struct Counts {
    long long bytesToCopy = 0, items = 0;
};

// -calculating changes: what will be copied
void measure(const Plan &plan, const std::string &dir, const std::string &previous, Counts &c)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        const std::string path = dir + "/" + e->d_name;
        if (isExcluded(plan, path))
            continue;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0)
            continue;
        ++c.items;
        const std::string prev = previous.empty() ? std::string() : previous + "/" + e->d_name;
        if (S_ISDIR(st.st_mode))
            measure(plan, path, prev, c);
        else if (S_ISREG(st.st_mode) && (prev.empty() || !unchanged(st, prev)))
            c.bytesToCopy += st.st_size;
    }
    closedir(d);
}

struct Copier {
    const Plan &plan;
    Status &status;
    long long total = 0, copied = 0, files = 0, linked = 0;
    std::time_t lastReport = 0;
    bool failed = false;

    void tick()
    {
        const std::time_t now = std::time(nullptr);
        if (now != lastReport) {
            lastReport = now;
            status.progress(total ? double(copied) / double(total) : 0, copied, total);
        }
    }

    void tree(const std::string &dir, const std::string &target, const std::string &previous)
    {
        DIR *d = opendir(dir.c_str());
        if (!d)
            return;
        while (dirent *e = readdir(d)) {
            if (g_cancel || failed)
                break;
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
                continue;
            const std::string path = dir + "/" + e->d_name, to = target + "/" + e->d_name;
            if (isExcluded(plan, path))
                continue;
            struct stat st;
            if (lstat(path.c_str(), &st) != 0)
                continue;
            const std::string prev = previous.empty() ? std::string() : previous + "/" + e->d_name;
            if (S_ISDIR(st.st_mode)) {
                if (mkdir(to.c_str(), (st.st_mode & 07777) | 0700) != 0 && errno != EEXIST) {
                    failed = true;
                    break;
                }
                tree(path, to, prev);
                const struct timespec times[2] = {st.st_atim, st.st_mtim};
                utimensat(AT_FDCWD, to.c_str(), times, AT_SYMLINK_NOFOLLOW);
            } else if (S_ISLNK(st.st_mode)) {
                char link[4096];
                const ssize_t n = readlink(path.c_str(), link, sizeof link - 1);
                if (n >= 0) {
                    link[n] = 0;
                    if (symlink(link, to.c_str()) != 0 && errno != EEXIST)
                        failed = true;
                }
            } else if (S_ISREG(st.st_mode)) {
                // unchanged: a link to the latest snapshot's copy (fileLinksCreatedInCurrentCopy)
                if (!prev.empty() && unchanged(st, prev) && link(prev.c_str(), to.c_str()) == 0) {
                    ++linked;
                    continue;
                }
                if (!copyFile(path, to, st, copied, [this] { tick(); })) {
                    if (!g_cancel)
                        failed = true;
                    break;
                }
                ++files;
            }
            // sockets, pipes and devices: not backed up
        }
        closedir(d);
    }
};

long long freeBytes(const std::string &path)
{
    struct statvfs s;
    return statvfs(path.c_str(), &s) == 0 ? (long long)s.f_bavail * (long long)s.f_frsize : -1;
}

bool removeTree(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return true;
    if (S_ISDIR(st.st_mode)) {
        chmod(path.c_str(), (st.st_mode & 07777) | 0700);
        if (DIR *d = opendir(path.c_str())) {
            while (dirent *e = readdir(d))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    removeTree(path + "/" + e->d_name);
            closedir(d);
        }
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

int backup()
{
    Status status;
    status.phase("Starting");
    Settings prefs("timemachine");
    std::string disk = prefs.string("BackupPath");
    if (disk.empty())
        disk = prefs.string("BackupAlias");
    if (disk.empty()) {
        writeResult(NoDisk, false);
        status.done();
        return NoDisk;
    }
    if (!isDir(disk)) {   // MountingBackupVol: a folder on a disk that isn't there
        status.phase("MountingBackupVol");
        writeResult(DiskNotFound, false);
        status.done();
        return DiskNotFound;
    }
    const std::string machine = disk + "/Backups.backupdb/" + computerName();
    makePath(machine);
    if (!isDir(machine)) {
        writeResult(FolderError, false);
        status.done();
        return FolderError;
    }

    Plan plan;
    plan.source = home();
    plan.volume = volumeName(plan.source);
    for (const char *p : UserPathsExcluded)
        plan.excluded.insert(plan.source + "/" + p);
    for (const std::string &p : prefs.list("ExcludeByPath"))
        plan.excluded.insert(p.rfind("~/", 0) == 0 ? plan.source + p.substr(1) : p);
    for (const std::string &p : prefs.list("SkipPaths"))
        plan.excluded.insert(p.rfind("~/", 0) == 0 ? plan.source + p.substr(1) : p);
    plan.excluded.insert(disk);   // the backup disk itself (/Backups.backupdb)

    // -removeInProgressContainer: an interrupted backup's container
    if (DIR *d = opendir(machine.c_str())) {
        std::vector<std::string> stale;
        while (dirent *e = readdir(d))
            if (endsWith(e->d_name, ".inProgress"))
                stale.push_back(machine + "/" + e->d_name);
        closedir(d);
        for (const std::string &c : stale)
            removeTree(c);
    }

    // the latest snapshot: what unchanged files link to
    const std::vector<Snapshot> snapshots = Thinning::list(machine);
    const std::string latest = snapshots.empty() ? std::string() : machine + "/" + snapshots.back().name;
    const std::string previous = latest.empty() ? std::string() : latest + "/" + plan.volume + plan.source;

    status.phase("CalculatingChanges");
    status.progress(0, 0, 0);
    Counts counts;
    measure(plan, plan.source, isDir(previous) ? previous : std::string(), counts);

    // ThinningPreBackup: the expired ones first, then the oldest, while there isn't room (with 20%
    // padding, totalBytesRequiredWithPadding)
    const long long needed = counts.bytesToCopy + counts.bytesToCopy / 5;
    if (freeBytes(disk) >= 0 && freeBytes(disk) < needed) {
        status.phase("ThinningPreBackup");
        Thinning::prune(machine, std::time(nullptr));
        std::vector<Snapshot> left = Thinning::list(machine);
        while (freeBytes(disk) < needed && left.size() > 1) {
            removeTree(machine + "/" + left.front().name);   // "deleting oldest backups to make room"
            left.erase(left.begin());
        }
        if (freeBytes(disk) < needed) {
            writeResult(TooLarge, false);
            status.done();
            return TooLarge;
        }
    }

    // the snapshot, in progress: SnapshotInProgressContainer "<date>.inProgress", the snapshot a
    // UUID inside it (-createNewSnapshot)
    const std::string container = machine + "/" + isoNow(std::time(nullptr)) + ".inProgress";
    const std::string snapshot = container + "/" + uuid();
    const std::string target = snapshot + "/" + plan.volume + plan.source;
    makePath(target);
    if (!isDir(target)) {
        writeResult(FolderError, false);
        status.done();
        return FolderError;
    }
    status.phase("Copying");
    Copier copier{plan, status};
    copier.total = counts.bytesToCopy;
    copier.tree(plan.source, target, isDir(previous) ? previous : std::string());
    if (g_cancel || copier.failed) {
        removeTree(container);
        const int code = g_cancel ? Canceled : CopyError;
        writeResult(code, false);
        status.done();
        return code;
    }
    status.progress(1, copier.copied, copier.total);

    // -promoteCompletedSnapshotFromContainer: named by its completion date ("%Y-%m-%d-%H%M%S",
    // "%@-%d" from 2 while the name is taken), then "Latest" to it
    status.phase("Finishing");
    const std::string date = isoNow(std::time(nullptr));
    std::string name = date;
    for (int n = 2; isDir(machine + "/" + name) && n < 1000; ++n)
        name = date + "-" + std::to_string(n);
    const std::string done = machine + "/" + name;
    if (rename(snapshot.c_str(), done.c_str()) != 0) {
        removeTree(container);
        writeResult(FolderError, false);
        status.done();
        return FolderError;
    }
    rmdir(container.c_str());
    // -updateLatestSnapshotSymlinkForSnapshot:
    const std::string link = machine + "/Latest";
    unlink(link.c_str());
    if (symlink(name.c_str(), link.c_str()) != 0)
        std::fprintf(stderr, "backupd: Failed to make snapshot link at path: %s\n", link.c_str());
    std::fprintf(stderr, "backupd: Copied %lld files (%lld bytes) from volume %s; %lld linked.\n", copier.files, copier.copied,
                 plan.volume.c_str(), copier.linked);

    status.phase("ThinningPostBackup");
    const int removed = Thinning::prune(machine, std::time(nullptr));
    if (removed)
        std::fprintf(stderr, "backupd: Post-back up thinning complete: %d expired backups removed\n", removed);
    else
        std::fprintf(stderr, "backupd: No post-back up thinning needed: no expired backups exist\n");

    writeResult(OK, true);
    status.done();
    std::fprintf(stderr, "backupd: Backup completed successfully.\n");
    return OK;
}

// backupd-helper -auto: when automatic backups are on and the latest is an hour old
bool due()
{
    Settings prefs("timemachine");
    if (!prefs.boolean("AutoBackup", false))
        return false;
    Settings r("TimeMachine.Results");
    const long long last = std::stoll("0" + r.string("BACKUP_COMPLETED_DATE", "0"));
    return std::time(nullptr) - last >= 3600;
}

// one backupd at a time
bool lock()
{
    const std::string file = configDir() + "/backupd.lock";
    makePath(configDir());
    const int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLK, &fl) == 0;   // held until the process ends
}

} // namespace

int main(int argc, char **argv)
{
    struct sigaction sa{};
    sa.sa_handler = [](int) { g_cancel = 1; };
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    const std::string mode = argc > 1 ? argv[1] : "--now";
    if (mode == "--daemon") {
        // com.apple.backupd-auto: every hour (StartInterval 3600); checked every minute so a backup
        // asked for while asleep isn't an hour late
        while (!g_cancel) {
            if (due() && lock()) {
                const pid_t pid = fork();
                if (pid == 0)
                    _exit(backup());
                int st = 0;
                while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                    if (g_cancel)
                        kill(pid, SIGTERM);
            }
            for (int i = 0; i < 60 && !g_cancel; ++i)
                sleep(1);
        }
        return 0;
    }
    if (mode == "--auto" && !due())
        return 0;
    if (mode != "--now" && mode != "--auto") {
        std::fprintf(stderr, "usage: backupd [--now | --auto | --daemon]\n");
        return 64;
    }
    if (!lock()) {
        std::fprintf(stderr, "backupd: a backup is already running\n");
        return 1;
    }
    return backup();
}
