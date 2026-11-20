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

    const std::map<int, Track> &tracks() const { return m_tracks; }
    const Track *track(int id) const;
    int idForPath(const std::string &path) const;   // 0: not in the library
    Track *track(int id);
    std::vector<Playlist> &playlists() { return m_playlists; }
    void remove(int id);
    // Get Info's changes into the song's file (TagLib) as well as the library: false when the file
    // could not be written
    bool writeTags(const Track &t);
    // A smart playlist's songs: the rules applied to the library, then the limit
    std::vector<int> evaluate(const SmartInfo &info) const;

private:
    void addFile(const std::string &path);
    std::map<int, Track> m_tracks;
    std::vector<Playlist> m_playlists;
    int m_nextId = 100;
};

std::string formatTime(int ms);   // "3:07", "1:02:03"
// A song's own artwork, the first picture in its tags (ID3's APIC, MP4's covr, FLAC's and Vorbis'
// METADATA_BLOCK_PICTURE): its encoded bytes, empty when it has none
std::vector<unsigned char> artworkData(const std::string &path);

} // namespace iTunes
