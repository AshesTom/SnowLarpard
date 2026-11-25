// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's iTunes DJ (the Party Shuffle of iTunes 4 to 7: the playlist of kind 0x16, its row group
// 0x82; docs/itunes-reverse.md section 30). A playlist that fills itself from a source (the library's
// Music or a playlist), keeping a number of the songs played before the one playing and a number of
// songs to come; its settings the application's +0x2A84 (Play higher rated songs more often), +0x2A86
// (recently played), +0x2A8A (upcoming), +0x2A85 (hidden: the General pane's box), +0x2A96 (the intro
// seen), +0xF551… (the Remote's requests).
#pragma once

#include "library.h"

#include <set>
#include <string>
#include <vector>

namespace iTunes {

// The sources' rows of iTunes DJ and Genius (under PLAYLISTS: groups 0x82 and 0x83)
constexpr int DJTag = -160, GeniusTag = -161;

struct DJSettings {
    // PartyShuffleSettings.nib: Display (popup 4) recently played songs, (popup 6) upcoming songs; the
    // menus' 0, 5, 10, 15, 20, 25, 50, 75, 100 (FUN_002b0246: the item n / 5 + 1 below 50, else n / 25 + 5)
    int played = 5, upcoming = 10;
    bool higherRated = false;   // box 8, +0x2A84
    // the Remote's: box 10 (+0xF551), the welcome message 12 (+0xF552), voting 13 (+0xF753), password
    // 15 / 16 (+0xF752, +0xF754)
    bool requests = false, voting = false, password = false;
    std::string message, pass;
};

class ITunesDJ {
public:
    explicit ITunesDJ(Library &library) : m_library(library) {}

    struct Item {
        int track = 0;
        bool user = false;   // added by hand (no 0x400000: FUN_0034375e); kept when the list is trimmed
    };
    DJSettings settings;
    std::string source;      // a playlist's Persistent ID; empty: the library's Music (FUN_00343558's flag 8)
    bool introSeen = false;  // +0x2A96 (`PDon`)
    std::vector<Item> items;
    int current = -1;        // the song playing or the last played (+0x4E8): its index

    void load();
    void save() const;

    // FUN_00342640: a song iTunes DJ plays — a file (not a stream), there, no video; chosen by itself
    // (not `byHand`), neither a podcast nor an audiobook
    bool playable(const Track &t, bool byHand) const;
    // The source's songs (FUN_00342954), and whether it has one iTunes DJ plays (FUN_003429ba)
    std::vector<int> sourceSongs() const;
    bool sourceHasPlayable(const std::string &playlistId) const;

    // FUN_00342bd2: the songs played beyond the number kept dropped from the top, then songs chosen
    // until the upcoming number is there: false when nothing changed
    bool refill();
    // FUN_003433fc (Refresh, `PSRB`): the songs it chose itself (not played, not playing, not added by
    // hand) taken out, then filled again
    void refresh();
    // FUN_00342adc: the upcoming number lowered: the songs it chose itself taken from the end
    void trimUpcoming(int n);
    // FUN_0034375e: songs added by hand — at the end (Add to iTunes DJ, `psad`: one already there to come
    // is left) or after the one playing (Play Next in iTunes DJ `pspn`, Play in iTunes DJ `psnw`: one
    // already there is moved); the index of the first, -1 when none
    int add(const std::vector<int> &ids, bool next);
    void remove(const std::set<int> &indexes);
    // the song at `index` playing: the ones before it played (0x800000: greyed), filled again
    void setCurrent(int index);
    // A row's state (FUN_00342bd2's flags): played (before the current), the current, to come
    bool played(int index) const { return current >= 0 && index < current; }

private:
    int pick(const std::vector<int> &songs, std::set<int> &inList);
    Library &m_library;
    // the source's songs in a random order (FUN_00454edc), and where the last choice was (+0x4EC, +0x4F0)
    std::vector<int> m_order;
    size_t m_pos = 0;
};

} // namespace iTunes
