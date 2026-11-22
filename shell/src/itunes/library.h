// SPDX-License-Identifier: LGPL-2.1-or-later
// The iTunes library: its tracks and playlists, kept where iTunes keeps them,
// ~/Music/iTunes/iTunes Music Library.xml, in the format iTunes writes it (a plist: Major Version 1,
// Minor Version 1, Application Version, Music Folder, Tracks by Track ID, Playlists with their Playlist
// Items). The files' tags are TagLib's. At the first launch, the music folder (XDG_MUSIC_DIR) is added.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace iTunes {

struct Track {
    int id = 0;
    std::string name, artist, album, genre, kind, path;
    long long size = 0;
    int totalTime = 0;   // milliseconds
    int trackNumber = 0, year = 0, bitRate = 0, sampleRate = 0, playCount = 0;
    std::string dateAdded;   // ISO 8601, as the plist's <date>
    // Get Info's: the plist's Composer, Album Artist, Grouping, Comments, Track Count, Disc Number,
    // Disc Count, BPM; Rating 0 to 100 (20 a star)
    std::string composer, albumArtist, grouping, comments;
    int trackCount = 0, discNumber = 0, discCount = 0, bpm = 0, rating = 0;
};

// A smart playlist's rules (SmartPlaylist.nib; docs/itunes-reverse.md section 11): a rule is a field
// (iTunes' field numbers: 2 Name, 3 Album, 4 Artist…), an operator (its codes: 0x01000002 contains…)
// and a value: text, or numbers (from, to), and for the dates' "in the last" a unit (1 day, 7 a
// week, 30 a month)
struct SmartRule {
    int field = 4;
    uint32_t op = 0x01000002;
    std::string text;
    long long from = 0, to = 0;
    int unit = 1;
};
struct SmartInfo {
    bool any = false;           // Match any / all of the rules
    bool useRules = true;       // the Match box
    bool limit = false;
    int limitValue = 25;
    int limitUnit = 4;          // MENU 304's: 0 minutes, 1 hours, 2 MB, 3 GB, 4 items
    int limitOrder = 0;         // MENU 305's entries, separators left out: 0 random, 1 album…
    bool checkedOnly = false;
    bool live = true;
    std::vector<SmartRule> rules;
};

struct Playlist {
    std::string name;
    std::vector<int> items;   // Track IDs
    bool master = false;      // "Library"
    bool smart = false;
    SmartInfo info;
    // a folder (New Playlist Folder: FUN_002e5f90, its flag 2 at +0x128): its songs those of the
    // playlists in it; the plist's Folder, Playlist Persistent ID and Parent Persistent ID
    bool folder = false;
    std::string persistentId, parentId;
};

class Library {
public:
    static std::string musicFolder();   // XDG_MUSIC_DIR, else ~/Music
    std::string file() const;           // ~/Music/iTunes/iTunes Music Library.xml
    bool load();
    bool save() const;

    // Files and folders (their audio files, recursively) added: the tracks new to the library
    int add(const std::vector<std::string> &paths);
    bool isAudio(const std::string &path) const;
    // Advanced ▸ Open Audio Stream (FUN_0017fd64): the URL's track ('HTTP', Track Type URL), the one
    // already there with that URL or a new one named by the URL's file name (its last path component,
    // the query left out), else its path, else the whole URL; its Track ID
    int addURL(const std::string &url);

    const std::map<int, Track> &tracks() const { return m_tracks; }
    const Track *track(int id) const;
    int idForPath(const std::string &path) const;   // 0: not in the library
    Track *track(int id);
    std::vector<Playlist> &playlists() { return m_playlists; }
    void remove(int id);
    // Get Info's changes into the song's file (TagLib) as well as the library: false when the file
    // could not be written
    bool writeTags(const Track &t);
    // A new playlist's Persistent ID (16 hex digits, as the plist has them)
    static std::string newPersistentId();
    // A folder's songs: those of the playlists in it and in its folders, each once, in their order
    std::vector<int> folderItems(const Playlist &folder) const;
    // A smart playlist's songs: the rules applied to the library, then the limit
    std::vector<int> evaluate(const SmartInfo &info) const;

private:
    void addFile(const std::string &path);
    std::map<int, Track> m_tracks;
    std::vector<Playlist> m_playlists;
    int m_nextId = 100;
};

// File ▸ Show Duplicates (FUN_00138cea): the songs sorted by name, each compared with those after it
// until one differs; a pair is the same media kind (audio or video: FUN_001827a0) with the same name and
// artist (CFString compares, case ignored); Show Exact Duplicates (the menu item with Option, bit 11 of
// the modifiers: FUN_003ae488) also the same album, track number, track count, disc number and disc
// count (+0xaa to +0xb0). Both songs of a pair are marked (0x200000 in the item's flags); the marked
// ones, in the list's order, are what the list then shows
std::vector<int> duplicates(const Library &library, const std::vector<int> &ids, bool exact);

std::string formatTime(int ms);   // "3:07", "1:02:03"
// A track's Location is a URL of the network (a stream), not a file
inline bool isStream(const Track &t) { return t.path.find("://") != std::string::npos; }
// A song's own artwork, the first picture in its tags (ID3's APIC, MP4's covr, FLAC's and Vorbis'
// METADATA_BLOCK_PICTURE): its encoded bytes, empty when it has none
std::vector<unsigned char> artworkData(const std::string &path);

} // namespace iTunes
