// SPDX-License-Identifier: LGPL-2.1-or-later
// The grid as iTunes 8.2.1 has it:
//   the browse bar (Placards.nib's "BrowseBar": a user pane 29 high; BrowseBarView, its commands
//   FUN_00584afa): picture 564 (20 × 29) stretched across; the grouping's buttons (BBBP, TXTS 720:
//   Txtr 720, Lucida Grande 11, white, centred) 83 × 21 each, 4 down, side by side in the middle, the
//   first on 552, the middle ones on 554, the last on 553 (two columns, kind 2: the second pressed; two
//   rows: the second the chosen one; 3 points kept at the ends); their titles STR# 250.001–250.004
//   (Albums, Artists, Genres, Composers). At the right the size: the small picture 560 (9 × 10, BBZS:
//   the size to 0), the slider (13, 7, 123 × 16: track 556, 4 points kept at its ends; thumb 558,
//   three cells of kind 3; its value 0 to 100, at first 50), the large picture 562 (13 × 14, BBZL: to
//   1); the group 153 wide, 20 from the right.
//   The size: the slider's value / 100 (FUN_0005b120), the grid's scale 0.5 + 1.5 × it
//   (FUN_00150974 on the 'Brws' 'PSCh' event, kept per grouping by FUN_000ee868).
//   The covers' frames: PBUT 578 (126 × 126, 12 kept at each side: the cover's shadow) round a cover,
//   570 (22 kept: the blue ring) round the chosen one.
//   Not read: the covers' base size (100 points at scale 1 here), their spacing, the titles' style
//   and the background (by eye: the bar's dark grey continued).
#include "grid.h"

#include "controls.h"
#include "foundation.h"
#include "frame.h"
#include "library.h"
#include "resources.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace AK;

