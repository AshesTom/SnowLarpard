// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes DJ's choices, as iTunes 8.2.1 makes them (docs/itunes-reverse.md section 30):
//   FUN_00342bd2 (filling): nothing while iTunes DJ is hidden (+0x2A85). The current song (FUN_00342a3a:
//   the one playing when it is iTunes DJ's, else the one kept at +0x4E8, else the first not played) at
//   index c: the songs before c - (recently played) dropped; then (upcoming) - (the songs after c) chosen,
//   (upcoming) + 1 - (count) when there is no current song. Each new one flagged 0x400000 (chosen by
//   iTunes DJ) and added at the end. Then each row's flags: before c 0x800000 (played, greyed), c
//   0x1000000, the next one 0x2000000, and +0x24 its distance from c.
//   The choice without "Play higher rated songs more often" (+0x2A84 clear): the source's songs in a
//   random order (FUN_00454edc), kept with the place reached (+0x4EC, +0x4F0) from one filling to the
//   next; a song taken when iTunes DJ plays it (FUN_0034271e) and it is not in the list already
//   (FUN_000dba9e); a new order once all are gone through, and songs already there allowed once a whole
//   pass found none. With it: a random start (FUN_000da504(count)) and a random budget of 50 + 0…49; the
//   songs walked from the start, each costing 1, or its stars + 2 (at most 7) when rated (FUN_0017ac94,
//   FUN_0017ad58), until the budget is no more than the song's cost: that one is chosen.
//   FUN_00342640 (a song iTunes DJ plays): a file ('FILE'), there (+0x11), checked, without video
//   (FUN_00184e74); chosen by itself, not a podcast (FUN_000da24a) nor an audiobook (media kind 8).
#include "dj.h"

#include "foundation.h"

#include <algorithm>
#include <random>

using namespace AK;

namespace iTunes {

namespace {

std::mt19937 &rng()
{
    static std::mt19937 g(std::random_device{}());
    return g;
}

int below(int n) { return n > 0 ? int(std::uniform_int_distribution<int>(0, n - 1)(rng())) : 0; }

} // namespace

void ITunesDJ::load()
{
    Settings s("itunes");
    settings.played = s.integer("DJPlayed", 5);
    settings.upcoming = s.integer("DJUpcoming", 10);
    settings.higherRated = s.boolean("DJHigherRated", false);
    settings.requests = s.boolean("DJRequests", false);
    settings.voting = s.boolean("DJVoting", false);
    settings.password = s.boolean("DJPasswordOn", false);
    settings.message = s.string("DJMessage");
    settings.pass = s.string("DJPassword");
    source = s.string("DJSource");
    introSeen = s.boolean("DJIntroSeen", false);
    items.clear();
    std::set<int> byHand;
    for (const std::string &i : s.list("DJUser"))
        byHand.insert(std::atoi(i.c_str()));
    for (int id : m_library.djItems)
        if (m_library.track(id))
            items.push_back({id, byHand.count(int(items.size())) != 0});
    current = std::min(s.integer("DJCurrent", -1), int(items.size()) - 1);
}

void ITunesDJ::save() const
{
    Settings s("itunes");
    s.set("DJPlayed", settings.played);
    s.set("DJUpcoming", settings.upcoming);
    s.set("DJHigherRated", settings.higherRated);
    s.set("DJRequests", settings.requests);
    s.set("DJVoting", settings.voting);
    s.set("DJPasswordOn", settings.password);
    s.set("DJMessage", settings.message);
    s.set("DJPassword", settings.pass);
    s.set("DJSource", source);
    s.set("DJIntroSeen", introSeen);
    s.set("DJCurrent", current);
    std::vector<std::string> byHand;
    m_library.djItems.clear();
    for (size_t i = 0; i < items.size(); ++i) {
        m_library.djItems.push_back(items[i].track);
        if (items[i].user)
            byHand.push_back(std::to_string(i));
    }
    s.set("DJUser", byHand);
    m_library.save();
}

bool ITunesDJ::playable(const Track &t, bool byHand) const
{
    if (isStream(t) || t.hasVideo || !isFile(t.path))
        return false;
    return byHand || !(t.mediaKind & (4 | 8));
}

std::vector<int> ITunesDJ::sourceSongs() const
{
    if (!source.empty())
        for (const Playlist &p : m_library.playlists())
            if (p.persistentId == source && !p.master)
                return p.folder ? m_library.folderItems(p) : p.smart && p.info.live ? m_library.evaluate(p.info) : p.items;
    return m_library.items(Kind::Music);
}

bool ITunesDJ::sourceHasPlayable(const std::string &playlistId) const
{
    std::vector<int> songs = m_library.items(Kind::Music);
    for (const Playlist &p : m_library.playlists())
        if (p.persistentId == playlistId && !p.master)
            songs = p.folder ? m_library.folderItems(p) : p.smart && p.info.live ? m_library.evaluate(p.info) : p.items;
    for (int id : songs)
        if (const Track *t = m_library.track(id); t && playable(*t, false))
            return true;
    return false;
}

int ITunesDJ::pick(const std::vector<int> &songs, std::set<int> &inList)
{
    if (songs.empty())
        return 0;
    if (settings.higherRated) {
        // a walk from a random song with a budget of 50 to 99: each song its cost, the rated ones more
        size_t i = size_t(below(int(songs.size())));
        int budget = 50 + below(50);
        for (size_t n = 0; n < songs.size() * 100; ++n, i = (i + 1) % songs.size()) {
            const Track *t = m_library.track(songs[i]);
            const int cost = t && t->rating > 0 ? std::min(7, t->rating / 20 + 2) : 1;
            if (budget <= cost)
                return songs[i];
            budget -= cost;
        }
        return songs[i];
    }
    // the random order kept; a new one when the source changed or it was gone through
    if (m_order.size() != songs.size() || !std::is_permutation(m_order.begin(), m_order.end(), songs.begin())) {
        m_order = songs;
        std::shuffle(m_order.begin(), m_order.end(), rng());
        m_pos = 0;
    }
    for (int pass = 0; pass < 2; ++pass)
        for (size_t n = 0; n < m_order.size(); ++n) {
            if (m_pos >= m_order.size()) {
                std::shuffle(m_order.begin(), m_order.end(), rng());
                m_pos = 0;
            }
            const int id = m_order[m_pos++];
            if (pass == 1 || !inList.count(id)) {
                inList.insert(id);
                return id;
            }
        }
    return 0;
}

bool ITunesDJ::refill()
{
    bool changed = false;
    if (current >= 0 && current > settings.played) {
        const int drop = current - settings.played;
        items.erase(items.begin(), items.begin() + drop);
        current -= drop;
        changed = true;
    }
    const int count = int(items.size());
    const int need = current >= 0 ? settings.upcoming - (count - current - 1) : settings.upcoming + 1 - count;
    if (need > 0) {
        std::vector<int> songs;
        for (int id : sourceSongs())
            if (const Track *t = m_library.track(id); t && playable(*t, false))
                songs.push_back(id);
        std::set<int> inList;
        for (const Item &i : items)
            inList.insert(i.track);
        for (int k = 0; k < need; ++k) {
            const int id = pick(songs, inList);
            if (!id)
                break;
            items.push_back({id, false});
            changed = true;
        }
    }
    if (changed)
        save();
    return changed;
}

void ITunesDJ::refresh()
{
    std::vector<Item> kept;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].user || int(i) <= current)
            kept.push_back(items[i]);
    items = kept;
    m_order.clear();   // the order made again (+0x5C's 0x8000)
    refill();
    save();
}

