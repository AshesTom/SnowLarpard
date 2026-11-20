// SPDX-License-Identifier: LGPL-2.1-or-later
// Cover Flow as AlbumCoverDisplay draws it (FUN_004065aa) on its 3D canvas (FUN_003f0f08: a 50° vertical
// field of view, near 1, far 20; FUN_003f1262 a picture at x, y in the view's points, a depth, turned
// about its own vertical middle). Its layout (FUN_00403798, read by FUN_00403c40 and FUN_00405e7c):
//   a cover is a square 2/3 of the view's height (19/30 in its small mode); 8 covers on each side;
//   the middle one facing at the view's middle; the first on the right starts a quarter of a cover
//   right of the middle one's middle (+0x1C: 1/4 of the cover), each next 1/7 of a cover further
//   (+0x18: 6/7 of a cover overlap), the left side the same mirrored; the side covers turned 60°
//   (+0x24, +0x28) and set back at depth 60 (+0x2C), the middle one at 30 (+0x30); past the eighth
//   the covers go on a quarter of the overlap apart (+0x20).
//   Each cover's reflection under it: mirrored, its opacity 0.4 at the cover's foot down to 0.05
//   (FUN_004065aa; 0.5 or 0.4 to 0.001 in its other mode).
// Not GL here: cairo draws each cover column by column, each column's height by its depth's
// perspective (the 50° field of view); the depth units (60, 30) taken as fractions of a cover (by
// eye: the canvas's +0x24 scale not read). The background black and the titles under the covers
// (white, then grey) by eye.
#include "coverflow.h"

#include "library.h"
#include "resources.h"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace AK;

namespace iTunes {

namespace {

constexpr int SideCount = 8;
constexpr double SideAngle = 60;       // degrees
constexpr double FieldOfView = 50;     // degrees, vertical

} // namespace

CoverFlowView::CoverFlowView(Library &library) : m_library(library) {}

CoverFlowView::~CoverFlowView()
{
    if (m_timer)
        App::shared().removeTimer(m_timer);
}

void CoverFlowView::setSongs(const std::vector<int> &ids)
{
    const std::string keep = m_selected < int(m_albums.size()) && !m_albums.empty() ? m_albums[size_t(m_selected)].title : std::string();
    m_albums.clear();
    std::map<std::string, size_t> index;
    for (int id : ids) {
        const Track *t = m_library.track(id);
        if (!t)
            continue;
        const std::string artist = t->albumArtist.empty() ? t->artist : t->albumArtist;
        const std::string key = t->album + "\x01" + artist;
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, m_albums.size()).first;
            m_albums.push_back({t->album, artist, {}, {}});
        }
        Album &a = m_albums[it->second];
        a.ids.push_back(id);
        if (a.artPath.empty() && !artworkData(t->path).empty())
            a.artPath = t->path;
    }
    m_selected = 0;
    for (size_t i = 0; i < m_albums.size(); ++i)
        if (m_albums[i].title == keep)
            m_selected = int(i);
    m_position = m_selected;
    setNeedsDisplay();
}

void CoverFlowView::showSong(int id)
{
    for (size_t i = 0; i < m_albums.size(); ++i)
        if (std::find(m_albums[i].ids.begin(), m_albums[i].ids.end(), id) != m_albums[i].ids.end()) {
            select(int(i), false);
            return;
        }
}

void CoverFlowView::select(int index, bool notify)
{
    if (m_albums.empty())
        return;
    index = std::clamp(index, 0, int(m_albums.size()) - 1);
    m_selected = index;
    if (!m_timer)
        m_timer = App::shared().addTimer(1.0 / 60, true, [this] { tick(); });
    if (notify && onChoose)
        onChoose(m_albums[size_t(index)].ids.front());
}

// the covers glide to their places: a sixth of the way each frame
void CoverFlowView::tick()
{
    const double d = m_selected - m_position;
    if (std::fabs(d) < 0.01) {
        m_position = m_selected;
        App::shared().removeTimer(m_timer);
        m_timer = 0;
    } else {
        m_position += d / 6;
    }
    setNeedsDisplay();
}

