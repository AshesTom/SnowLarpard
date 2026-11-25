// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's iTunes Music folder (docs/itunes-reverse.md section 31): where it is (the Advanced pane,
// DITL 260: Change…, Reset), the names iTunes gives the folders and files in it (FUN_000f86e0's folders,
// FUN_000fbf3e's "1-02 Name", FUN_00254300's lengths, FUN_00254132's characters, FUN_0025400e's reserved
// names, FUN_0025647c's numbers), and what is done with them: Keep iTunes Music folder organized
// (FUN_001832e2), Copy files to iTunes Music folder when adding (the copy thread, FUN_001a2a56),
// File ▸ Library ▸ Consolidate Library (FUN_001a086c).
#pragma once

#include <string>
#include <vector>

namespace iTunes {

struct Track;

namespace MusicFolder {

std::string defaultPath();               // ~/Music/iTunes/iTunes Music (the Music folder: XDG_MUSIC_DIR)
std::string path();                      // the one chosen with Change… (Settings "MusicFolder"), else the default
void setPath(const std::string &folder); // empty: Reset (back to the default)
bool keepOrganized();                    // DITL 260 item 6 (+0xc07)
bool copyWhenAdding();                   // DITL 260 item 8
bool contains(const std::string &file, const std::string &root = path());   // FUN_0025607c: the file is in it

// FUN_00254300 with FUN_00254132 and FUN_0025400e: a file's or a folder's name as iTunes makes it. The name
// is shortened so that it, " N" (the number, when not 0) and ".extension" fit in `limit` UTF-16 units (253
// for a file; no limit for a folder) and its trailing spaces are taken off; then : / \ < > " | * ? the
// control characters, ´ (U+00B4), Ø (U+00D8), U+2018 to U+201F, and a "." or " " first or last become
// "_"; a name before its extension that is CON, PRN, AUX, NUL, LPT1–9 or COM1–9 (case ignored) has its
// last character made "_". Empty when it cannot fit.
std::string safeName(const std::string &name, const std::string &extension = {}, int number = 0, size_t limit = 253);

// FUN_000f86e0: the folders a track's file goes in, under the music folder. The first: Podcasts
// (STR# 128.005) for a podcast, Movies, TV Shows, Ringtones, Voice Memos by the media kind,
// Compilations (128.006) for a compilation, else the album artist, else the artist, else Unknown Artist
// (128.002). The second, for music and podcasts: the album, else Unknown Album (128.001; Unknown Podcast
// 128.004); for a TV show its show; none for movies and ringtones
std::vector<std::string> folders(const Track &t);
// FUN_000fbf3e: the file's name before its extension: "02 Name", "1-02 Name" with a disc number when there
// are several discs (disc count not 1); no number without a track number or when
// create-filenames-with-tracknumber is off (Settings "FileNamesWithTrackNumber", on)
std::string fileBase(const Track &t);
// The place the track's file goes: <root>/<folders>/<fileBase>.<extension>, or "<fileBase> 1", " 2"…
// when that name is taken (FUN_0025647c); `current`, the file itself, counts as free. Empty when no name fits.
// The reusable call for anything that puts files in the music folder (adding, organizing, importing a CD).
std::string destination(const Track &t, const std::string &extension, const std::string &current = {},
                        const std::string &root = path());

// The file moved (rename, or copy and delete across disks) or copied to `to`, its folders made; the old
// folders left empty taken away up to the music folder (FUN_000f92ca's loop) after a move
bool moveFile(const std::string &from, const std::string &to, const std::string &root = path());
bool copyFile(const std::string &from, const std::string &to);
// Free bytes on the disk holding `dir` (Consolidate's check, FUN_00254f4e)
long long freeSpace(const std::string &dir);

} // namespace MusicFolder
} // namespace iTunes
