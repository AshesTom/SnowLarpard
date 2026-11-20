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

} // namespace iTunes