// FUN_00403c40's zones for a cover at a fractional distance from the middle: x of its middle (in
// covers from the view's middle), its depth (in covers back), its angle
std::vector<CoverFlowView::Placed> CoverFlowView::placements() const
{
    std::vector<Placed> out;
    const int first = std::max(0, int(std::floor(m_position)) - SideCount - 2);
    const int last = std::min(int(m_albums.size()) - 1, int(std::ceil(m_position)) + SideCount + 2);
    auto at = [](double k, double &x, double &z, double &angle) {
        // k: the signed distance from the middle, in covers
        const double s = k < 0 ? -1 : 1, a = std::fabs(k);
        // the depths 60 and 30: their difference, 30, as half a cover behind the middle one (by eye)
        const double sideZ = (60.0 - 30.0) / 60, midZ = 0;
        if (a <= 1) {
            // from facing in the middle to turned at the first side place
            x = s * a * 0.75;
            z = midZ + (sideZ - midZ) * a;
            angle = -s * SideAngle * a;
        } else {
            // the first side cover's rect starts a quarter of a cover right of the middle one's middle
            // (its own middle 3/4 out, FUN_00403c40's zone 3), then 1/7 apart
            const double firstX = 0.75;
            const double n = a - 1;
            const double step = 1.0 / 7, far = step / 4;
            const double extra = n <= SideCount - 1 ? n * step : (SideCount - 1) * step + (n - (SideCount - 1)) * far;
            x = s * (firstX + extra);
            z = sideZ;
            angle = -s * SideAngle;
        }
    };
    for (int i = first; i <= last; ++i) {
        Placed p;
        p.index = i;
        double x, z, angle;
        at(i - m_position, x, z, angle);
        p.x = x;
        p.z = z;
        p.angle = angle;
        p.alpha = std::fabs(i - m_position) > SideCount + 1 ? 0 : 1;
        out.push_back(p);
    }
    // drawn from the far ones to the middle one
    std::sort(out.begin(), out.end(), [this](const Placed &a, const Placed &b) {
        return std::fabs(a.index - m_position) > std::fabs(b.index - m_position);
    });
    return out;
}

Image CoverFlowView::cover(const std::string &path) const
{
    auto it = m_covers.find(path);
    if (it != m_covers.end())
        return it->second;
    Image img = path.empty() ? Image() : Image::fromData(artworkData(path), {300, 300});
    m_covers[path] = img;
    return img;
}

