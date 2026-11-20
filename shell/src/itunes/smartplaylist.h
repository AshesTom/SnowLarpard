// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's smart playlists (File ▸ New Smart Playlist…, ⌥⌘N; Edit Smart Playlist): the window
// SmartPlaylist.nib lays out, its rules' fields and operators (MENU 301–309), the rules applied to the
// library (docs/itunes-reverse.md section 11).
#pragma once

#include "appkit.h"
#include "library.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iTunes {

// A field the rules can test: its number, its title in the nib's popup, its kind
struct SmartField {
    enum Kind { Text, Number, Rating, Size, BitRate, SampleRate, Time, Date };
    int id;
    const char *title;
    Kind kind;
};
const std::vector<SmartField> &smartFields();
const SmartField *smartField(int id);
// a kind's operators: the codes, in its MENU's order (301 text, 302 numeric, 308 date)
const std::vector<uint32_t> &smartOperators(SmartField::Kind kind);
std::vector<std::string> smartOperatorTitles(SmartField::Kind kind);

class SmartPlaylistWindow {
public:
    // done(info) on OK
    SmartPlaylistWindow(const SmartInfo &info, std::function<void(const SmartInfo &)> done);
    ~SmartPlaylistWindow();
    void show();

private:
    struct RuleRow;
    void build();
    void layoutRows();
    void addRow(int after);
    void removeRow(int index);
    SmartInfo collect() const;

    SmartInfo m_info;
    std::function<void(const SmartInfo &)> m_done;
    std::unique_ptr<AK::Window> m_window;
    std::vector<std::unique_ptr<RuleRow>> m_rows;
    struct Controls;
    std::unique_ptr<Controls> m_c;
};

} // namespace iTunes
