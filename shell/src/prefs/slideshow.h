// SPDX-License-Identifier: LGPL-2.1-or-later
// The screen savers: slideshows of a folder of pictures, cross-fading (10.6's picture savers do it with
// their own pictures; here the Desktop Pictures collections and your folders). Shown in the pane's
// preview, and full screen by Test and by the Finder when the computer has been idle long enough.
#pragma once

#include "appkit.h"

#include <string>
#include <vector>

class Slideshow : public AK::View {
public:
    explicit Slideshow(bool fullScreen = false) : m_fullScreen(fullScreen) {}
    ~Slideshow() override;
    void setFolder(const std::string &folder);
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &) override;
    void mouseMoved(AK::Event &e) override;
    void keyDown(AK::Event &) override;
    bool acceptsFirstResponder() const override { return m_fullScreen; }

private:
    void next();
    void stop();
    std::vector<std::string> m_pictures;
    int m_index = -1;
    AK::Image m_current, m_previous;
    double m_fade = 1;
    int m_hold = 0, m_step = 0;
    bool m_fullScreen;
    AK::Point m_firstMouse{-1, -1};
};
