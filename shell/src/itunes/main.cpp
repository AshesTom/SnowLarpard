// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes.app (8.2.1) on AquaKit, as Snow Leopard's DVD has it (docs/itunes-reverse.md): its menus from
// its own xMNU resources, its browser window laid out and drawn as BrowserWindow, GradientFrameView and
// the LCD's views do it, its art from iTunes.rsrc; the library in ~/Music/iTunes/iTunes Music Library.xml;
// playback by libmpv.
//   The top band: the content border of 54 points under the title bar, its gradient continuing the
//   title bar's (197 to 150 over the 76 points, 233 to 207 inactive, v(t) = start − d(0.5t + 1.5t² −
//   t³)), a line of grey 64 (135 inactive) under it; the transport (316, 320, 318), the volume (350,
//   352, the speakers 328), the LCD (153, 200 wide plus two thirds of the width past 700, centred), the
//   view buttons (540–542) and the search field (7/16 of the room right of the LCD).
//   The bottom band: 29 points, 191 to 150, its top line; the buttons 20 high, 4 above the bottom:
//   new playlist 431, shuffle 433, repeat 432, artwork 654 from the left at 15; Genius 436 and the
//   Genius sidebar 470 from the right at 18; the totals in Txtr 304.
//   The body: the source list (200 wide, at least 150), a line, the songs (Name, Time, Artist, Album,
//   Genre, Plays: CWid 9000's widths), at least 300 wide.
#include "appkit.h"
#include "controls.h"
#include "foundation.h"
#include "mainmenu.h"
#include "panels.h"
#include "text.h"

#include "library.h"
#include "player.h"
#include "resources.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace AK;
using namespace iTunes;

namespace {

// ---- GradientFrameView's gradients (FUN_00028bba, FUN_00028dc6; the greys at 0xa5fbc0)

double gradientValue(double start, double end, int row, int rows)
{
    const double t = rows > 1 ? double(row) / (rows - 1) : 0, d = start - end;
    return std::round(t * t * t * d - t * t * d * 1.5 - d * 0.5 * t + start);
}

// rows [from, to) of a gradient `rows` tall (90 at most, then its end grey) into r
void drawGradient(cairo_t *cr, const Rect &r, double start, double end, int rows, int from)
{
    const int span = std::min(rows, 90);
    for (int y = 0; y < int(r.h); ++y) {
        const int row = from + y;
        const double v = row < span ? gradientValue(start, end, row, span) : end;
        fill(cr, Rect{r.x, r.y + y, r.w, 1}, Color::gray(int(v)));
    }
}

// A text with its shadow under it (the sources' and the selection's), as iTunes' FUN_002985e2 draws
void drawShadowed(cairo_t *cr, const std::string &text, const Rect &r, AK::TextStyle t, const Color &shadow)
{
    const Color c = t.color;
    t.color = shadow;
    drawText(cr, text, r.translated(0, 1), t);
    t.color = c;
    drawText(cr, text, r, t);
}

constexpr double TitleBarHeight = 22, TopBandHeight = 54, BottomBandHeight = 29;

// ---- a picture button (UIPictureButton): its picture's cell for its state, its row its value

class PictureButton : public View {
public:
    PictureButton(int picture, int row = 0) : m_picture(picture), m_row(row) {}
    std::function<void()> onClick;
    void setPicture(int p) { m_picture = p; setNeedsDisplay(); }
    void setRow(int row) { m_row = row; setNeedsDisplay(); }
    int row() const { return m_row; }
    void draw(cairo_t *cr) override
    {
        // FUN_0002c5f8: 2 pressed, 1 enabled in an active window, 0 disabled; 5 and 4 in an inactive one
        const bool key = windowIsKey();
        const int state = m_pressed ? 2 : isEnabled() ? (key ? 1 : 5) : (key ? 0 : 4);
        const PictureInfo info = pictureInfo(m_picture);
        drawCell(cr, m_picture, stateColumn(info.kind, state), m_row, bounds());
    }
    void mouseDown(Event &) override
    {
        if (!isEnabled())
            return;
        m_pressed = true;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        const bool in = bounds().contains(e.location);
        if (in != m_pressed) {
            m_pressed = in;
            setNeedsDisplay();
        }
    }
    void mouseUp(Event &e) override
    {
        const bool fire = m_pressed && bounds().contains(e.location);
        m_pressed = false;
        setNeedsDisplay();
        if (fire && onClick)
            onClick();
    }

private:
    int m_picture, m_row;
    bool m_pressed = false;
};

// ---- the volume: the track 350 stretched, the thumb 352 (three cells: disabled, normal, pressed)

class VolumeSlider : public View {
public:
    std::function<void(double)> onChange;
    double value = 1;
    void draw(cairo_t *cr) override
    {
        drawCell(cr, 350, 0, 0, Rect{0, 0, frame().w, frame().h});
        const Size thumb = cellSize(352);
        const double x = std::round(value * (frame().w - thumb.w));
        const int state = m_dragging ? 2 : 1;
        drawCell(cr, 352, stateColumn(3, state), 0, Rect{x, 0, thumb.w, thumb.h});
    }
    void mouseDown(Event &e) override
    {
        m_dragging = true;
        track(e.location.x);
    }
    void mouseDragged(Event &e) override { track(e.location.x); }
    void mouseUp(Event &) override
    {
        m_dragging = false;
        setNeedsDisplay();
    }
    void setValue(double v)
    {
        value = std::clamp(v, 0.0, 1.0);
        setNeedsDisplay();
    }

private:
    void track(double x)
    {
        const double w = cellSize(352).w;
        setValue((x - w / 2) / std::max(1.0, frame().w - w));
        if (onChange)
            onChange(value);
    }
    bool m_dragging = false;
};

// ---- the LCD (BrowserLCD: LCDBackgroundView, LCDStaticTextView, LCDProgressBarView)

class LCD : public View {
public:
    std::string line1, line2;
    double position = 0, duration = 0;   // seconds
    bool showing = false;                // a song: else the Apple
    std::function<void(double)> onSeek;

    LCD()
    {
        m_timer = App::shared().addTimer(1.0 / 15, true, [this] { tick(); });
    }
    ~LCD() override { App::shared().removeTimer(m_timer); }

    void setLines(const std::string &a, const std::string &b)
    {
        if (a != line1 || b != line2) {
            line1 = a;
            line2 = b;
            m_scroll[0] = m_scroll[1] = 0;
            m_wait = 0;
        }
        setNeedsDisplay();
    }

    Rect barRect() const
    {
        // LCDProgressBarView (FUN_0005f712): 15 to 5 above the bottom, 25 in from each side, at most 240
        Rect r{25, frame().h - 15, frame().w - 50, 10};
        if (r.w > 240) {
            r.x += (r.w - 240) / 2;
            r.w = 240;
        }
        return r;
    }

