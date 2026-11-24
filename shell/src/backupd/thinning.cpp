// SPDX-License-Identifier: LGPL-2.1-or-later
// SnapshotAger (backupd 0x100028f4c…0x10002a4d3, decrypted) and SnapshotCuller:
//
//   - -bucketForDate: (0x100029e30): the hourlies' bucket the last 24 hours; before that buckets
//     going back from the next midnight (now + 1 day at 00:00) by the table at 0x1000655a0: thirty
//     of one day, then weeks without end; a snapshot in the bucket holding its date;
//   - a bucket's snapshots newest first: -removeNewestSnapshot takes the first, -removeOldestSnapshot
//     the last, -removeNextToOldestSnapshot the one before the last;
//   - -nextSnapshotToRemove (0x10002a2cc): the oldest bucket holding two or more gives up its newest
//     (each day, then each week, keeps its oldest); none: the hourlies give up their next to oldest
//     once they are 25 or more;
//   - -pruneExpiredSnapshots (0x100022c2f): while under five minutes and fewer than five removed.
#include "thinning.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace Thinning {

namespace {

constexpr long Day = 86400;

bool parseName(const std::string &name, std::time_t &date)
{
    std::tm tm{};
    char rest = 0;
    int n = 0;
    if (std::sscanf(name.c_str(), "%4d-%2d-%2d-%2d%2d%2d%c%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec, &rest, &n) < 6)
        return false;
    if (name.size() != 17 && (name.size() < 19 || name[17] != '-'))
        return false;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    date = std::mktime(&tm);
    return date != -1;
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

} // namespace

std::vector<Snapshot> list(const std::string &machine)
{
    std::vector<Snapshot> out;
    DIR *d = opendir(machine.c_str());
    if (!d)
        return out;
    while (dirent *e = readdir(d)) {
        Snapshot s;
        s.name = e->d_name;
        struct stat st;
        if (parseName(s.name, s.date) && stat((machine + "/" + s.name).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            out.push_back(s);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Snapshot &a, const Snapshot &b) {
        return a.date != b.date ? a.date < b.date : a.name < b.name;
    });
    return out;
}

int nextToRemove(const std::vector<Snapshot> &snapshots, std::time_t now)
{
    // the next midnight (_baseDate)
    std::time_t tomorrow = now + Day;
    std::tm tm{};
    localtime_r(&tomorrow, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    const std::time_t base = std::mktime(&tm);
    const std::time_t hourliesOldest = now - Day;

    // each snapshot's bucket: -1 the hourlies, else the bucket's start
    std::vector<int> hourlies;
    std::map<std::time_t, std::vector<int>, std::greater<std::time_t>> buckets;   // newest bucket first
    for (size_t i = 0; i < snapshots.size(); ++i) {
        const std::time_t date = snapshots[i].date;
        if (date >= hourliesOldest) {
            hourlies.push_back(int(i));
            continue;
        }
        // the table at 0x1000655a0: {1 day, 30}, {7 days, unlimited}
        std::time_t start = base;
        bool found = false;
        for (int k = 0; k < 30 && !found; ++k) {
            start -= Day;
            if (start <= date)
                found = true;
        }
        while (!found && start > 7 * Day) {
            start -= 7 * Day;
            if (start <= date)
                found = true;
        }
        if (found)
            buckets[start].push_back(int(i));
    }
    // the oldest bucket with two or more: its newest (snapshots are oldest first here)
    for (auto it = buckets.rbegin(); it != buckets.rend(); ++it)
        if (it->second.size() >= 2)
            return it->second.back();
    // the hourlies: 25 or more, the next to oldest
    if (hourlies.size() >= 25)
        return hourlies[1];
    return -1;
}

int prune(const std::string &machine, std::time_t now)
{
    const std::time_t started = std::time(nullptr);
    int removed = 0;
    while (removed < 5 && std::time(nullptr) < started + 300) {
        const std::vector<Snapshot> all = list(machine);
        const int i = nextToRemove(all, now);
        if (i < 0)
            break;
        if (!removeTree(machine + "/" + all[size_t(i)].name))
            break;
        std::fprintf(stderr, "backupd: Deleted backup %s\n", all[size_t(i)].name.c_str());
        ++removed;
    }
    return removed;
}

} // namespace Thinning
