// SPDX-License-Identifier: LGPL-2.1-or-later
// A movie's picture (docs/itunes-reverse.md section 29): what plays, drawn by libmpv's software renderer
// at the view's size (its aspect kept, black around it), in the iTunes window, a window of its own or
// over the whole screen, as the Playback pane's "Play Movies and TV Shows" and "Play Music Videos" say.
#pragma once

#include "appkit.h"

#include <functional>

namespace iTunes {

class Player;

class VideoView : public AK::View {
public:
    explicit VideoView(Player &player) : m_player(player) {}
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }
    std::function<void()> onDoubleClick;   // full screen and back (by eye)
    std::function<void()> onEscape;        // the movie stopped

private:
    Player &m_player;
};

} // namespace iTunes
