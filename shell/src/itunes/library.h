// SPDX-License-Identifier: LGPL-2.1-or-later
// The iTunes library: its tracks and playlists, kept where iTunes keeps them,
// ~/Music/iTunes/iTunes Music Library.xml, in the format iTunes writes it (a plist: Major Version 1,
// Minor Version 1, Application Version, Music Folder, Tracks by Track ID, Playlists with their Playlist
// Items). The files' tags are TagLib's. At the first launch, the music folder (XDG_MUSIC_DIR) is added.
#pragma once

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
};

struct Playlist {
    std::string name;
    std::vector<int> items;   // Track IDs
    bool master = false;      // "Library"
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
    Track *track(int id);
    std::vector<Playlist> &playlists() { return m_playlists; }
    void remove(int id);

private:
    void addFile(const std::string &path);
    std::map<int, Track> m_tracks;
    std::vector<Playlist> m_playlists;
    int m_nextId = 100;
};

std::string formatTime(int ms);   // "3:07", "1:02:03"

} // namespace iTunes
