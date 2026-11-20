// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 11.4's Up Next (PlayQueuePopover at 0xbb6f78, PlayQueueHeaderView, PlayQueueCell and
// PlayQueueMsgCellView, their code at 0xba9000–0xbbc000; docs/itunes-reverse.md section 17):
//   the header: picture 8216 (512 × 31) stretched across; its title "Up Next" (text 3801.002) in
//   TextStyles' 1411 (HelveticaNeue-Medium 13, centred); the History button 8314 (18 × 18 cells, its
//   rows the clock plain and on) at its left; Clear (3801.035) on 8259 (69 × 22, 5-point ends) in
//   1409 (HelveticaNeue 11, centred) at its right.
//   The songs (PlayQueueCell, an ITTwoLineImageCellView): 50 high, the artwork 40 square, the name in
//   1401 (HelveticaNeue-Medium 12), the artist in 1402 (HelveticaNeue 12); under the pointer the
//   buttons 8452 (play, 20 × 20), 8454 (add) and 8459 (remove), their columns kind 3.
//   Nothing to come: 3801.010 "Start playback or add items to Up Next." in 1410 (Medium 12, centred).
//   The popover 400 wide, at most 500 high (0xbb5062).
// By eye: the places inside the header and the rows (5 from the edges, the artwork 5 in), the window
// in place of the popover, no History list, no scrolling past what fits.
#include "upnext11.h"

#include "resources.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace iTunes {

namespace {
constexpr double Width = 400, MaxHeight = 500, HeaderH = 31, RowH = 50, Art = 40;

AK::TextStyle style(int number, Color c = Color::gray(0))
{
    const iTunes::TextStyle s = textStyle(number);
    AK::TextStyle t;
    t.font = s.font;
    t.align = s.align;
    t.color = c;
    return t;
}
} // namespace