    void draw(cairo_t *cr) override
    {
        // the background: picture 153, its second column when inactive (10.5 and later)
        drawCell(cr, 153, windowIsKey() ? 0 : 1, 0, bounds());
        if (!showing) {
            const Image apple = picture(154);
            apple.draw(cr, Point{std::floor((frame().w - apple.width()) / 2), std::floor((frame().h - apple.height()) / 2) + 1});
            return;
        }
        const iTunes::TextStyle lcd = textStyle(302);
        drawLine(cr, 0, line1, Rect{8, 3, frame().w - 16, 14}, lcd);
        drawLine(cr, 1, line2, Rect{8, 16, frame().w - 16, 14}, lcd);
        // the progress bar: the frame 475, the fill 478 to the part played
        const Rect bar = barRect();
        drawCell(cr, 475, 0, 0, bar);
        const Rect inner{bar.x + 2, bar.y + 1, bar.w - 4, bar.h - 3};
        if (duration > 0) {
            const double w = std::floor(inner.w * std::clamp(position / duration, 0.0, 1.0));
            drawCell(cr, 478, 0, 0, Rect{inner.x, inner.y, w, inner.h});
        }
        // the times in the margins either side of the bar
        AK::TextStyle t;
        t.font = lcd.font;
        t.font.size = 10;
        t.color = lcd.color;
        t.elide = false;
        t.align = Align::Right;
        drawText(cr, formatTime(int(position * 1000)), Rect{2, bar.y - 2, bar.x - 4, 13}, t);
        t.align = Align::Left;
        if (duration > 0)
            drawText(cr, "-" + formatTime(int(std::max(0.0, duration - position) * 1000)),
                     Rect{bar.right() + 2, bar.y - 2, frame().w - bar.right() - 4, 13}, t);
    }

    void mouseDown(Event &e) override
    {
        const Rect bar = barRect();
        if (showing && duration > 0 && Rect{bar.x, bar.y - 3, bar.w, bar.h + 6}.contains(e.location))
            seekAt(e.location.x);
    }
    void mouseDragged(Event &e) override
    {
        if (showing && duration > 0)
            seekAt(e.location.x);
    }

private:
    void seekAt(double x)
    {
        const Rect bar = barRect();
        const double f = std::clamp((x - bar.x - 2) / std::max(1.0, bar.w - 4), 0.0, 1.0);
        if (onSeek)
            onSeek(f * duration);
    }

    // LCDStaticTextView: centred when it fits; else scrolling left, twice with a 10 point gap, a frame
    // every 1/15 s after 1.5 s (FUN_0004ecf6)
    void drawLine(cairo_t *cr, int n, const std::string &text, const Rect &r, const iTunes::TextStyle &style)
    {
        AK::TextStyle t;
        t.font = style.font;
        t.color = style.color;
        t.elide = false;
        const double w = textWidth(text, style.font);
        m_width[n] = w;
        m_room[n] = r.w;
        if (w <= r.w) {
            t.align = Align::Center;
            drawText(cr, text, r, t);
            return;
        }
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_clip(cr);
        t.align = Align::Left;
        const double x = r.x - m_scroll[n];
        drawText(cr, text, Rect{x, r.y, w + 2, r.h}, t);
        drawText(cr, text, Rect{x + w + 10, r.y, w + 2, r.h}, t);
        cairo_restore(cr);
    }

    void tick()
    {
        if (!showing)
            return;
        bool moved = false;
        if (++m_wait < 22)   // 1.5 s
            return;
        for (int n = 0; n < 2; ++n)
            if (m_width[n] > m_room[n]) {
                m_scroll[n] += 1;
                if (m_scroll[n] >= m_width[n] + 10)
                    m_scroll[n] = 0;
                moved = true;
            }
        if (moved)
            setNeedsDisplay();
    }

