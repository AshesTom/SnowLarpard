// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's LIBRARY sources (docs/itunes-reverse.md section 29): Music, Movies, TV Shows, Podcasts,
// Audiobooks, Applications, Radio, Ringtones — each shown or not by the General pane's Show boxes — their
// icons, the media kinds each takes and the columns each opens with.
#pragma once

#include <string>
#include <vector>

namespace iTunes {

struct LibrarySource {
    int kind;              // the distinguished kind (+0xE8; Radio: its own source, 'tune')
    int group;             // FUN_00168254's row group: the order under LIBRARY
    const char *nameKey;   // STR# 9000's (Radio 154.001)
    int icon;              // Source Icons (441)'s row
    const char *showKey;   // the General pane's box ("" Music: always)
    bool shownAtFirst;
};

constexpr int RadioKind = 0x74756e65;   // 'tune'
// The sources' rows carry -100 - kind (Music keeps -1)
inline int sourceTag(int kind) { return kind == 4 ? -1 : kind == RadioKind ? -150 : -100 - kind; }
inline int kindForTag(int tag) { return tag == -1 ? 4 : tag == -150 ? RadioKind : tag <= -100 && tag > -150 ? -100 - tag : 0; }

const std::vector<LibrarySource> &librarySources();   // in their groups' order
bool sourceShown(const LibrarySource &s);
// The kinds of the tracks just added: their sources shown (FUN_002045c4 and FUN_001517f8 clear the
// hide flags of Movies, TV Shows, Audiobooks, Ringtones and Applications when one of theirs comes in)
bool showSourcesFor(int mediaKinds);

// A source's columns (View Options, kept per source; FUN_002e68aa's, FUN_003c21fa's at first) and the
// one it is sorted by
std::vector<int> sourceColumns(int kind, const std::vector<int> &music);
void keepSourceColumns(int kind, const std::vector<int> &columns);
int sourceSortField(int kind, bool &ascending);
// A column's title in that source (the Podcasts' Name column is "Podcast", the Radio's "Stream")
std::string sourceColumnTitle(int kind, int field, const std::string &title);

} // namespace iTunes
