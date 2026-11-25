// SPDX-License-Identifier: LGPL-2.1-or-later
// The LIBRARY sources, as iTunes 8.2.1 has them:
//   FUN_00168254 builds the sources' rows; a distinguished playlist of the library (FUN_002e616c: its
//   +0x128 bit 1 and a kind of 2 to 7 or 0x1A) gets its group by kind (2 Movies 4, 3 TV Shows 6, 4 Music
//   3, 5 Audiobooks 8, 6 Ringtones 0xB, 7 Rented Movies 5, 0x1A 0x83), the Podcasts 7, Applications (0x12)
//   9, the radio tuner (its own source, 'tune') 10; the rows sorted by group (FUN_00166d96).
//   FUN_000dc5be: a row is hidden by a flag of the library's: Movies +0x5932, TV Shows +0x5933,
//   Audiobooks +0x5934, Applications +0x5935, Ringtones +0xF2D8, Podcasts +0x1099D, the radio +0x1099E
//   (the General pane's Show boxes, DITL 256 items 4–9 and 11).
//   FUN_002e68aa: the distinguished playlists' columns (FUN_0045386e: the field, then 1 for the sorted one):
//   Movies Name (sorted), Time, Genre, Year, My Rating, Description; TV Shows Name, Time, Show (sorted),
//   Season, Description; Audiobooks Name, Time, Artist (sorted), My Rating; Ringtones Name (sorted), Time,
//   Artist, Album. FUN_003c21fa the Podcasts': Name, Time, Release Date (sorted, descending: +7), Description
//   (125 wider).
#include "librarysources.h"

#include "foundation.h"
#include "resources.h"

#include <sstream>

using namespace AK;

namespace iTunes {

const std::vector<LibrarySource> &librarySources()
{
    // the defaults: not read (the preferences' loader not found); a fresh 8.2.1's sources (Music, Movies,
    // TV Shows, Podcasts, Radio), the others shown when one of theirs is added (showSourcesFor)
    static const std::vector<LibrarySource> sources = {
        {4, 3, "9000.036", 0, "", true},
        {2, 4, "9000.034", 1, "ShowMovies", true},
        {3, 6, "9000.035", 2, "ShowTVShows", true},
        {10, 7, "9000.030", 3, "ShowPodcasts", true},
        {5, 8, "9000.037", 5, "ShowAudiobooks", false},
        {0x12, 9, "9000.039", 27, "ShowApplications", false},
        {RadioKind, 10, "154.001", 4, "ShowRadio", true},
        {6, 0xb, "9000.041", 24, "ShowRingtones", false},
    };
    return sources;
}

bool sourceShown(const LibrarySource &s)
{
    return !*s.showKey || Settings("itunes").boolean(s.showKey, s.shownAtFirst);
}

bool showSourcesFor(int k)
{
    Settings s("itunes");
    bool changed = false;
    auto show = [&](const char *key, bool when) {
        if (when && !s.boolean(key, false)) {
            s.set(key, true);
            changed = true;
        }
    };
    show("ShowMovies", (k & 0x8006) == 2);
    show("ShowTVShows", (k & 0x8044) == 0x40);
    show("ShowAudiobooks", (k & 0x800c) == 8);
    show("ShowRingtones", (k & 0x4000) != 0);
    show("ShowApplications", (k & 0x61000) != 0);
    return changed;
}

namespace {

std::string columnsKey(int kind) { return "Columns." + std::to_string(kind); }

std::vector<int> defaults(int kind)
{
    switch (kind) {
    case 2: return {2, 13, 8, 7, 25, 54};
    case 3: return {2, 13, 62, 63, 54};
    case 5: return {2, 13, 4, 25};
    case 6: return {2, 13, 4, 3};
    case 10: return {2, 13, 34, 54};
    case RadioKind: return {2, 5, 14};   // Stream, Bit Rate, Comments (by eye)
    case 0x12: return {2, 4, 8, 9};      // not read (no applications here)
    }
    return {};
}

} // namespace

std::vector<int> sourceColumns(int kind, const std::vector<int> &music)
{
    if (kind == 4 || kind == 0)
        return music;
    std::vector<int> cols;
    std::istringstream in(Settings("itunes").string(columnsKey(kind)));
    for (int f; in >> f;)
        cols.push_back(f);
    return cols.empty() ? defaults(kind) : cols;
}

void keepSourceColumns(int kind, const std::vector<int> &columns)
{
    std::string s;
    for (int f : columns)
        s += (s.empty() ? "" : " ") + std::to_string(f);
    Settings("itunes").set(kind == 4 || kind == 0 ? std::string("Columns") : columnsKey(kind), s);
}

int sourceSortField(int kind, bool &ascending)
{
    ascending = true;
    switch (kind) {
    case 3: return 62;
    case 5: return 4;
    case 10: ascending = false; return 34;
    case RadioKind: return 2;
    }
    return 2;
}

std::string sourceColumnTitle(int kind, int field, const std::string &title)
{
    if (field == 2 && kind == 10)
        return tr("9006.032");   // Podcast
    if (field == 2 && kind == RadioKind)
        return tr("141.001");    // Stream
    if (field == 14 && kind == RadioKind)
        return "Comments";       // by eye (STR# 9006's is "Comment")
    if (field == 54 && kind == 10)
        return title;
    return title;
}

} // namespace iTunes