    int m_timer = 0, m_wait = 0;
    double m_scroll[2] = {0, 0}, m_width[2] = {0, 0}, m_room[2] = {0, 0};
};

// ---- the bands

// ---- the window's own frame (GradientFrameView, WindowTitleBarView: GradientWindow.nib's document
//   window). The title bar is the gradient's first 22 rows; along its top the bevel 7000 between its
//   ends 7001 and 7002 (white at 40%, the window's outline rounding its corners); the title
//   (WindowTitleTextView 1001, TXTS 730: Txtr 13000, Lucida Grande 13, its shadow #C4C4C4 a point
//   under it) from 102 to 102 points from the right, 1 to 19 down; the buttons 1050–1052 at 8, 29, 50,
//   4 down (WindowFrameButtonView: 13004–13006's columns inactive, normal, rollover, pressed, all three
//   in rollover while the pointer is over any of them). Its gradient, its controls aside, moves the
//   window; a double click in the title bar puts it in the Dock.

class FrameButtons : public View {
public:
    std::function<void(int)> onClick;   // 1 close, 2 minimize, 3 zoom (WFBt)
    void draw(cairo_t *cr) override
    {
        const bool key = windowIsKey();
        for (int i = 0; i < 3; ++i) {
            // FUN_0002b9f0's kind 4: 0 inactive, 1 normal, 3 rollover (column 2), 2 pressed (column 3)
            const int state = m_pressed == i && m_inside ? 2 : m_over ? 3 : key ? 1 : 0;
            const Size cell = cellSize(13004 + i);
            drawCell(cr, 13004 + i, stateColumn(4, state), 0, Rect{Offsets[i], 0, cell.w, cell.h});
        }
    }
    void mouseMoved(Event &e) override { setOver(buttonAt(e.location) >= 0 || overUnion(e.location)); }
    void mouseExited(Event &) override { setOver(false); }
    void mouseDown(Event &e) override
    {
        m_pressed = buttonAt(e.location);
        m_inside = m_pressed >= 0;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        const bool in = m_pressed >= 0 && buttonAt(e.location) == m_pressed;
        if (in != m_inside) {
            m_inside = in;
            setNeedsDisplay();
        }
    }
    void mouseUp(Event &e) override
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

private:
    static constexpr double Offsets[3] = {0, 21, 42};   // 8, 29, 50 in the title bar
    int buttonAt(Point p) const
    {
        for (int i = 0; i < 3; ++i)
            if (Rect{Offsets[i], 0, 14, 15}.contains(p))
                return i;
        return -1;
    }
    // the tracking area: the union of the three (FUN_0001b9c8)
    bool overUnion(Point p) const { return Rect{0, 0, Offsets[2] + 14, 15}.contains(p); }
    void setOver(bool over)
    {
        if (over != m_over) {
            m_over = over;
            setNeedsDisplay();
        }
    }
    int m_pressed = -1;
    bool m_inside = false, m_over = false;
};

// A band of the frame: a drag on it moves the window (GradientFrameView's own clicks)
class FrameBand : public View {
public:
    std::function<void()> onDoubleClickTitle;
    void mouseDown(Event &e) override
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
    void mouseDragged(Event &e) override
    {
        if (Window *w = window())
            w->dragMove(m_from, e.screen, false);
    }

protected:
    double m_titleBar = 0;

private:
    Point m_from;
};

class TopBand : public FrameBand {
public:
    TopBand() { m_titleBar = TitleBarHeight; }
    std::string title;
    void draw(cairo_t *cr) override
    {
        // the title bar's 22 points and the content border's 54: one gradient of 76 rows
        const bool key = windowIsKey();
        const double w = frame().w;
        drawGradient(cr, bounds(), key ? 197 : 233, key ? 150 : 207, int(TitleBarHeight + TopBandHeight), 0);
        fill(cr, Rect{0, frame().h - 1, w, 1}, Color::gray(key ? 64 : 135));
        // the bevel along the top (FUN_0002776a): 7001, 7000 stretched, 7002, at their own height
        const Size end = cellSize(7001);
        drawCell(cr, 7001, 0, 0, Rect{0, 0, end.w, end.h});
        drawCell(cr, 7000, 0, 0, Rect{end.w, 0, std::max(0.0, w - 2 * end.w), end.h});
        drawCell(cr, 7002, 0, 0, Rect{w - end.w, 0, end.w, end.h});
        // the title: TXTS 730
        const iTunes::TextStyle s = textStyle(13000);
        AK::TextStyle t;
        t.font = s.font;
        t.color = key ? Color::gray(0) : Color::gray(0, 0.5);
        t.align = Align::Center;
        const Rect r{102, 1, std::max(0.0, w - 204), 18};
        if (key)
            drawShadowed(cr, title, r, t, Color::rgb(0xc4, 0xc4, 0xc4));
        else
            drawText(cr, title, r, t);
    }
};

class BottomBand : public FrameBand {
public:
    std::string totals;
    void draw(cairo_t *cr) override
    {
        const bool key = windowIsKey();
        drawGradient(cr, bounds(), key ? 191 : 229, key ? 150 : 207, int(BottomBandHeight), 0);
        fill(cr, Rect{0, 0, frame().w, 1}, Color::gray(key ? 64 : 135));
        const iTunes::TextStyle s = textStyle(304);
        AK::TextStyle t;
        t.font = s.font;
        t.color = key ? Color::gray(0) : Color::gray(94);
        t.align = Align::Center;
        drawText(cr, totals, Rect{200, 5, frame().w - 400, 18}, t);
    }
};

// ---- the songs (TrackDisplay, ColumnDisplay)
//   The header: ListHeader (PBUT 304), 16 high, its cell kept 1 on the left and 16 on the right; the
//   sorted column's rows 1 (descending) and 2 (ascending); its titles in Txtr 9004 (Lucida Grande 11
//   bold, grey 50). The rows: List Contents (Small), Txtr 9002 (Lucida Grande 11, black), the line's
//   height plus ACfg's line gap (2), the text a point up (ACfg's centre tweak, −1). Their colours
//   (FUN_0040ef86): rows 0, 2, 4… colour 14 (241 245 250), the others 16 (white), the lines between the
//   columns grey 217; selected with the keyboard in an active window colour 10 (61 128 223), its lines
//   19, its text 22 (white); otherwise 11 (192), 20 and 23 (black). The song playing: TrackStatusIcons
//   (PBUT 306), the speaker with its waves, the bottom row's when selected.

struct Column {
    std::string title;
    double width;
    Align align;
};

double listRowHeight() { return std::ceil(lineHeight(textStyle(9002).font)) + 2; }

class TrackList : public View {
public:
    std::vector<Column> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<int> ids;
    int selected = -1, playing = -1;   // rows
    bool playingPaused = false;
    int sortColumn = 1;
    bool ascending = true;
    std::function<void(int)> onDoubleClick;
    std::function<void(int)> onSort;
    static constexpr double HeaderHeight = 16;
    double rowHeight = 16;

