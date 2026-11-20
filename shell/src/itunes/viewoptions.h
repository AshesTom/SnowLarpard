// SPDX-License-Identifier: LGPL-2.1-or-later
// The song list's columns and iTunes 8.2.1's View Options (View ▸ View Options…, ⌘J: DLOG / DITL 144,
// docs/itunes-reverse.md section 13). A column is one of iTunes' fields (the numbers CWid 9000 and
// the smart playlists' rules share: 2 Name, 3 Album, 4 Artist, 13 Time, 25 My Rating…), its title
// STR# 9006's, its default width and justification CWid 9000's.
#pragma once

#include "appkit.h"
#include "library.h"
#include "text.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iTunes {

struct ListField {
    int field;
    const char *titleKey;   // STR# 9006
    double width;           // CWid 9000's default
    AK::Align align;        // CWid 9000's justification (−1 right)
    bool numeric;           // sorted by number
};
const std::vector<ListField> &listFields();   // the ones the library keeps
const ListField *listField(int field);
std::vector<int> defaultColumns();             // Name, Time, Artist, Album, Genre, Play Count, My Rating

std::string cellText(const Track &t, int field);
long long cellNumber(const Track &t, int field);
std::string cellKey(const Track &t, int field);   // the text sort's key (Artist: then album, track)

class ViewOptionsWindow {
public:
    // the source's name and icon row (441) over the boxes; done(columns) on OK
    ViewOptionsWindow(const std::string &source, int icon, const std::vector<int> &columns,
                      std::function<void(const std::vector<int> &)> done);
    ~ViewOptionsWindow();
    void show();

private:
    std::unique_ptr<AK::Window> m_window;
    std::vector<int> m_columns;
    std::function<void(const std::vector<int> &)> m_done;
    std::string m_source;
    int m_icon;
};

} // namespace iTunes
