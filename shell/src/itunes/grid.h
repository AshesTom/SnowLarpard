// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's grid (View ▸ as Grid, ⌥⌘4: TrackGroupDisplay under BrowseBarView, docs/itunes-reverse.md
// section 10): the songs grouped by album, artist, genre or composer, a cover each.
#pragma once

#include "appkit.h"
#include "graphics.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace iTunes {

class Library;

class GridView : public AK::View {
public:
    enum Grouping { Albums, Artists, Genres, Composers };
    explicit GridView(Library &library);
    // the songs shown (the source's, filtered by the search field)
    void setSongs(const std::vector<int> &ids);
    void setGrouping(Grouping g);
    Grouping grouping() const { return m_grouping; }
    // the size slider's value, 0 to 1 (FUN_0005b120: the slider's value / 100)
    void setZoom(double z);
    double zoom() const { return m_zoom; }
    std::function<void(const std::vector<int> &)> onPlay;   // a cover double-clicked: its songs
    std::function<void()> onChanged;                        // the grouping or the size changed
    void setFrame(const AK::Rect &r) override;
    void draw(cairo_t *cr) override;

private:
    struct Group {
        std::string title, subtitle;
        std::vector<int> ids;
        std::string artPath;   // the first song with a picture
    };
    class Bar;
    class Items;
    void regroup();
    void relayout();
    AK::Image cover(const std::string &path);

    Library &m_library;
    Bar *m_bar = nullptr;
    AK::View *m_scroll = nullptr;
    Items *m_items = nullptr;
    std::vector<int> m_ids;
    std::vector<Group> m_groups;
    Grouping m_grouping = Albums;
    double m_zoom = 0.5;
    int m_selected = -1;
    std::map<std::string, AK::Image> m_covers;
};

} // namespace iTunes