void CoverFlowView::drawCover(cairo_t *cr, const Album &a, const Placed &p, double size, double baseY) const
{
    if (p.alpha <= 0)
        return;
    Image img = cover(a.artPath);
    const bool placeholder = img.isNull();
    const double W = frame().w;
    const double f = (frame().h / 2) / std::tan(FieldOfView * M_PI / 360);   // the focal length, in points
    const double cx = W / 2 + p.x * size, rad = p.angle * M_PI / 180;
    const int columns = std::max(2, int(size / 2));
    for (int c = 0; c < columns; ++c) {
        // the column's two edges in 3D, then projected
        auto project = [&](double u, double &sx, double &scale) {
            const double off = (u - 0.5) * size;
            const double X = cx + off * std::cos(rad) - W / 2;
            const double Z = p.z * size - off * std::sin(rad);
            scale = f / (f + Z);
            sx = W / 2 + X * scale;
        };
        double x0, s0, x1, s1;
        project(double(c) / columns, x0, s0);
        project(double(c + 1) / columns, x1, s1);
        if (x1 < x0) {
            std::swap(x0, x1);
            std::swap(s0, s1);
        }
        const double scale = (s0 + s1) / 2, h = size * scale;
        const double top = baseY - h - (baseY - frame().h / 2) * (1 - scale);
        const double cw = std::max(1.0, x1 - x0 + 0.5);
        for (int pass = 0; pass < 2; ++pass) {   // the cover, then its reflection
            cairo_save(cr);
            cairo_rectangle(cr, x0, pass == 0 ? top : top + h, cw, h);
            cairo_clip(cr);
            if (placeholder) {
                cairo_set_source_rgba(cr, 0.35, 0.35, 0.35, pass == 0 ? 1 : 0.25);
                cairo_paint(cr);
            } else {
                // the column of the picture stretched to the column on screen
                const double srcX = double(c) / columns * img.width(), srcW = double(img.width()) / columns;
                cairo_translate(cr, x0, pass == 0 ? top : top + 2 * h);
                cairo_scale(cr, cw / srcW, (pass == 0 ? 1 : -1) * h / img.height());
                cairo_set_source_surface(cr, img.surface(), -srcX, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
                if (pass == 0) {
                    cairo_paint(cr);
                } else {
                    // 0.4 at the foot down to 0.05
                    cairo_pattern_t *mask = cairo_pattern_create_linear(0, img.height(), 0, 0);
                    cairo_pattern_add_color_stop_rgba(mask, 0, 0, 0, 0, 0.4);
                    cairo_pattern_add_color_stop_rgba(mask, 1, 0, 0, 0, 0.05);
                    cairo_mask(cr, mask);
                    cairo_pattern_destroy(mask);
                }
            }
            cairo_restore(cr);
        }
    }
}

void CoverFlowView::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(0));
    if (m_albums.empty())
        return;
    // the cover: 2/3 of the height (FUN_00403798), its foot a little under the middle, room for the
    // titles under the reflections
    const double size = std::floor(frame().h * 2 / 3 * 0.68);
    const double baseY = std::floor(frame().h * 0.56);
    for (const Placed &p : placements())
        drawCover(cr, m_albums[size_t(p.index)], p, size, baseY);
    const Album &a = m_albums[size_t(std::clamp(m_selected, 0, int(m_albums.size()) - 1))];
    AK::TextStyle t;
    t.font = Font::boldSystem(12);
    t.color = Color::gray(255);
    t.align = Align::Center;
    // the titles over the scroller (by eye)
    const double ty = scrollerRect().y - 40;
    drawText(cr, a.title, Rect{0, ty, frame().w, 16}, t);
    t.font = Font::system(11);
    t.color = Color::gray(160);
    drawText(cr, a.artist, Rect{0, ty + 17, frame().w, 15}, t);
    drawHud(cr);
}

// FUN_00408c7e, the HUD's controls: the scroller (`sbar`, PSCR 2022, 2023 when +0x87 is off) 16 high, its
// foot 23 from the view's foot (57 in full screen); its insets both the larger of 110 and what the
// buttons beside it take (the full screen button: its width + 35), so 110 in the window; the full
// screen button (`full`, PBUT 2020, 34 × 24, its second row pressed) at the right, 23 in
Rect CoverFlowView::scrollerRect() const
{
    const double W = frame().w, H = frame().h;
    const double inset = fullScreen ? 201 : 110;   // full screen: past Previous, Play, Next (57 + 31 + 5 + 37 + 5 + 31 + 35)
    const double foot = fullScreen ? 57 : 23;
    return {inset, H - foot - 16, std::max(0.0, W - 2 * inset), 16};
}

// the thumb (26 wide in the art, 8 kept at its ends) within the track less 20 at each end, as wide as
// one album's share, the middle album's place along it
Rect CoverFlowView::thumbRect() const
{
    const Rect r = scrollerRect();
    const double usable = std::max(0.0, r.w - 40);
    const double n = std::max<double>(1, m_albums.size());
    const double w = std::clamp(usable / n, 26.0, std::max(26.0, usable));
    const double f = m_albums.size() > 1 ? std::clamp(m_position / (n - 1), 0.0, 1.0) : 0;
    return {std::round(r.x + 20 + f * (usable - w)), r.y, w, 16};
}

Rect CoverFlowView::fullRect() const
{
    const Rect r = scrollerRect();
    return {frame().w - 23 - 34, std::round(r.y + 8 - 12), 34, 24};
}

