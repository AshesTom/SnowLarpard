// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's searches: the toolbar's search field (TSearchToolbarItem: the name typed, in This Mac
// or the window's folder, FXDefaultSearchScope) and the sidebar's SEARCH FOR, each a canned search
// (CannedSearches/<name>.cannedSearch/search.savedSearch on the DVD, their Spotlight queries):
//   Today        InRange(kMDItemLastUsedDate, $time.today, $time.today(+1)), not _kMDItemGroupId 9
//   Yesterday    InRange(kMDItemLastUsedDate, $time.today(-1), $time.today), not group 9
//   Past Week    InRange(kMDItemLastUsedDate, $time.this_week, $time.this_week(+1)), not group 9
//   All Images   _kMDItemGroupId = 13;  All Movies 7;  All Documents kMDItemContentTypeTree = public.content
// all in kMDQueryScopeComputer. No Spotlight here: a thread walks the home folder and the mounted
// volumes (no hidden items), the last use a file's latest access or change, the groups its extension's.
#pragma once

#include "files.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Finder {

class Search {
public:
    // text: the name contains it (no case); canned: "Today", "Yesterday", "Past Week", "All Images",
    // "All Movies", "All Documents"; scope: a folder, or empty for This Mac
    static std::shared_ptr<Search> start(const std::string &text, const std::string &canned, const std::string &scope);
    ~Search();
    std::vector<Item> take();   // the results found since the last call
    bool finished() const { return m_finished; }
    void cancel() { m_cancel = true; }
    static std::vector<std::string> thisMacFolders();   // where This Mac is walked

private:
    Search() = default;
    void walk(const std::string &folder, int depth);
    bool matches(const std::string &path, const std::string &name, bool folder) const;
    std::string m_text, m_canned;
    long long m_from = 0, m_to = 0;   // the canned dates' range
    std::vector<Item> m_found;
    std::mutex m_lock;
    std::atomic<bool> m_cancel{false}, m_finished{false};
    std::atomic<int> m_count{0};
    std::thread m_thread;
};

} // namespace Finder