    TrackList()
    {
        rowHeight = listRowHeight();
        m_scroll = add<ScrollView>();
        m_scroll->lineStep = rowHeight;
        m_body = m_scroll->document()->add<Body>(this);
    }
    void setFrame(const Rect &r) override
    {
        View::setFrame(r);
        reload();
    }
    void reload()
    {
        m_scroll->setFrame({0, HeaderHeight, frame().w, frame().h - HeaderHeight});
        m_scroll->setDocumentHeight(std::max(double(rows.size()) * rowHeight, m_scroll->frame().h));
        m_body->setFrame(m_scroll->document()->bounds());
        setNeedsDisplay();
        m_body->setNeedsDisplay();
    }
    void scrollToRow(int r)
    {
        if (r >= 0)
            m_scroll->scrollToVisible({0, r * rowHeight, 10, rowHeight});
    }
    View *body() const { return m_body; }
    void draw(cairo_t *cr) override
    {
        const iTunes::TextStyle head = textStyle(9004);
        const int state = windowIsKey() ? 1 : 0;
        double x = 0;
        for (size_t c = 0; c < columns.size(); ++c) {
            const double w = c + 1 == columns.size() ? std::max(columns[c].width, frame().w - x) : columns[c].width;
            const int row = int(c) == sortColumn ? (ascending ? 2 : 1) : 0;
            drawCell(cr, 304, stateColumn(pictureInfo(304).kind, m_pressed == int(c) ? 2 : state), row, Rect{x, 0, w, HeaderHeight});
            AK::TextStyle t;
            t.font = head.font;
            t.color = head.color;
            t.align = columns[c].align;
            drawText(cr, columns[c].title, Rect{x + 4, 0, columns[c].width - (int(c) == sortColumn ? 20 : 8), HeaderHeight - 1}, t);
            x += w;
        }
    }
    void mouseDown(Event &e) override
    {
        if (e.location.y >= HeaderHeight)
            return;
        m_pressed = columnAt(e.location.x);
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        const int c = m_pressed;
        m_pressed = -1;
        setNeedsDisplay();
        if (c >= 0 && c == columnAt(e.location.x) && onSort)
            onSort(c);
    }
    bool acceptsFirstResponder() const override { return true; }
    void focusChanged() override
    {
        setNeedsDisplay();
        m_body->setNeedsDisplay();
    }
    void keyDown(Event &e) override
    {
        if (e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_Up) {
            if (rows.empty())
                return;
            selected = std::clamp(selected + (e.keysym == XKB_KEY_Down ? 1 : -1), 0, int(rows.size()) - 1);
            scrollToRow(selected);
            m_body->setNeedsDisplay();
            return;
        }
        if ((e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter) && selected >= 0 && onDoubleClick) {
            onDoubleClick(selected);
            return;
        }
        View::keyDown(e);
    }

private:
    int columnAt(double px) const
    {
        double x = 0;
        for (size_t c = 0; c < columns.size(); ++c) {
            if (px >= x && px < x + columns[c].width)
                return int(c);
            x += columns[c].width;
        }
        return -1;
    }
    struct Body : View {
        TrackList *list;
        explicit Body(TrackList *l) : list(l) {}
        void draw(cairo_t *cr) override
        {
            const bool focused = list->isFirstResponder() && windowIsKey();
            const double h = list->rowHeight;
            const int n = int(std::ceil(frame().h / h));
            const iTunes::TextStyle body = textStyle(9002);
            for (int r = 0; r < n; ++r) {
                const double y = r * h;
                const bool sel = r == list->selected && size_t(r) < list->rows.size();
                Color bg = colour(r % 2 == 0 ? 14 : 16), line = Color::gray(217), text = colour(23);
                if (sel) {
                    bg = colour(focused ? 10 : 11);
                    line = colour(focused ? 19 : 20);
                    text = colour(focused ? 22 : 23);
                }
                fill(cr, Rect{0, y, frame().w, h}, bg);
                double x = 0;
                for (size_t c = 0; c < list->columns.size(); ++c) {
                    const Column &col = list->columns[c];
                    if (size_t(r) < list->rows.size()) {
                        if (c == 0) {
                            if (r == list->playing)
                                drawCell(cr, 306, list->playingPaused ? 0 : 2, sel && focused ? 1 : 0,
                                         Rect{x + std::floor((col.width - 12) / 2), y + std::floor((h - 12) / 2), 12, 12});
                        } else {
                            AK::TextStyle t;
                            t.font = body.font;
                            t.align = col.align;
                            t.color = text;
                            const std::string &cell = c < list->rows[size_t(r)].size() ? list->rows[size_t(r)][c] : std::string();
                            drawText(cr, cell, Rect{x + 4, y - 1, col.width - 8, h}, t);
                        }
                    }
                    x += col.width;
                    fill(cr, Rect{x - 1, y, 1, h}, line);
                }
            }
        }
        void mouseDown(Event &e) override
        {
            const int r = int(e.location.y / list->rowHeight);
            window()->makeFirstResponder(list);
            if (r >= 0 && size_t(r) < list->rows.size()) {
                list->selected = r;
                if (e.clickCount == 2 && list->onDoubleClick)
                    list->onDoubleClick(r);
            }
            setNeedsDisplay();
            list->setNeedsDisplay();
        }
    };
    ScrollView *m_scroll;
    Body *m_body;
    int m_pressed = -1;
};

// ---- the sources (SourceDisplay: FUN_0016cb22 draws a row, FUN_0002d3b2 places its parts)
//   The background: colour 27 (209 215 226), 28 (232) in an inactive window on 10.5 and later
//   (FUN_00021406). Rows 20 high (the small size). A header ("LIBRARY"): Txtr 9004 at x 9, colour 29
//   (80 93 109; 30 inactive) over its shadow 31 (243 245 248) a point lower. An item of level n: its
//   icon (Source Icons (Small), 441: column 1 when selected) at 14n + 19, its name in Txtr 9002 at
//   14n + 39 to 4 from the right, centred in the row; black (32), or when selected white (33) over its
//   shadow 34 (27 67 121; 35, 83 95 120, without the keyboard). The selection: picture 300 across the
//   row, its column 0 with the keyboard in an active window, else 1.

struct SourceRow {
    std::string title;
    int icon = -1;   // 441's row
    bool header = false, selectable = true;
    int level = 0;
    int tag = 0;
};

class SourceList : public View {
public:
    std::vector<SourceRow> rows;
    static constexpr double RowHeight = 20;
    std::function<void(int)> onSelect;
    int selected() const { return m_selected; }
    void select(int i, bool notify = false)
    {
        if (i < 0 || size_t(i) >= rows.size() || !rows[size_t(i)].selectable)
            return;
        m_selected = i;
        setNeedsDisplay();
        if (notify && onSelect)
            onSelect(i);
    }
    double contentHeight() const { return double(rows.size()) * RowHeight; }
    bool acceptsFirstResponder() const override { return true; }
    void focusChanged() override { setNeedsDisplay(); }
    void draw(cairo_t *cr) override
    {
        const bool key = windowIsKey();
        const bool focused = key && isFirstResponder();
        fill(cr, bounds(), colour(key ? 27 : 28));
        const iTunes::TextStyle head = textStyle(9004), name = textStyle(9002);
        const Image icons = picture(441);
        for (size_t i = 0; i < rows.size(); ++i) {
            const SourceRow &r = rows[i];
            const Rect row{0, double(i) * RowHeight, frame().w, RowHeight};
            const bool sel = int(i) == m_selected;
            if (sel)
                drawCell(cr, 300, focused ? 0 : 1, 0, row);
            AK::TextStyle t;
            if (r.header) {
                t.font = head.font;
                t.color = colour(key ? 29 : 30);
                drawShadowed(cr, r.title, Rect{9, row.y, row.w - 13, RowHeight}, t, colour(31));
                continue;
            }
            const double ix = 14 * r.level + 19, tx = 14 * r.level + 39;
            if (r.icon >= 0 && !icons.isNull())
                drawCell(cr, 441, sel ? 1 : 0, r.icon, Rect{ix, row.y + 2, 16, 16});
            t.font = name.font;
            if (sel) {
                t.color = colour(33);
                drawShadowed(cr, r.title, Rect{tx, row.y, row.w - tx - 4, RowHeight}, t, colour(focused ? 34 : 35));
            } else {
                t.color = r.selectable ? colour(32) : Color::gray(0, 0.5);
                drawText(cr, r.title, Rect{tx, row.y, row.w - tx - 4, RowHeight}, t);
            }
        }
    }
    void mouseDown(Event &e) override
    {
        window()->makeFirstResponder(this);
        select(int(e.location.y / RowHeight), true);
    }
    void keyDown(Event &e) override
    {
        if (e.keysym != XKB_KEY_Down && e.keysym != XKB_KEY_Up) {
            View::keyDown(e);
            return;
        }
        const int step = e.keysym == XKB_KEY_Down ? 1 : -1;
        for (int i = m_selected + step; i >= 0 && size_t(i) < rows.size(); i += step)
            if (rows[size_t(i)].selectable) {
                select(i, true);
                return;
            }
    }

private:
    int m_selected = -1;
};

// ---- the application

class ITunesApp {
public:
    ITunesApp()
    {
        if (!m_library.load()) {
            m_library.add({Library::musicFolder()});
            m_library.save();
        }
        buildWindow();
        buildMenus();
        loadSource();
        m_timer = App::shared().addTimer(0.25, true, [this] { refreshLCD(); });
        m_player.onEnd = [this] { advance(1, true); };
        m_player.onError = [this](const std::string &) { advance(1, true); };
    }

    void comeForward() { m_window->orderFront(); }

