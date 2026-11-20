// SPDX-License-Identifier: LGPL-2.1-or-later
#include "frame.h"

#include "resources.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace iTunes {

double gradientValue(double start, double end, int row, int rows)
{
    const double t = rows > 1 ? double(row) / (rows - 1) : 0, d = start - end;
    return std::round(t * t * t * d - t * t * d * 1.5 - d * 0.5 * t + start);
}

void drawGradient(cairo_t *cr, const Rect &r, double start, double end, int rows, int from)
{
    const int span = std::min(rows, 90);
    for (int y = 0; y < int(r.h); ++y) {
        const int row = from + y;
        const double v = row < span ? gradientValue(start, end, row, span) : end;
        fill(cr, Rect{r.x, r.y + y, r.w, 1}, Color::gray(int(v)));
    }
}

void drawShadowed(cairo_t *cr, const std::string &text, const Rect &r, AK::TextStyle t, const Color &shadow)
{
    const Color c = t.color;
    t.color = shadow;
    drawText(cr, text, r.translated(0, 1), t);
    t.color = c;
    drawText(cr, text, r, t);
}

// ---- FrameButtons

Size FrameButtons::buttonSize() const { return cellSize(m_base); }

void FrameButtons::draw(cairo_t *cr)
{
    const bool key = windowIsKey();
    const Size cell = buttonSize();
    for (int i = 0; i < 3; ++i) {
        // FUN_0002b9f0's kind 4: 0 inactive, 1 normal, 3 rollover (column 2), 2 pressed (column 3)
        const int state = m_pressed == i && m_inside ? 2 : m_over ? 3 : key ? 1 : 0;
        drawCell(cr, m_base + i, stateColumn(4, state), 0, Rect{i * m_step, 0, cell.w, cell.h});
    }
}

void FrameButtons::mouseMoved(Event &e) { setOver(buttonAt(e.location) >= 0 || overUnion(e.location)); }
void FrameButtons::mouseExited(Event &) { setOver(false); }

void FrameButtons::mouseDown(Event &e)
{
    m_pressed = buttonAt(e.location);
    m_inside = m_pressed >= 0;
    setNeedsDisplay();
}

void FrameButtons::mouseDragged(Event &e)
{
    const bool in = m_pressed >= 0 && buttonAt(e.location) == m_pressed;
    if (in != m_inside) {
        m_inside = in;
        setNeedsDisplay();
    }
}

void FrameButtons::mouseUp(Event &e)
{
    const int b = m_pressed;
    const bool fire = b >= 0 && buttonAt(e.location) == b;
    m_pressed = -1;
    m_inside = false;
    m_over = overUnion(e.location);
    setNeedsDisplay();
    if (fire && onClick)
        onClick(b + 1);
}

int FrameButtons::buttonAt(Point p) const
{
    const Size cell = buttonSize();
    for (int i = 0; i < 3; ++i)
        if (Rect{i * m_step, 0, cell.w, cell.h}.contains(p))
            return i;
    return -1;
}

bool FrameButtons::overUnion(Point p) const
{
    const Size cell = buttonSize();
    return Rect{0, 0, 2 * m_step + cell.w, cell.h}.contains(p);
}

void FrameButtons::setOver(bool over)
{
    if (over != m_over) {
        m_over = over;
        setNeedsDisplay();
    }
}

// ---- FrameBand

void FrameBand::mouseDown(Event &e)
{
    if (e.clickCount == 2 && e.location.y < m_titleBar) {
        if (onDoubleClickTitle)
            onDoubleClickTitle();
        return;
    }
    if (Window *w = window())
        w->dragMove(e.screen, e.screen, true);
    m_from = e.screen;
}

void FrameBand::mouseDragged(Event &e)
{
    if (Window *w = window())
        w->dragMove(m_from, e.screen, false);
}

} // namespace iTunes
