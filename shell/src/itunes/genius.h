// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's Genius (docs/itunes-reverse.md section 30): Store ▸ Turn On Genius (`TGen`,
// FUN_0057a542), the Genius bar over a Genius playlist (Placards.nib's "GeniusBar") and the Genius
// sidebar (View ▸ Show Genius Sidebar, `shgb`; the bottom bar's `Bb25`). Genius was Apple's: turning it
// on sent the library to Apple, which sent back the songs' similarities (Genius.itdb's
// genius_similarities). The URLs are the Store bag's (http://ax.init.itunes.apple.com/bag.xml?ix=2):
// genius-welcome, library-update-*, sidebar-*; Apple answers genius-welcome now with a Goto to its
// "showUpgradePage", and the bag has no sidebar-* keys any more.
#pragma once

#include "appkit.h"
#include "runloop.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iTunes {

// Placards.nib's "GeniusBar": a user pane (GBBt 1000) 24 high across the list's top (FUN_000249e4: the
// list 24 + 1 lower, the short at 0xaafb24), on picture 1200 (8 × 24, stretched): "Based On:" (5, TXTS
// 1202, 9, 4), the song (6, TXTS 1203), "by" (7, 1204), its artist (8, 1203); bound to the right "Limit
// to:" (4, TXTS 1201, right justified, 453, 4, 53 × 13), the limit (3, `LGPl`, PBUT 1204: 25, 50, 75,
// 100 songs; 508, 4, 85 × 17), Refresh (2, `RGPl`, PBUT 1202, 598, 62 × 17) and Save Playlist (1,
// `SGPl`, 665, 89 × 17), their titles TXTS 1200
class GeniusBar : public AK::View {
public:
    static constexpr double Height = 24;
    std::function<void(const std::string &)> onCommand;
    std::function<void(int)> onLimit;   // 25, 50, 75 or 100
    void set(const std::string &song, const std::string &artist, int limit, bool canSave);
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;

private:
    AK::Rect part(int which) const;   // 1 Save Playlist, 2 Refresh, 3 the limit
    std::string m_song, m_artist;
    int m_limit = 25, m_pressed = 0;
    bool m_canSave = true;
};

// The Genius sidebar's place (FUN_000249e4): right of the songs, from the top band to the bottom one,
// its width the preference +0xF54E (at least the page's own minimum +0xF548, at most 350) and a point
// for the line left of it; the songs narrowed first, then the source list. Its contents are the Store's
// pages (the bag's sidebar-welcome with Genius off, sidebar-search…; FUN_000328fa): none comes now, so
// FUN_00044000 leaves it empty. Dragging its line (FUN_0014dbb4) changes the width; under half the
// minimum it hides
class GeniusSidebar : public AK::View {
public:
    static constexpr double MaxWidth = 350;
    std::function<void(double)> onResize;   // the width dragged to (the line's x, in the window)
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    const char *cursor() const override { return "col-resize"; }

private:
    double m_grab = 0;
};

// Store ▸ Turn On Genius: the bag's genius-welcome asked; done(true) never (Apple's answer now is the
// upgrade page), done(false, message, informative) with what iTunes says
class GeniusService {
public:
    void turnOn(std::function<void(const std::string &message, const std::string &informative)> failed);
    bool busy() const { return m_task != nullptr; }

private:
    std::unique_ptr<AK::Task> m_task;
};

} // namespace iTunes
