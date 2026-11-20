// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's Cover Flow (View ▸ as Cover Flow, ⌥⌘5: AlbumCoverDisplay, docs/itunes-reverse.md
// section 12): the albums' covers in a row in perspective over the songs, the chosen one facing.
#pragma once

#include "appkit.h"
#include "graphics.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace iTunes {

class Library;

class CoverFlowView : public AK::View {
public:
    explicit CoverFlowView(Library &library);
    ~CoverFlowView() override;
    // the songs shown, in the list's order: an album per run of songs of the same album
    void setSongs(const std::vector<int> &ids);
    // the album of a song brought to the middle (the list's selection)
    void showSong(int id);
    std::function<void(int id)> onChoose;                  // an album chosen: its first song
    std::function<void(const std::vector<int> &)> onPlay;  // a cover double-clicked: its songs
    std::function<void()> onFullScreen;                    // the HUD's full screen button (`full`)
    bool fullScreen = false;                               // AlbumCoverDisplay's mode flag 1
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    void scrollWheel(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }

private:
    struct Album {
        std::string title, artist, artPath;
        std::vector<int> ids;
    };
    struct Placed {
        int index;
        double x, z, angle, alpha;
    };
    void select(int index, bool notify);
    AK::Rect scrollerRect() const;
    AK::Rect thumbRect() const;
    AK::Rect fullRect() const;
    void drawHud(cairo_t *cr) const;
    void scrollTo(double x);
    void tick();
    std::vector<Placed> placements() const;
    void drawCover(cairo_t *cr, const Album &a, const Placed &p, double size, double baseY) const;
    AK::Image cover(const std::string &path) const;

    Library &m_library;
    std::vector<Album> m_albums;
    int m_selected = 0;
    double m_position = 0;   // the animated index in the middle
    int m_timer = 0;
    bool m_dragThumb = false, m_pressFull = false;
    double m_grab = 0;
    mutable std::map<std::string, AK::Image> m_covers;
};

} // namespace iTunes
