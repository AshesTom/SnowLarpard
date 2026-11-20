// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's own window frames (GradientFrameView, WindowTitleBarView, WindowFrameButtonView:
// docs/itunes-reverse.md, section 4), shared by its windows: the browser window and the Equalizer.
#pragma once

#include "appkit.h"
#include "text.h"

#include <functional>
#include <string>

namespace iTunes {

// GradientFrameView's gradient (FUN_00028bba, FUN_00028dc6; the greys at 0xa5fbc0): rows [from, …) of
// a gradient `rows` tall (90 at most, then its end grey) into r
double gradientValue(double start, double end, int row, int rows);
void drawGradient(cairo_t *cr, const AK::Rect &r, double start, double end, int rows, int from);

// A text with its shadow a point under it, as FUN_002985e2 draws a TXTS's
void drawShadowed(cairo_t *cr, const std::string &text, const AK::Rect &r, AK::TextStyle t, const AK::Color &shadow);

// The close, minimize and zoom buttons (WindowFrameButtonView, WFBt 1 to 3): the document window's
// 13004–13006 21 apart, the utility window's 13014–13016 18 apart. Their columns inactive, normal,
// rollover, pressed (FUN_0002b9f0's kind 4); all three in rollover while the pointer is over any of
// them (one tracking area, FUN_0001b9c8)
class FrameButtons : public AK::View {
public:
    explicit FrameButtons(bool utility = false) : m_base(utility ? 13014 : 13004), m_step(utility ? 18 : 21) {}
    std::function<void(int)> onClick;   // 1 close, 2 minimize, 3 zoom
    void draw(cairo_t *cr) override;
    void mouseMoved(AK::Event &e) override;
    void mouseExited(AK::Event &) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;

private:
    AK::Size buttonSize() const;
    int buttonAt(AK::Point p) const;
    bool overUnion(AK::Point p) const;
    void setOver(bool over);
    int m_base;
    double m_step;
    int m_pressed = -1;
    bool m_inside = false, m_over = false;
};

// A part of the frame a drag moves the window by (GradientFrameView's own clicks); a double click in
// its title bar's `m_titleBar` points calls onDoubleClickTitle
class FrameBand : public AK::View {
public:
    std::function<void()> onDoubleClickTitle;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;

protected:
    double m_titleBar = 0;

private:
    AK::Point m_from;
};

} // namespace iTunes
