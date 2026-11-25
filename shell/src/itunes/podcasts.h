// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's podcasts (docs/itunes-reverse.md section 29): Advanced ▸ Subscribe to Podcast… (`pcsb`,
// FUN_000b20dc's URL box, then FUN_003cd11c), the feeds read (RSS 2.0 with the iTunes namespace, Atom),
// their episodes in the library (Podcast, Podcast URL, External GUID, Release Date, Description), the
// episodes downloaded into iTunes Music/Podcasts, Refresh (`PCRB`), Unsubscribe (`PCSB`), and the
// Podcasts pane's (DITL 254) checking, downloading and keeping. The feeds come over HTTP with curl (a
// program on the PATH: without it iTunes' own error, STR# 9008's).
#pragma once

#include "library.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace AK {
class Task;
}

namespace iTunes {

// A feed as read: the channel's title, description, author, and its items as tracks (not yet in the
// library: Location the enclosure's URL)
struct Feed {
    std::string url, title, description, author;
    std::vector<Track> episodes;
};
bool parseFeed(const std::string &xml, const std::string &url, Feed &out);

class Podcasts {
public:
    explicit Podcasts(Library &library);
    ~Podcasts();

    struct Info {
        std::string url, title, description;
        bool subscribed = true;
    };
    const std::vector<Info> &feeds() const { return m_feeds; }
    const Info *feed(const std::string &url) const;

    void subscribe(const std::string &url);
    void refresh(const std::string &url = {});   // every feed subscribed when empty
    void unsubscribe(const std::string &url);
    void download(int id);
    bool downloading(int id) const { return m_downloads.count(id) != 0; }
    bool updating() const { return !m_fetches.empty(); }
    // the Podcasts pane's "Check for new episodes": every hour, day, week (a timer's tick)
    void tick();
    void played(int id);   // its blue dot cleared

    std::function<void()> onChanged;                       // the library changed: the list again
    std::function<void(const std::string &)> onError;      // a message for an alert
    std::function<void(const std::string &)> onSubscribed; // a new feed's URL (its row opened)

private:
    void fetch(const std::string &url, bool first);
    void fetched(const std::string &url, bool first, int status, const std::string &xml);
    void applyKeep(const std::string &url);
    void save() const;
    std::string folderFor(const std::string &url) const;

    Library &m_library;
    std::vector<Info> m_feeds;
    std::map<std::string, std::unique_ptr<AK::Task>> m_fetches;
    std::map<int, std::unique_ptr<AK::Task>> m_downloads;
};

} // namespace iTunes