class UpNext11::Body : public View {
public:
    explicit Body(UpNext11 &owner) : m_owner(owner) {}
    void draw(cairo_t *cr) override
    {
        const double W = frame().w, H = frame().h;
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        // the header
        drawCell(cr, 8216, 0, 0, Rect{0, 0, W, HeaderH});
        const Size hist = cellSize(8314);
        drawCell(cr, 8314, stateColumn11(3, 0), 0, historyRect());
        (void)hist;
        drawText(cr, text(0x0ed90002), Rect{0, 0, W, HeaderH}, centred(style(1411)));
        const Rect clear = clearRect();
        drawCell(cr, 8259, stateColumn11(3, m_pressClear ? 1 : 0), 0, clear);
        drawText(cr, text(0x0ed90023), Rect{clear.x, clear.y + 4, clear.w, clear.h - 4}, centred(style(1409)));
        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        cairo_rectangle(cr, 0, HeaderH, W, 1);
        cairo_fill(cr);
        const auto &songs = m_owner.m_songs;
        if (songs.empty()) {
            drawText(cr, text(0x0ed9000a), Rect{0, HeaderH, W, H - HeaderH}, centred(style(1410, Color::gray(0.5))));
            return;
        }
        for (size_t i = 0; i < songs.size(); ++i) {
            const double y = HeaderH + 1 + i * RowH;
            if (y > H)
                break;
            const Song &s = songs[i];
            const Rect art{5, y + 5, Art, Art};
            if (!s.artwork.isNull())
                s.artwork.draw(cr, art);
            else {
                cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
                cairo_rectangle(cr, art.x, art.y, art.w, art.h);
                cairo_fill(cr);
            }
            const double tx = art.right() + 8, tw = W - tx - (int(i) == m_over ? 60 : 10);
            const AK::TextStyle n = style(1401), a = style(1402, Color::gray(0.45));
            const double lh = std::ceil(lineHeight(n.font));
            drawText(cr, s.name, Rect{tx, y + RowH / 2 - lh, tw, lh}, n);
            drawText(cr, s.artist, Rect{tx, y + RowH / 2, tw, lh}, a);
            if (int(i) == m_over) {
                drawCell(cr, 8452, stateColumn11(3, 0), 0, playRect(i));
                drawCell(cr, 8459, stateColumn11(3, 0), 0, removeRect(i));
            }
            cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
            cairo_rectangle(cr, tx, y + RowH - 1, W - tx, 1);
            cairo_fill(cr);
        }
    }
    void mouseMoved(Event &e) override
    {
        const int row = rowAt(e.location);
        if (row != m_over) {
            m_over = row;
            setNeedsDisplay();
        }
    }
    void mouseExited(Event &) override
    {
        m_over = -1;
        setNeedsDisplay();
    }
    void mouseDown(Event &e) override
    {
        if (clearRect().contains(e.location)) {
            m_pressClear = true;
            setNeedsDisplay();
            return;
        }
        const int row = rowAt(e.location);
        if (row < 0)
            return;
        if (playRect(size_t(row)).contains(e.location) || e.clickCount == 2) {
            if (m_owner.onPlay)
                m_owner.onPlay(size_t(row));
        } else if (removeRect(size_t(row)).contains(e.location)) {
            if (m_owner.onRemove)
                m_owner.onRemove(size_t(row));
        }
    }
    void mouseUp(Event &e) override
    {
        if (m_pressClear) {
            m_pressClear = false;
            setNeedsDisplay();
            if (clearRect().contains(e.location) && m_owner.onClear)
                m_owner.onClear();
        }
    }

private:
    static AK::TextStyle centred(AK::TextStyle t)
    {
        t.align = Align::Center;
        return t;
    }
    Rect historyRect() const
    {
        const Size s = cellSize(8314);
        return Rect{8, std::floor((HeaderH - s.h) / 2), s.w, s.h};
    }
    Rect clearRect() const
    {
        const Size s = cellSize(8259);
        return Rect{frame().w - 8 - s.w, std::floor((HeaderH - s.h) / 2), s.w, s.h};
    }
    Rect playRect(size_t i) const
    {
        const Size s = cellSize(8452);
        return Rect{frame().w - 10 - 2 * s.w - 6, HeaderH + 1 + i * RowH + (RowH - s.h) / 2, s.w, s.h};
    }
    Rect removeRect(size_t i) const
    {
        const Size s = cellSize(8459);
        return Rect{frame().w - 10 - s.w, HeaderH + 1 + i * RowH + (RowH - s.h) / 2, s.w, s.h};
    }
    int rowAt(Point p) const
    {
        if (p.y < HeaderH + 1)
            return -1;
        const int r = int((p.y - HeaderH - 1) / RowH);
        return r < int(m_owner.m_songs.size()) ? r : -1;
    }
    UpNext11 &m_owner;
    int m_over = -1;
    bool m_pressClear = false;
};

UpNext11::UpNext11() = default;
UpNext11::~UpNext11() = default;

void UpNext11::toggle()
{
    if (isShown()) {
        m_window->close();
        return;
    }
    if (!m_window) {
        m_window = std::make_unique<Window>(Size{Width, MaxHeight}, text(0x0ed90002));
        m_window->setFloatingPanel(true);
        m_body = m_window->contentView()->add<Body>(*this);
        m_window->onResize = [this](Size s) { m_body->setFrame({0, 0, s.w, s.h}); };
    }
    setSongs(m_songs);
    m_window->show();
}

bool UpNext11::isShown() const { return m_window && m_window->isVisible(); }

void UpNext11::setSongs(std::vector<Song> songs)
{
    m_songs = std::move(songs);
    if (!m_window)
        return;
    const double h = std::clamp(HeaderH + 1 + std::max<size_t>(m_songs.size(), 2) * RowH, HeaderH + 1 + 2 * RowH, MaxHeight);
    m_window->setContentSize({Width, h});
    m_body->setFrame({0, 0, Width, h});
    m_body->setNeedsDisplay();
}

} // namespace iTunes
