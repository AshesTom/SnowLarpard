// SPDX-License-Identifier: LGPL-2.1-or-later
#include "showall.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace {

// 10.6's Show All, by eye: eight columns 81 points wide from 10 in, the group's name in the band's
// top left, the icons (32 points) 24 down, their names (two lines at most) 3 under them
constexpr double Margin = 10, Column = 81, IconTop = 24, IconSize = 32, LabelGap = 3, LabelHeight = 30, RowHeight = 98;
constexpr int Columns = 8;
const Font LabelFont = Font::system(12);

} // namespace

ShowAll::ShowAll()
{
    double y = 0;
    for (const PaneGroup &g : catalog()) {
        const int rows = std::max(1, int((g.panes.size() + Columns - 1) / Columns));
        const Rect band{0, y, Width, rows * RowHeight};
        m_bands.push_back({g.title, band});
        for (size_t i = 0; i < g.panes.size(); ++i) {
            const double x = Margin + double(i % Columns) * Column, top = y + double(i / Columns) * RowHeight;
            m_cells.push_back({&g.panes[i], Rect{x, top + IconTop - 2, Column, IconSize + LabelGap + LabelHeight + 4}});
        }
        y += band.h;
    }
}

double ShowAll::height() const { return m_bands.empty() ? 200 : m_bands.back().second.bottom(); }

void ShowAll::setSearch(const std::string &text)
{
    m_searching = !text.empty();
    m_matches = searchPanes(text);
    setNeedsDisplay();
}

int ShowAll::cellAt(Point p) const
{
    for (size_t i = 0; i < m_cells.size(); ++i)
        if (m_cells[i].rect.contains(p))
            return int(i);
    return -1;
}

void ShowAll::draw(cairo_t *cr)
{
    // the bands: one grey, the next lighter, a line between them
    for (size_t b = 0; b < m_bands.size(); ++b) {
        const Rect &r = m_bands[b].second;
        fill(cr, r, Color::gray(b % 2 ? 237 : 230));
        if (b > 0)
            fill(cr, Rect{0, r.y, r.w, 1}, Color::gray(202));
        TextStyle t;
        t.font = Font::boldSystem(13);
        t.color = Color::gray(75);
        t.engraved = true;
        t.engravedAlpha = 0.6;
        drawText(cr, m_bands[b].first, Rect{14, r.y + 4, 300, 18}, t);
    }
    for (size_t i = 0; i < m_cells.size(); ++i) {
        const Cell &c = m_cells[i];
        const Rect icon{c.rect.x + std::floor((Column - IconSize) / 2), c.rect.y + 2, IconSize, IconSize};
        c.pane->icon.draw(cr, icon);
        if (int(i) == m_pressed) {
            // pressed: the icon darkened (its own shape as the mask)
            cairo_save(cr);
            cairo_translate(cr, icon.x, icon.y);
            cairo_scale(cr, icon.w / c.pane->icon.width(), icon.h / c.pane->icon.height());
            cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
            cairo_mask_surface(cr, c.pane->icon.surface(), 0, 0);
            cairo_restore(cr);
        }
        TextStyle t;
        t.font = LabelFont;
        t.align = Align::Center;
        drawWrappedText(cr, c.pane->label, Rect{c.rect.x + 1, icon.bottom() + LabelGap, Column - 2, LabelHeight}, t);
    }
    if (!m_searching)
        return;
    // Spotlight: everything darker but the matches, each under its light (System Preferences' spotlight.tif)
    // spotlight.tif is the shade around the light (black, transparent in the middle): the light is
    // where it is transparent
    static const Image light = [] {
        Image tif = Image::load(Assets::path("/Applications/System Preferences.app/Contents/Resources/spotlight.tif"));
        if (tif.isNull())
            return tif;
        Image mask(tif.width(), tif.height());
        cairo_surface_flush(tif.surface());
        cairo_surface_flush(mask.surface());
        const unsigned char *src = cairo_image_surface_get_data(tif.surface());
        unsigned char *dst = cairo_image_surface_get_data(mask.surface());
        const int ss = cairo_image_surface_get_stride(tif.surface()), ds = cairo_image_surface_get_stride(mask.surface());
        for (int y = 0; y < tif.height(); ++y)
            for (int x = 0; x < tif.width(); ++x) {
                const uint32_t v = reinterpret_cast<const uint32_t *>(src + y * ss)[x];
                const uint32_t lum = 255 - (v >> 24);
                reinterpret_cast<uint32_t *>(dst + y * ds)[x] = lum << 24 | lum << 16 | lum << 8 | lum;
            }
        cairo_surface_mark_dirty(mask.surface());
        return mask;
    }();
    cairo_push_group(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
    for (const Cell &c : m_cells)
        if (std::find(m_matches.begin(), m_matches.end(), c.pane->id) != m_matches.end()) {
            const double cx = c.rect.x + Column / 2, cy = c.rect.y + 2 + IconSize / 2 + 8;
            if (!light.isNull())
                light.draw(cr, Rect{cx - light.width() / 2.0, cy - light.height() / 2.0, double(light.width()), double(light.height())});
            else {
                cairo_arc(cr, cx, cy, 45, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        }
    cairo_pop_group_to_source(cr);
    cairo_paint(cr);
}

void ShowAll::mouseDown(Event &e)
{
    m_down = m_pressed = cellAt(e.location);
    setNeedsDisplay();
}

void ShowAll::mouseDragged(Event &e)
{
    const int now = cellAt(e.location) == m_down ? m_down : -1;
    if (now != m_pressed) {
        m_pressed = now;
        setNeedsDisplay();
    }
}

void ShowAll::mouseUp(Event &)
{
    const int chosen = m_pressed;
    m_pressed = m_down = -1;
    setNeedsDisplay();
    if (chosen >= 0 && onOpen)
        onOpen(m_cells[size_t(chosen)].pane->id);
}