namespace iTunes {

namespace {

constexpr double BarHeight = 29, ButtonW = 83, ButtonH = 21;
const Color Background = Color::gray(66);

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// ---- the browse bar

class GridView::Bar : public View {
public:
    explicit Bar(GridView *g) : m_grid(g) {}
    void draw(cairo_t *cr) override
    {
        drawCell(cr, 564, 0, 0, bounds());
        // the grouping's buttons
        const Rect group = buttons();
        AK::TextStyle t;
        const TextStyle s = textStyle(720);
        t.font = s.font;
        t.color = Color::gray(255);
        t.align = Align::Center;
        for (int i = 0; i < 4; ++i) {
            const int pic = i == 0 ? 552 : i == 3 ? 553 : 554;
            const Rect r{group.x + i * ButtonW, group.y, ButtonW, ButtonH};
            drawCell(cr, pic, m_pressed == i ? 1 : 0, int(m_grid->m_grouping) == i ? 1 : 0, r);
            static const char *const keys[4] = {"250.001", "250.002", "250.003", "250.004"};
            drawText(cr, tr(keys[i]), r.translated(0, -1), t);
        }
        // the size
        const Rect sz = sizeGroup();
        drawCell(cr, 560, stateColumn(3, 1), 0, Rect{sz.x, sz.y + 10, 9, 10});
        drawCell(cr, 562, stateColumn(3, 1), 0, Rect{sz.x + 140, sz.y + 8, 13, 14});
        const Rect track = sliderRect();
        drawCell(cr, 556, 0, 0, Rect{track.x, track.y + std::floor((track.h - 7) / 2), track.w, 7});
        const Size thumb = cellSize(558);
        const double x = track.x + std::round(m_grid->m_zoom * (track.w - thumb.w));
        drawCell(cr, 558, stateColumn(3, m_dragging ? 2 : 1), 0, Rect{x, track.y + std::floor((track.h - thumb.h) / 2), thumb.w, thumb.h});
    }
    void mouseDown(Event &e) override
    {
        const Rect group = buttons();
        if (group.contains(e.location)) {
            m_pressed = int((e.location.x - group.x) / ButtonW);
            setNeedsDisplay();
            return;
        }
        const Rect sz = sizeGroup();
        if (Rect{sz.x, sz.y, 12, 30}.contains(e.location)) {   // BBZS
            m_grid->setZoom(0);
            return;
        }
        if (Rect{sz.x + 138, sz.y, 15, 30}.contains(e.location)) {   // BBZL
            m_grid->setZoom(1);
            return;
        }
        if (sliderRect().inset(-2, -4).contains(e.location)) {
            m_dragging = true;
            track(e.location.x);
        }
    }
    void mouseDragged(Event &e) override
    {
        if (m_dragging)
            track(e.location.x);
    }
    void mouseUp(Event &e) override
    {
        if (m_pressed >= 0 && buttons().contains(e.location) && int((e.location.x - buttons().x) / ButtonW) == m_pressed)
            m_grid->setGrouping(Grouping(m_pressed));
        m_pressed = -1;
        m_dragging = false;
        setNeedsDisplay();
    }

private:
    Rect buttons() const { return {std::floor((frame().w - 4 * ButtonW) / 2), 4, 4 * ButtonW, ButtonH}; }
    Rect sizeGroup() const { return {frame().w - 20 - 153, 0, 153, 30}; }
    Rect sliderRect() const
    {
        const Rect g = sizeGroup();
        return {g.x + 13, g.y + 7, 123, 16};
    }
    void track(double x)
    {
        const Rect r = sliderRect();
        const double w = cellSize(558).w;
        m_grid->setZoom(std::clamp((x - r.x - w / 2) / std::max(1.0, r.w - w), 0.0, 1.0));
    }
    GridView *m_grid;
    int m_pressed = -1;
    bool m_dragging = false;
};

// ---- the covers

class GridView::Items : public View {
public:
    explicit Items(GridView *g) : m_grid(g) {}
    double side() const { return std::round(100 * (0.5 + 1.5 * m_grid->m_zoom)); }
    double cellW() const { return side() + 40; }
    double cellH() const { return side() + 58; }
    int columns() const { return std::max(1, int((frame().w - 10) / cellW())); }
    double contentHeight() const
    {
        const int n = int(m_grid->m_groups.size()), c = columns();
        return 10 + std::ceil(double(n) / c) * cellH();
    }
    Rect cellRect(int i) const
    {
        const int c = columns();
        // the row's cells spread over the width
        const double gap = (frame().w - c * cellW()) / (c + 1);
        return {gap + (i % c) * (cellW() + gap), 10 + (i / c) * cellH(), cellW(), cellH()};
    }
    Rect artRect(int i) const
    {
        const Rect c = cellRect(i);
        return {c.x + 20, c.y + 12, side(), side()};
    }
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Background);
        AK::TextStyle title, sub;
        title.font = Font::boldSystem(11);
        title.color = Color::gray(255);
        title.align = Align::Center;
        sub.font = Font::system(11);
        sub.color = Color::gray(170);
        sub.align = Align::Center;
        for (size_t i = 0; i < m_grid->m_groups.size(); ++i) {
            const Group &g = m_grid->m_groups[i];
            const Rect a = artRect(int(i));
            const bool sel = int(i) == m_grid->m_selected;
            // the frame: 578 (its 12-point shadow) or 570 (the blue ring, 22 kept)
            if (sel)
                drawCell(cr, 570, 0, 0, Rect{a.x - 12, a.y - 12, a.w + 24, a.h + 24});
            else
                drawCell(cr, 578, 0, 0, Rect{a.x - 12, a.y - 12, a.w + 24, a.h + 24});
            const Image img = m_grid->cover(g.artPath);
            if (!img.isNull()) {
                // the cover filled into its square
                cairo_save(cr);
                cairo_rectangle(cr, a.x, a.y, a.w, a.h);
                cairo_clip(cr);
                const double s = std::max(a.w / img.width(), a.h / img.height());
                cairo_translate(cr, a.x + (a.w - img.width() * s) / 2, a.y + (a.h - img.height() * s) / 2);
                cairo_scale(cr, s, s);
                cairo_set_source_surface(cr, img.surface(), 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            } else {
                fill(cr, a, Color::gray(110));
                const Image note = picture(154);   // the Apple of the LCD, the placeholder here
                if (!note.isNull())
                    note.draw(cr, Point{std::floor(a.x + (a.w - note.width()) / 2), std::floor(a.y + (a.h - note.height()) / 2)});
            }
            const Rect c = cellRect(int(i));
            drawText(cr, g.title, Rect{c.x + 2, a.bottom() + 8, c.w - 4, 15}, title);
            if (!g.subtitle.empty())
                drawText(cr, g.subtitle, Rect{c.x + 2, a.bottom() + 23, c.w - 4, 15}, sub);
        }
    }
    void mouseDown(Event &e) override
    {
        int hit = -1;
        for (size_t i = 0; i < m_grid->m_groups.size(); ++i)
            if (cellRect(int(i)).contains(e.location))
                hit = int(i);
        m_grid->m_selected = hit;
        setNeedsDisplay();
        if (hit >= 0 && e.clickCount == 2 && m_grid->onPlay)
            m_grid->onPlay(m_grid->m_groups[size_t(hit)].ids);
    }

private:
    GridView *m_grid;
};

// ---- GridView

GridView::GridView(Library &library) : m_library(library)
{
    m_bar = add<Bar>(this);
    ScrollView *sv = add<ScrollView>();
    m_scroll = sv;
    m_items = sv->document()->add<Items>(this);
    Settings s("itunes");
    m_grouping = Grouping(std::clamp(s.integer("GridGrouping", 0), 0, 3));
    m_zoom = std::clamp(s.integer("GridSize", 50), 0, 100) / 100.0;
}

void GridView::setFrame(const Rect &r)
{
    View::setFrame(r);
    relayout();
}

void GridView::relayout()
{
    m_bar->setFrame({0, 0, frame().w, BarHeight});
    auto *sv = static_cast<ScrollView *>(m_scroll);
    sv->setFrame({0, BarHeight, frame().w, std::max(0.0, frame().h - BarHeight)});
    m_items->setFrame({0, 0, sv->frame().w - 15, 1});
    sv->lineStep = 20;
    sv->setDocumentHeight(std::max(m_items->contentHeight(), sv->frame().h));
    m_items->setFrame(sv->document()->bounds());
    m_items->setNeedsDisplay();
    m_bar->setNeedsDisplay();
}

void GridView::draw(cairo_t *cr) { fill(cr, bounds(), Background); }

void GridView::setSongs(const std::vector<int> &ids)
{
    m_ids = ids;
    regroup();
}

void GridView::setGrouping(Grouping g)
{
    if (g == m_grouping)
        return;
    m_grouping = g;
    m_selected = -1;
    Settings("itunes").set("GridGrouping", int(g));
    regroup();
    if (onChanged)
        onChanged();
}

void GridView::setZoom(double z)
{
    m_zoom = std::clamp(z, 0.0, 1.0);
    Settings("itunes").set("GridSize", int(std::lround(m_zoom * 100)));
    relayout();
}

void GridView::regroup()
{
    m_groups.clear();
    std::map<std::string, size_t> index;
    for (int id : m_ids) {
        const Track *t = m_library.track(id);
        if (!t)
            continue;
        std::string key, title, subtitle;
        const std::string artist = t->albumArtist.empty() ? t->artist : t->albumArtist;
        switch (m_grouping) {
        case Albums:
            title = t->album;
            subtitle = artist;
            key = lower(title) + "\x01" + lower(subtitle);
            break;
        case Artists: title = t->artist; key = lower(title); break;
        case Genres: title = t->genre; key = lower(title); break;
        case Composers: title = t->composer; key = lower(title); break;
        }
        if (title.empty())
            continue;
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, m_groups.size()).first;
            m_groups.push_back({title, subtitle, {}, {}});
        }
        Group &g = m_groups[it->second];
        g.ids.push_back(id);
        if (g.artPath.empty() && !artworkData(t->path).empty())
            g.artPath = t->path;
    }
    std::sort(m_groups.begin(), m_groups.end(), [](const Group &a, const Group &b) {
        return lower(a.title + "\x01" + a.subtitle) < lower(b.title + "\x01" + b.subtitle);
    });
    if (m_selected >= int(m_groups.size()))
        m_selected = -1;
    relayout();
}

Image GridView::cover(const std::string &path)
{
    if (path.empty())
        return {};
    auto it = m_covers.find(path);
    if (it != m_covers.end())
        return it->second;
    const Image img = Image::fromData(artworkData(path), {300, 300});
    m_covers[path] = img;
    return img;
}

} // namespace iTunes