    void addFiles(const std::vector<std::string> &paths)
    {
        if (paths.empty())
            return;
        m_library.add(paths);
        m_library.save();
        loadSource();
    }

private:
    // ---- the window (FUN_0000fe66: a document window, at least 700 x 400)

    void buildWindow()
    {
        m_window = std::make_unique<Window>(Size{1000, 620 + TitleBarHeight}, "iTunes");
        // its own frame (GradientFrameView): no title bar from the window server, the outline's rounded
        // corners, the bottom ones too
        m_window->setBorderless();
        m_window->setRoundedBottomCorners(true);
        m_window->setResizable({700, 400});
        m_window->onResize = [this](Size) { layout(); };
        m_window->onKeyChanged = [this](bool) { m_window->contentView()->setNeedsDisplay(); };
        m_window->onUnhandledKey = [this](Event &e) { m_menu.performKeyEquivalent(e); };
        m_window->onClose = [] { App::shared().quit(); };
        View *c = m_window->contentView();

        m_top = c->add<TopBand>();
        m_top->title = "iTunes";
        m_top->onDoubleClickTitle = [this] { m_window->miniaturize(); };
        m_frameButtons = m_top->add<FrameButtons>();
        m_frameButtons->onClick = [this](int kind) {
            if (kind == 1)
                m_window->close();
            else if (kind == 2)
                m_window->miniaturize();
            else
                zoom();
        };
        m_prev = m_top->add<PictureButton>(316);
        m_play = m_top->add<PictureButton>(320);
        m_next = m_top->add<PictureButton>(318);
        m_prev->onClick = [this] { previous(); };
        m_play->onClick = [this] { playPause(); };
        m_next->onClick = [this] { advance(1, false); };
        m_speakerLow = m_top->add<PictureButton>(328, 0);
        m_speakerHigh = m_top->add<PictureButton>(328, 2);
        m_speakerLow->onClick = [this] { setVolume(0); };
        m_speakerHigh->onClick = [this] { setVolume(1); };
        m_volume = m_top->add<VolumeSlider>();
        m_volume->onChange = [this](double v) { m_player.setVolume(v); };
        m_volume->setValue(Settings("itunes").integer("Volume", 100) / 100.0);
        m_player.setVolume(m_volume->value);
        m_lcd = m_top->add<LCD>();
        m_lcd->onSeek = [this](double s) {
            m_player.seek(s);
            refreshLCD();
        };
        for (int i = 0; i < 3; ++i) {
            m_views[i] = m_top->add<PictureButton>(540 + i, i == 0 ? 1 : 0);
            m_views[i]->setEnabled(i == 0);   // the list alone: no grid, no Cover Flow
        }
        m_search = m_top->add<SearchField>();
        m_search->placeholder = tr("kSearchFieldPrompt") == "kSearchFieldPrompt" ? "Search" : tr("kSearchFieldPrompt");
        m_search->onChange = [this](const std::string &) { loadSource(); };

        m_sourceScroll = c->add<ScrollView>();
        m_sources = m_sourceScroll->document()->add<SourceList>();
        m_sources->onSelect = [this](int) { loadSource(); };
        m_divider = c->add<View>();

        m_list = c->add<TrackList>();
        // the status column (Play Status, the speaker), then Name, Time, Artist, Album, Genre, Play Count:
        // STR# 9006's titles, CWid 9000's default widths
        m_list->columns = {{"", 45, Align::Left},          {tr("9006.001"), 200, Align::Left}, {tr("9006.009"), 59, Align::Right},
                           {tr("9006.003"), 125, Align::Left}, {tr("9006.002"), 125, Align::Left}, {tr("9006.006"), 80, Align::Left},
                           {tr("9006.019"), 82, Align::Right}};
        m_list->onDoubleClick = [this](int row) { playRow(row); };
        m_list->onSort = [this](int c) {
            if (m_list->sortColumn == c)
                m_list->ascending = !m_list->ascending;
            else {
                m_list->sortColumn = c;
                m_list->ascending = true;
            }
            loadSource();
        };

        m_bottom = c->add<BottomBand>();
        m_newPlaylist = m_bottom->add<PictureButton>(431, 0);
        m_newPlaylist->onClick = [this] { newPlaylist(); };
        m_shuffleButton = m_bottom->add<PictureButton>(433, 0);
        m_shuffleButton->onClick = [this] { setShuffle(!m_shuffle); };
        m_repeatButton = m_bottom->add<PictureButton>(432, 0);
        m_repeatButton->onClick = [this] { setRepeat((m_repeat + 1) % 3); };
        m_artwork = m_bottom->add<PictureButton>(654, 0);
        m_artwork->setEnabled(false);
        m_geniusSidebar = m_bottom->add<PictureButton>(470, 0);
        m_geniusSidebar->setEnabled(false);
        m_genius = m_bottom->add<PictureButton>(436, 0);
        m_genius->setEnabled(false);

        Settings s("itunes");
        m_shuffle = s.boolean("Shuffle", false);
        m_repeat = s.integer("Repeat", 0);
        m_shuffleButton->setRow(m_shuffle ? 1 : 0);
        m_repeatButton->setRow(m_repeat);

        rebuildSources();
        layout();
        m_window->setIcon(Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/iTunes.icns"), {128, 128}));
        m_window->show();
        m_window->makeFirstResponder(m_list);
    }

    // BrowserWindow's layout pass (FUN_000249e4) and the top bar's places (FUN_00142cb2)
    // The zoom button: iTunes' mini player (the window down to its top band), not yet ported
    void zoom() {}

    void layout()
    {
        const Size s = m_window->contentView()->frame().w > 0 ? Size{m_window->contentView()->frame().w, m_window->contentView()->frame().h}
                                                              : m_window->size();
        const double W = s.w, H = s.h;
        // the frame's top: the title bar, then the content border; the controls under the title bar
        const double T = TitleBarHeight;
        m_top->setFrame({0, 0, W, T + TopBandHeight});
        m_frameButtons->setFrame({8, 4, 56, 16});
        // the transport
        m_prev->setFrame({24, T + 8, 31, 32});
        m_play->setFrame({60, T + 4, 39, 39});
        m_next->setFrame({104, T + 8, 31, 32});
        // the LCD: 200, plus two thirds of the width past 700, 46 high, centred
        const double lcdW = W < 700 ? 200 : 200 + std::floor(2 * (W - 700) / 3);
        const Rect lcd{std::floor((W - lcdW) / 2), T, lcdW, 46};
        m_lcd->setFrame(lcd);
        // the volume: 78 wide at (168, 18), narrower when the LCD leaves less (FUN_0013be44)
        const double volW = std::min(78.0, std::max(20.0, lcd.x - 200));
        m_volume->setFrame({168, T + 18, volW, 12});
        m_speakerLow->setFrame({156, T + 17, 13.5, 13});
        m_speakerHigh->setFrame({168 + volW + 4, T + 17, 13.5, 13});
        // the search field: its right at W − 44, 7/16 of the room right of the LCD
        const double right = W - 44;
        const double left = W - 49 - std::floor(7 * std::max(0.0, W - 69 - lcd.right()) / 16);
        const double fieldH = 22;
        m_search->setFrame({left, T + 4 + std::floor((31 - fieldH) / 2), right - left, fieldH});
        // the view buttons: 27, 25, 27, ending 25 left of the search field, on the LCD's middle, 3 up
        const double widths[3] = {27, 25, 27};
        double x = left - 25 - (widths[0] + widths[1] + widths[2]);
        const double y = std::floor(lcd.y + (lcd.h - 22) / 2) - 3;
        for (int i = 0; i < 3; ++i) {
            m_views[i]->setFrame({x, y, widths[i], 22});
            x += widths[i];
        }

        // the body: the content between the bands; the source list, a line, the songs
        const double bodyTop = T + TopBandHeight, bodyH = std::max(0.0, H - bodyTop - BottomBandHeight);
        const double sourceW = std::clamp(m_sourceWidth, 150.0, std::max(150.0, W - 300 - 1));
        m_sourceScroll->setFrame({0, bodyTop, sourceW, bodyH});
        m_sources->setFrame({0, 0, sourceW - 15, std::max(bodyH, m_sources->contentHeight())});
        m_sourceScroll->setDocumentHeight(m_sources->frame().h);
        m_divider->setFrame({sourceW, bodyTop, 1, bodyH});
        m_list->setFrame({sourceW + 1, bodyTop, W - sourceW - 1, bodyH});

        // the bottom band: its buttons 20 high, 4 above the bottom
        m_bottom->setFrame({0, H - BottomBandHeight, W, BottomBandHeight});
        const double by = BottomBandHeight - 24;
        m_newPlaylist->setFrame({15, by, 40, 20});
        m_shuffleButton->setFrame({55, by, 39, 20});
        m_repeatButton->setFrame({94, by, 39, 20});
        m_artwork->setFrame({133, by, 39, 20});
        m_geniusSidebar->setFrame({W - 18 - 40, by, 40, 20});
        m_genius->setFrame({W - 18 - 40 - 39, by, 39, 20});
        m_window->contentView()->setNeedsDisplay();
    }

    // ---- the sources: LIBRARY (Music), STORE, PLAYLISTS

    void rebuildSources()
    {
        // Localizable.strings: STR# 135 (the headers), 137 (the Store), 9000 (Music, untitled playlist)
        const int keep = m_sources->selected();
        m_sources->rows.clear();
        auto header = [&](const char *key) {
            SourceRow h;
            h.header = true;
            h.selectable = false;
            h.title = tr(key);
            m_sources->rows.push_back(h);
        };
        header("135.011");
        SourceRow music;
        music.title = tr("9000.036");
        music.icon = 0;
        music.tag = -1;
        m_sources->rows.push_back(music);
        header("135.013");
        SourceRow store;
        store.title = tr("137.001");
        store.icon = 7;
        store.selectable = false;
        store.tag = -2;
        m_sources->rows.push_back(store);
        header("135.014");
        int i = 0;
        for (const Playlist &p : m_library.playlists()) {
            if (!p.master) {
                SourceRow r;
                r.title = p.name;
                r.icon = 15;
                r.tag = i;
                m_sources->rows.push_back(r);
            }
            ++i;
        }
        m_sources->setNeedsDisplay();
        m_sources->select(keep >= 0 && size_t(keep) < m_sources->rows.size() ? keep : 1);
    }

    // The songs of the selected source, filtered by the search field, sorted by the column
    void loadSource()
    {
        std::vector<int> ids;
        const int sel = m_sources->selected();
        const int tag = sel >= 0 && size_t(sel) < m_sources->rows.size() ? m_sources->rows[size_t(sel)].tag : -1;
        if (tag >= 0 && size_t(tag) < m_library.playlists().size())
            ids = m_library.playlists()[size_t(tag)].items;
        else
            for (const auto &[id, t] : m_library.tracks())
                ids.push_back(id);
        const std::string q = lower(m_search ? m_search->text : std::string());
        std::vector<int> shown;
        for (int id : ids) {
            const Track *t = m_library.track(id);
            if (!t)
                continue;
            if (!q.empty() && lower(t->name + " " + t->artist + " " + t->album + " " + t->genre).find(q) == std::string::npos)
                continue;
            shown.push_back(id);
        }
        const int col = m_list->sortColumn;
        const bool asc = m_list->ascending;
        auto key = [&](const Track &t) -> std::string {
            switch (col) {
            case 1: return lower(t.name);
            case 3: return lower(t.artist) + "\x01" + lower(t.album) + "\x01" + std::to_string(1000 + t.trackNumber);
            case 4: return lower(t.album) + "\x01" + std::to_string(1000 + t.trackNumber);
            case 5: return lower(t.genre) + "\x01" + lower(t.artist);
            default: return {};
            }
        };
        std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) {
            const Track &ta = *m_library.track(a), &tb = *m_library.track(b);
            bool less;
            if (col == 2)
                less = ta.totalTime < tb.totalTime;
            else if (col == 6)
                less = ta.playCount < tb.playCount;
            else
                less = key(ta) < key(tb);
            if (!asc) {
                const bool more = col == 2 ? tb.totalTime < ta.totalTime : col == 6 ? tb.playCount < ta.playCount : key(tb) < key(ta);
                return more;
            }
            return less;
        });
        m_list->ids = shown;
        m_list->rows.clear();
        long long ms = 0, bytes = 0;
        for (int id : shown) {
            const Track &t = *m_library.track(id);
            m_list->rows.push_back({std::string(), t.name, formatTime(t.totalTime), t.artist, t.album, t.genre,
                                    t.playCount ? std::to_string(t.playCount) : std::string()});
            ms += t.totalTime;
            bytes += t.size;
        }
        m_list->selected = std::min(m_list->selected, int(shown.size()) - 1);
        markPlaying();
        m_list->reload();
        m_bottom->totals = totals(shown.size(), ms, bytes);
        m_bottom->setNeedsDisplay();
    }