void ITunesDJ::trimUpcoming(int n)
{
    int upcoming = int(items.size()) - current - 1;
    for (int i = int(items.size()) - 1; i > current && upcoming > n; --i)
        if (!items[size_t(i)].user) {
            items.erase(items.begin() + i);
            --upcoming;
        }
    save();
}

int ITunesDJ::add(const std::vector<int> &ids, bool next)
{
    int at = next ? current + 1 : int(items.size());
    int first = -1;
    for (int id : ids) {
        const Track *t = m_library.track(id);
        if (!t || !playable(*t, true))
            continue;
        int there = -1;
        for (size_t i = size_t(std::max(0, current + 1)); i < items.size(); ++i)
            if (items[i].track == id)
                there = int(i);
        if (!next) {
            if (there >= 0)   // already to come: left where it is
                continue;
            items.push_back({id, true});
            if (first < 0)
                first = int(items.size()) - 1;
            continue;
        }
        if (there >= 0) {
            items.erase(items.begin() + there);
            if (there < at)
                --at;
        }
        items.insert(items.begin() + at, {id, true});
        if (first < 0)
            first = at;
        ++at;
    }
    refill();
    save();
    return first;
}

void ITunesDJ::remove(const std::set<int> &indexes)
{
    std::vector<Item> kept;
    int before = 0;
    bool currentGone = false;
    for (size_t i = 0; i < items.size(); ++i) {
        if (indexes.count(int(i))) {
            if (int(i) < current)
                ++before;
            if (int(i) == current)
                currentGone = true;
            continue;
        }
        kept.push_back(items[i]);
    }
    items = kept;
    if (current >= 0)
        current = currentGone ? current - before - 1 : current - before;
    if (current >= int(items.size()))
        current = int(items.size()) - 1;
    refill();
    save();
}

void ITunesDJ::setCurrent(int index)
{
    current = index;
    refill();
    save();
}

} // namespace iTunes
