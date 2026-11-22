// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's Info Window (File ▸ Get Info, ⌘I: DLOG / DITL 135, docs/itunes-reverse.md section 9):
// its tabs (tab# 1358: Summary, Info, Video, Sorting, Options, Lyrics, Artwork), the Info pane as DITL
// 271 lays it out, OK, Cancel, Previous and Next.
#pragma once

#include "appkit.h"

#include <functional>
#include <memory>
#include <vector>

namespace iTunes {

class Library;

class InfoWindow {
public:
    explicit InfoWindow(Library &library);
    ~InfoWindow();
    // the song ids[index], Previous and Next going through ids
    void show(const std::vector<int> &ids, int index);
    std::function<void(int id)> onChanged;   // a song's information saved

private:
    struct Pane;
    void build();
    void load();
    bool save();
    void step(int by);

    Library &m_library;
    std::unique_ptr<AK::Window> m_window;
    std::unique_ptr<Pane> m_pane;
    std::vector<int> m_ids;
    int m_index = 0;
};

// Get Info with several songs (FUN_000e9042): first STR# 9008.001 "Are you sure you want to edit
// information for multiple items?" (Yes, Cancel); then Multiple Item Information (DLOG / DITL 136, 598 ×
// 471: OK, Cancel, the tabs tab# 1390 Info, Video, Sorting, Options at (20, 15), 558 × 400; their panes
// DITL 280–283). A field shows the songs' value when they all have it (FUN_000fac26 clears a field's
// bit at the first song that differs); a box beside each field says it is written to every song
// (FUN_0018e2ac with the boxes' mask); a track or disc number boxed but left empty keeps each song's own.
class MultipleInfoWindow {
public:
    explicit MultipleInfoWindow(Library &library);
    ~MultipleInfoWindow();
    void show(const std::vector<int> &ids);
    std::function<void()> onChanged;   // the songs' information saved

private:
    struct Pane;
    void build();
    void save();

    Library &m_library;
    std::unique_ptr<AK::Window> m_window;
    std::unique_ptr<Pane> m_pane;
    std::vector<int> m_ids;
};

} // namespace iTunes