void CoverFlowView::drawHud(cairo_t *cr) const
{
    const Image art = scroller(2022);
    const Rect r = scrollerRect();
    if (!art.isNull() && r.w >= 52) {
        // the track (0..52 of the art): 26 kept at the left, 25 at the right, the middle stretched
        art.draw(cr, Rect{0, 0, 26, 16}, Rect{r.x, r.y, 26, 16});
        art.draw(cr, Rect{26, 0, 1, 16}, Rect{r.x + 26, r.y, r.w - 51, 16});
        art.draw(cr, Rect{27, 0, 25, 16}, Rect{r.right() - 25, r.y, 25, 16});
        if (m_albums.size() > 1) {
            const Rect t = thumbRect();
            art.draw(cr, Rect{52, 0, 8, 16}, Rect{t.x, t.y, 8, 16});
            art.draw(cr, Rect{60, 0, 10, 16}, Rect{t.x + 8, t.y, std::max(0.0, t.w - 16), 16});
            art.draw(cr, Rect{70, 0, 8, 16}, Rect{t.right() - 8, t.y, 8, 16});
        }
    }
    const Image full = picture(2020);
    if (!full.isNull() && onFullScreen) {
        const Rect f = fullRect();
        full.draw(cr, Rect{0, m_pressFull ? 24.0 : 0.0, 34, 24}, f);
    }
}

void CoverFlowView::scrollTo(double x)
{
    const Rect r = scrollerRect(), t = thumbRect();
    const double usable = std::max(1.0, r.w - 40 - t.w);
    const double f = std::clamp((x - m_grab - r.x - 20) / usable, 0.0, 1.0);
    select(int(std::round(f * (std::max<size_t>(1, m_albums.size()) - 1))), true);
}

void CoverFlowView::mouseDragged(Event &e)
{
    if (m_dragThumb)
        scrollTo(e.location.x);
}

void CoverFlowView::mouseUp(Event &e)
{
    if (m_pressFull) {
        m_pressFull = false;
        setNeedsDisplay();
        if (fullRect().contains(e.location) && onFullScreen)
            App::shared().later([this] { onFullScreen(); });
    }
    m_dragThumb = false;
}

void CoverFlowView::mouseDown(Event &e)
{
    window()->makeFirstResponder(this);
    if (onFullScreen && fullRect().contains(e.location)) {
        m_pressFull = true;
        setNeedsDisplay();
        return;
    }
    if (m_albums.empty())
        return;
    if (scrollerRect().contains(e.location)) {
        const Rect t = thumbRect();
        m_dragThumb = true;
        m_grab = t.contains(e.location) ? e.location.x - t.x : t.w / 2;
        scrollTo(e.location.x);
        return;
    }
    // which cover: by its projected middle, the nearest to the pointer
    const double size = std::floor(frame().h * 2 / 3 * 0.68);
    int hit = m_selected;
    double best = 1e9;
    for (const Placed &p : placements()) {
        const double cx = frame().w / 2 + p.x * size;
        const double d = std::fabs(e.location.x - cx);
        if (d < best) {
            best = d;
            hit = p.index;
        }
    }
    if (hit == m_selected && e.clickCount == 2 && onPlay) {
        onPlay(m_albums[size_t(hit)].ids);
        return;
    }
    select(hit, true);
}

void CoverFlowView::scrollWheel(Event &e)
{
    if (e.deltaY != 0)
        select(m_selected + (e.deltaY > 0 ? 1 : -1), true);
}

void CoverFlowView::keyDown(Event &e)
{
    if (e.keysym == XKB_KEY_Left)
        select(m_selected - 1, true);
    else if (e.keysym == XKB_KEY_Right)
        select(m_selected + 1, true);
    else if (e.keysym == XKB_KEY_Escape && fullScreen && onFullScreen)
        App::shared().later([this] { onFullScreen(); });
    else if ((e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter) && onPlay && !m_albums.empty())
        onPlay(m_albums[size_t(m_selected)].ids);
    else
        View::keyDown(e);
}

} // namespace iTunes
