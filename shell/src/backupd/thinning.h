// SPDX-License-Identifier: LGPL-2.1-or-later
// backupd's snapshots and their thinning: SnapshotStorage -_findSnapshots, SnapshotAger and
// SnapshotBucket, SnapshotCuller -pruneExpiredSnapshots (docs/reverse-status.md, section 37).
#pragma once

#include <ctime>
#include <string>
#include <vector>

struct Snapshot {
    std::string name;   // "2009-10-22-153045", or "…-2" when two completed in the same second
    std::time_t date = 0;
};

namespace Thinning {

// the machine folder's complete snapshots (SnapshotUtilities +pathIsSnapshot:), oldest first
std::vector<Snapshot> list(const std::string &machine);

// -pruneExpiredSnapshots: the expired ones removed, five at most and for five minutes at most;
// returns how many went
int prune(const std::string &machine, std::time_t now);

// -nextSnapshotToRemove's choice among `snapshots` (oldest first) at `now`: its index, or -1
int nextToRemove(const std::vector<Snapshot> &snapshots, std::time_t now);

} // namespace Thinning