    static std::string lower(std::string s)
    {
        for (char &c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // "1234 songs, 3.4 days, 5.67 GB"
    static std::string totals(size_t n, long long ms, long long bytes)
    {
        if (n == 0)
            return {};
        char buf[128];
        const double hours = ms / 3600000.0;
        std::string time;
        if (hours >= 24)
            std::snprintf(buf, sizeof buf, "%.1f days", hours / 24);
        else if (hours >= 1)
            std::snprintf(buf, sizeof buf, "%.1f hours", hours);
        else
            std::snprintf(buf, sizeof buf, "%lld minutes", (ms + 30000) / 60000);
        time = buf;
        const double gb = bytes / 1073741824.0;
        if (gb >= 1)
            std::snprintf(buf, sizeof buf, "%.2f GB", gb);
        else
            std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
        return std::to_string(n) + (n == 1 ? " song, " : " songs, ") + time + ", " + buf;
    }

    // ---- playback

    void markPlaying()
    {
        m_list->playing = -1;
        for (size_t i = 0; i < m_list->ids.size(); ++i)
            if (m_list->ids[i] == m_current)
                m_list->playing = int(i);
    }

    void playRow(int row)
    {
        if (row < 0 || size_t(row) >= m_list->ids.size())
            return;
        m_queue = m_list->ids;
        m_queuePos = row;
        if (m_shuffle)
            shuffleQueue(row);
        playTrack(m_queue[size_t(m_queuePos)]);
    }

    void shuffleQueue(int first)
    {
        const int id = m_queue[size_t(first)];
        std::shuffle(m_queue.begin(), m_queue.end(), std::mt19937(std::random_device{}()));
        auto it = std::find(m_queue.begin(), m_queue.end(), id);
        std::iter_swap(m_queue.begin(), it);
        m_queuePos = 0;
    }

    void playTrack(int id)
    {
        Track *t = m_library.track(id);
        if (!t)
            return;
        m_current = id;
        m_player.play(t->path);
        t->playCount++;
        m_library.save();
        for (size_t i = 0; i < m_list->ids.size(); ++i)
            if (m_list->ids[i] == id && m_list->rows[i].size() > 6)
                m_list->rows[i][6] = std::to_string(t->playCount);
        markPlaying();
        m_list->body()->setNeedsDisplay();
        refreshLCD();
    }

    void playPause()
    {
        if (m_player.isPlaying())
            m_player.pause();
        else if (m_player.isPaused())
            m_player.resume();
        else if (m_list->selected >= 0)
            playRow(m_list->selected);
        else if (!m_list->ids.empty())
            playRow(0);
        refreshLCD();
    }

    void advance(int step, bool atEnd)
    {
        if (m_queue.empty()) {
            if (!atEnd)
                playPause();
            return;
        }
        if (atEnd && m_repeat == 2) {
            playTrack(m_queue[size_t(m_queuePos)]);
            return;
        }
        int next = m_queuePos + step;
        if (next >= int(m_queue.size()) || next < 0) {
            if (m_repeat == 1 || !atEnd)
                next = (next + int(m_queue.size())) % int(m_queue.size());
            else {
                m_player.stop();
                m_current = -1;
                markPlaying();
                m_list->body()->setNeedsDisplay();
                refreshLCD();
                return;
            }
        }
        m_queuePos = next;
        playTrack(m_queue[size_t(m_queuePos)]);
    }

    // Previous: the song's start when more than a few seconds in, else the song before
    void previous()
    {
        if (!m_player.isStopped() && m_player.position() > 3) {
            m_player.seek(0);
            return;
        }
        advance(-1, false);
    }

    void setVolume(double v)
    {
        m_volume->setValue(v);
        m_player.setVolume(m_volume->value);
        Settings("itunes").set("Volume", int(std::round(m_volume->value * 100)));
    }

    void setShuffle(bool on)
    {
        m_shuffle = on;
        m_shuffleButton->setRow(on ? 1 : 0);
        Settings("itunes").set("Shuffle", on);
    }

    void setRepeat(int r)
    {
        m_repeat = r;
        m_repeatButton->setRow(r);
        Settings("itunes").set("Repeat", r);
    }

    void refreshLCD()
    {
        const bool active = !m_player.isStopped() && m_library.track(m_current);
        m_play->setRow(m_player.isPlaying() ? 1 : 0);
        if (m_list->playingPaused != m_player.isPaused()) {
            m_list->playingPaused = m_player.isPaused();
            m_list->body()->setNeedsDisplay();
        }
        m_lcd->showing = active;
        if (active) {
            const Track &t = *m_library.track(m_current);
            m_lcd->setLines(t.name, t.artist.empty() ? t.album : t.artist);
            m_lcd->position = m_player.position();
            m_lcd->duration = m_player.duration() > 0 ? m_player.duration() : t.totalTime / 1000.0;
        }
        m_lcd->setNeedsDisplay();
    }

    // ---- editing

    void newPlaylist()
    {
        Playlist p;
        p.name = "untitled playlist";
        int n = 1;
        auto taken = [&](const std::string &name) {
            return std::any_of(m_library.playlists().begin(), m_library.playlists().end(), [&](const Playlist &q) { return q.name == name; });
        };
        while (taken(p.name))
            p.name = "untitled playlist " + std::to_string(++n);
        m_library.playlists().push_back(p);
        m_library.save();
        rebuildSources();
        m_sources->select(int(m_sources->rows.size()) - 1, true);
        layout();
    }

    void newPlaylistFromSelection()
    {
        if (m_list->selected < 0)
            return;
        newPlaylist();
        m_library.playlists().back().items.push_back(m_list->ids[size_t(m_list->selected)]);
        m_library.save();
        loadSource();
    }

    void deleteSelection()
    {
        const int sel = m_sources->selected();
        const int tag = sel >= 0 ? m_sources->rows[size_t(sel)].tag : -1;
        if (m_list->selected < 0)
            return;
        const int id = m_list->ids[size_t(m_list->selected)];
        if (tag >= 0) {
            auto &items = m_library.playlists()[size_t(tag)].items;
            items.erase(std::remove(items.begin(), items.end(), id), items.end());
        } else {
            if (id == m_current) {
                m_player.stop();
                m_current = -1;
            }
            m_library.remove(id);
        }
        m_library.save();
        loadSource();
    }

    void showInFinder()
    {
        if (m_list->selected < 0)
            return;
        if (const Track *t = m_library.track(m_list->ids[size_t(m_list->selected)]))
            Workspace::openFile(dirName(t->path));
    }

    void showCurrentSong()
    {
        if (m_current < 0)
            return;
        m_sources->select(1, true);
        for (size_t i = 0; i < m_list->ids.size(); ++i)
            if (m_list->ids[i] == m_current) {
                m_list->selected = int(i);
                m_list->scrollToRow(int(i));
            }
        m_list->body()->setNeedsDisplay();
    }

    void addToLibrary()
    {
        OpenPanel::run(tr("kMenuItemAddToLib"), Library::musicFolder(),
                       {{"Audio files", {"*.mp3", "*.m4a", "*.aac", "*.aif", "*.aiff", "*.wav", "*.flac", "*.ogg", "*.oga", "*.opus"}}}, true,
                       [this](std::vector<std::string> files) { addFiles(files); });
    }

    void showAbout()
    {
        if (!m_about) {
            m_about = std::make_unique<Window>(Size{300, 200}, "");
            m_about->setStyleMask(2);
            View *c = m_about->contentView();
            auto *icon = c->add<ImageView>();
            icon->image = Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/iTunes.icns"), {64, 64});
            icon->setFrame({118, 16, 64, 64});
            auto *name = c->add<Label>("iTunes", Font::boldSystem(14));
            name->style.align = Align::Center;
            name->setFrame({0, 92, 300, 18});
            auto *version = c->add<Label>("Version 8.2.1", Font::system(10));
            version->style.align = Align::Center;
            version->setFrame({0, 116, 300, 14});
            auto *copyright = c->add<Label>("Copyright © 2000–2009 Apple Inc.\nAll rights reserved.", Font::system(10));
            copyright->style.align = Align::Center;
            copyright->setFrame({0, 148, 300, 30});
            m_about->onClose = [this] { App::shared().later([this] { m_about.reset(); }); };
        }
        m_about->show();
        m_about->orderFront();
    }

    // ---- the menus: iTunes' xMNU, their commands

    void buildMenus()
    {
        m_menu.menus() = menuBar();
        // Controls ▸ Play: the space bar (the menu shows "space" in its text)
        std::function<void(std::vector<MainMenuItem> &)> fix = [&](std::vector<MainMenuItem> &items) {
            for (MainMenuItem &it : items) {
                if (it.action == "plps" && it.key.empty()) {
                    it.key = " ";
                    it.modifiers = 0;
                }
                fix(it.items);
            }
        };
        fix(m_menu.menus());
        m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) { return validate(it, v); };
        m_menu.perform = [this](const MainMenuItem &it) { perform(it); };
        m_menu.publish();
    }

    bool validate(const MainMenuItem &it, MenuValidation &v)
    {
        const std::string &a = it.action;
        const bool hasSel = m_list->selected >= 0;
        if (a == "plps") {
            v.title = tr(m_player.isPlaying() ? "kMenuItemPause" : "kMenuItemPlay");
            if (v.title.rfind("kMenuItem", 0) == 0)
                v.title = m_player.isPlaying() ? "Pause" : "Play";
            const size_t gap = v.title.find("  ");
            if (gap != std::string::npos)
                v.title = v.title.substr(0, gap);
            return !m_list->ids.empty() || !m_player.isStopped();
        }
        if (a == "ntrk" || a == "back")
            return !m_queue.empty();
        if (a == "volu" || a == "vold" || a == "mute" || a == "TFnd" || a == "add " || a == "nply" || a == "minw" ||
            a == "zoom" || a == "brwn" || a == "batf" || a == "abou" || a == "terminate:" || a == "hide:" ||
            a == "hideOtherApplications:" || a == "unhideAllApplications:" || a == "clos")
            return true;
        if (a == "npls" || a == "revl" || a == "clea" || a == "plps")
            return hasSel;
        if (a == "SCur")
            return m_current >= 0;
        if (a == "shuf") {
            v.title = m_shuffle ? "Turn Off Shuffle" : tr("kMenuItemShuffleOn");
            return true;
        }
        if (a == "rpt0" || a == "rpt1" || a == "rpt2") {
            v.checked = (a == "rpt0" && m_repeat == 0) || (a == "rpt2" && m_repeat == 1) || (a == "rpt1" && m_repeat == 2);
            return true;
        }
        if (a == "lisv") {
            v.checked = true;
            return true;
        }
        return false;
    }

    void perform(const MainMenuItem &it)
    {
        const std::string &a = it.action;
        if (a == "plps")
            playPause();
        else if (a == "ntrk")
            advance(1, false);
        else if (a == "back")
            previous();
        else if (a == "volu")
            setVolume(m_volume->value + 0.1);
        else if (a == "vold")
            setVolume(m_volume->value - 0.1);
        else if (a == "mute") {
            if (m_volume->value > 0) {
                m_muted = m_volume->value;
                setVolume(0);
            } else
                setVolume(m_muted > 0 ? m_muted : 1);
        } else if (a == "TFnd")
            m_window->makeFirstResponder(m_search);
        else if (a == "add ")
            addToLibrary();
        else if (a == "nply")
            newPlaylist();
        else if (a == "npls")
            newPlaylistFromSelection();
        else if (a == "clea")
            deleteSelection();
        else if (a == "revl")
            showInFinder();
        else if (a == "SCur")
            showCurrentSong();
        else if (a == "shuf")
            setShuffle(!m_shuffle);
        else if (a == "rpt0")
            setRepeat(0);
        else if (a == "rpt2")
            setRepeat(1);
        else if (a == "rpt1")
            setRepeat(2);
        else if (a == "minw")
            m_window->miniaturize();
        else if (a == "brwn" || a == "batf")
            m_window->orderFront();
        else if (a == "clos")
            m_window->miniaturize();
        else if (a == "abou")
            showAbout();
        else if (a == "terminate:")
            App::shared().quit();
    }

    Library m_library;
    Player m_player;
    MainMenu m_menu;
    std::unique_ptr<Window> m_window, m_about;
    TopBand *m_top = nullptr;
    FrameButtons *m_frameButtons = nullptr;
    BottomBand *m_bottom = nullptr;
    PictureButton *m_prev = nullptr, *m_play = nullptr, *m_next = nullptr, *m_speakerLow = nullptr, *m_speakerHigh = nullptr;
    PictureButton *m_views[3] = {};
    PictureButton *m_newPlaylist = nullptr, *m_shuffleButton = nullptr, *m_repeatButton = nullptr, *m_artwork = nullptr,
                  *m_geniusSidebar = nullptr, *m_genius = nullptr;
    VolumeSlider *m_volume = nullptr;
    LCD *m_lcd = nullptr;
    SearchField *m_search = nullptr;
    ScrollView *m_sourceScroll = nullptr;
    SourceList *m_sources = nullptr;
    View *m_divider = nullptr;
    TrackList *m_list = nullptr;
    double m_sourceWidth = 200;   // ACfg's Default Source Width
    std::vector<int> m_queue;
    int m_queuePos = 0, m_current = -1;
    bool m_shuffle = false;
    int m_repeat = 0;   // 0 off, 1 all, 2 one (432's rows)
    double m_muted = 0;
    int m_timer = 0;
};

} // namespace

int main(int argc, char **argv)
{
    App &app = App::shared();
    setProgramName("iTunes");
    if (!app.open()) {
        std::fprintf(stderr, "iTunes: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    if (!resourcesPresent()) {
        std::fprintf(stderr, "iTunes: its files are not in the assets (tools/extract.py takes them from the DVD)\n");
        return 1;
    }
    std::vector<std::string> files(argv + 1, argv + argc);
    if (Instance::handToRunning("iTunes", files))
        return 0;
    ITunesApp itunes;
    itunes.addFiles(files);
    Instance::listen("iTunes", [&itunes](std::vector<std::string> f) {
        itunes.addFiles(f);
        itunes.comeForward();
    });
    return app.run();
}
