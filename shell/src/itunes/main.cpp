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
// iTunes 11.4 (Software Update's, once the system is 10.6.8) when it is the installed one: its window
// as BrowserWindowFrameView and BWHeaderController make it (docs/itunes-reverse.md, "iTunes 11.4"):
//   no title bar: one top part, picture 5000 (61 high, its two columns active and inactive, 4-point
//   ends) under the stoplights 13004–13006 (at 7, 5, 7 apart); the transport 317, 321, 319 at (60, 16),
//   13 apart; the volume (VolumeControlView: the speaker 328, the track 350 and thumb 352, the loud
//   speaker 4 after) 10 after the transport, 68 to 110 wide, ending 8 before the LCD's left less 5;
//   the LCD 40% of the width, centred (picture 150, the Apple 151 when stopped; LCDPlayerView's lines
//   and its scrubber 160 / 161); the search field 175 wide, its right 36 from the window's; the
//   status bar, picture 5001 (24 high), the totals in TextStyles' 301.
#include "appkit.h"
#include "controls.h"
#include "foundation.h"
#include "mainmenu.h"
#include "panels.h"
#include "text.h"

#include "library.h"
#include "player.h"
#include "resources.h"
#include "store.h"

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

// 11.4: a menu item's text by its number, its keys ("###M#option") and hand-made column dropped
std::string menuText(uint32_t number)
{
    std::string t = text(number);
    t = t.substr(0, t.find("###"));
    return t.substr(0, t.find("  "));
}

constexpr double TitleBarHeight = 22, TopBandHeight = 54, BottomBandHeight = 29;
// 11.4: the window's top part (5000) and its status bar (5001), no title bar
constexpr double TopHeight11 = 61, BottomHeight11 = 24;

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
        const Size thumb = cellSize(352);
        const double x = std::round(value * (frame().w - thumb.w));
        const int state = m_dragging ? 2 : 1;
        if (eleven()) {
            // 11.4: the track's two rows (21 high, 10-point ends), the lower one the side up to the
            // thumb; the thumb 21 × 21, the three cells of kind 3
            const double y = std::floor((frame().h - thumb.h) / 2);
            drawCell(cr, 350, 0, 0, Rect{0, y, frame().w, thumb.h});
            drawCell(cr, 350, 0, 1, Rect{0, y, std::max(20.0, x + thumb.w / 2), thumb.h});
            drawCell(cr, 352, stateColumn(3, state), 0, Rect{x, y, thumb.w, thumb.h});
            return;
        }
        drawCell(cr, 350, 0, 0, Rect{0, 0, frame().w, frame().h});
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

// ---- the LCD (BrowserLCD, LCDPlayerView; docs/itunes-reverse.md section 5)
//   LCDPlayerView's layout (FUN_0004ee6a), in the LCD's W × 46: the time's width t is "-00:00:00"'s in
//   the LCD style (Txtr 302); the scrubber (ITScrubberView, PSLD 512) at (t + 2, 46 − 15), W − 2(t + 2)
//   wide, 14 high, past 240 inset by a quarter of the excess on each side; the time played left of it,
//   right-justified, t wide, a point up; the time left (or the whole time: the `stim` preference, a
//   click on it switching) right of it, left-justified. The first line (title) 2 down,
//   centred in W − 10 from 5; the second (artist, album in turn every 10 s: BrowserLCDPlayer's kinds
//   1 to 5, the empty ones skipped) at (37, 16), W − 74 wide.
//   LCDStaticTextView: centred when it fits; else scrolling left a point every 1/15 s after 1.5 s, drawn
//   twice 10 points apart, pausing 1.5 s at each turn; a new text slides up over the old, a point a
//   frame, the old going out at the top.

class LCD : public View {
public:
    bool showing = false;                // a song: else the Apple
    double position = 0, duration = 0;   // seconds
    bool showTotal = false;              // `stim` 2: the whole time on the right, not the time left
    std::function<void(double)> onSeek;
    std::function<void()> onToggleTime;

    LCD()
    {
        m_timer = App::shared().addTimer(1.0 / 15, true, [this] { tick(); });
    }
    ~LCD() override { App::shared().removeTimer(m_timer); }

    // A song's fields: a new title restarts the lines; the second line goes to the first field that
    // changed (FUN_0004d90c), else stays where it is
    void setTrack(const std::string &name, const std::string &artist, const std::string &album)
    {
        if (name == m_name && artist == m_fields[1] && album == m_fields[2])
            return;
        const bool artistChanged = artist != m_fields[1], albumChanged = album != m_fields[2];
        m_name = name;
        m_fields[1] = artist;
        m_fields[2] = album;
        m_lines[0].set(name, false);
        int kind = m_kind;
        if (artistChanged && !artist.empty())
            kind = 1;
        else if (albumChanged && !album.empty())
            kind = 2;
        m_kind = nextKind(kind, false);
        m_lines[1].set(m_fields[m_kind], false);
        m_cycle = 0;
        setNeedsDisplay();
    }

    Rect scrubberRect() const
    {
        if (eleven()) {
            // 11.4 (LCDPlayerView, FUN_002b1dae): in the picture's box (2 to 56), the scrubber 2 above
            // its bottom between the times, past 240 inset by a tenth of the excess on each side
            const double t = timeWidth(), w = frame().w;
            Rect r{4 + t + 2, 56 - 2 - 21, w - 8 - 2 * (t + 2), 21};
            if (r.w > 240) {
                const double inset = std::floor((r.w - 240) / 10);
                r.x += inset;
                r.w -= 2 * inset;
            }
            return r;
        }
        const double t = timeWidth(), w = frame().w;
        Rect r{t + 2, frame().h - 15, w - 2 * (t + 2), 14};
        if (r.w > 240) {
            const double inset = (r.w - 240) * 0.25;
            r.x += inset;
            r.w -= 2 * inset;
        }
        return r;
    }

    void draw(cairo_t *cr) override
    {
        if (eleven()) {
            draw11(cr);
            return;
        }
        // the background: picture 153, its second column when inactive (10.5 and later)
        drawCell(cr, 153, windowIsKey() ? 0 : 1, 0, bounds());
        if (!showing) {
            const Image apple = picture(154);
            apple.draw(cr, Point{std::floor((frame().w - apple.width()) / 2), std::floor((frame().h - apple.height()) / 2) + 1});
            return;
        }
        const iTunes::TextStyle lcd = textStyle(302);
        const double W = frame().w, lineH = std::ceil(lineHeight(lcd.font));
        m_lines[0].draw(cr, Rect{5, 2, W - 10, lineH}, lcd);
        m_lines[1].draw(cr, Rect{37, 16, std::max(0.0, W - 74), lineH}, lcd);

        // the scrubber: the track (PSLD 512's slider rect, 21 × 10, its 7-point ends kept) across, the
        // thumb (8 × 10 at 21) on the time played, within the drag rect (2 in from each end)
        const Rect bar = scrubberRect();
        const Image art = slider(512);
        if (!art.isNull()) {
            // QuickDraw's CopyMask: whole pixels, the middle repeated, never blended into the ends
            const auto part = [&](const Rect &src, const Rect &dst) {
                if (dst.w <= 0)
                    return;
                cairo_save(cr);
                cairo_rectangle(cr, dst.x, dst.y, dst.w, dst.h);
                cairo_clip(cr);
                cairo_translate(cr, dst.x, dst.y);
                cairo_scale(cr, dst.w / src.w, dst.h / src.h);
                cairo_set_source_surface(cr, art.surface(), -src.x, -src.y);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                cairo_paint(cr);
                cairo_restore(cr);
            };
            const double y = bar.y;
            part(Rect{0, 0, 7, 10}, Rect{bar.x, y, 7, 10});
            part(Rect{7, 0, 7, 10}, Rect{bar.x + 7, y, std::max(0.0, bar.w - 14), 10});
            part(Rect{14, 0, 7, 10}, Rect{bar.right() - 7, y, 7, 10});
            if (duration > 0) {
                const double f = std::clamp(position / duration, 0.0, 1.0);
                const double x = std::round(bar.x + 2 + f * (bar.w - 4 - 8));
                part(Rect{21, 0, 8, 10}, Rect{x, y, 8, 10});
            }
        }
        // the times (FUN_0004eaf6, whole seconds)
        AK::TextStyle t;
        t.font = lcd.font;
        t.color = lcd.color;
        t.elide = false;
        const double tw = timeWidth();
        const int played = int(position), whole = int(duration);
        t.align = Align::Right;
        drawText(cr, formatTime(played * 1000), Rect{bar.x - tw - 2, bar.y - 1, tw, 14}, t);
        if (duration > 0) {
            t.align = Align::Left;
            const std::string right = showTotal ? formatTime(whole * 1000) : "-" + formatTime(std::max(0, whole - played) * 1000);
            drawText(cr, right, Rect{bar.right() + 2, bar.y - 1, tw, 14}, t);
        }
    }

    // 11.4's LCD: picture 150 stretched (its 4-point ends kept), the Apple 151 centred when stopped
    // (LCDBackgroundView, FUN_00022168); the title, then the artist and the album in turn, centred in
    // TextStyles' 302; the scrubber's track 160 (its first row, 10-point ends) and thumb 161 (10 × 13)
    void draw11(cairo_t *cr)
    {
        drawCell(cr, 150, 0, 0, bounds());
        if (!showing) {
            const Image apple = picture(151);
            apple.draw(cr, Point{std::floor((frame().w - apple.width()) / 2), std::floor((frame().h - apple.height()) / 2)});
            return;
        }
        const iTunes::TextStyle lcd = textStyle(302);
        const double W = frame().w, lineH = std::ceil(lineHeight(lcd.font));
        const Rect bar = scrubberRect();
        m_lines[0].draw(cr, Rect{9, 6, W - 18, lineH}, lcd);
        m_lines[1].draw(cr, Rect{9, bar.y - lineH + 4, W - 18, lineH}, lcd);
        drawCell(cr, 160, 0, 0, bar);
        if (duration > 0) {
            const Size thumb = cellSize(161);
            const double f = std::clamp(position / duration, 0.0, 1.0);
            drawCell(cr, 161, stateColumn(1, m_seeking ? 2 : 1), 0,
                     Rect{std::round(bar.x + f * (bar.w - thumb.w)), bar.y + std::floor((bar.h - thumb.h) / 2), thumb.w, thumb.h});
        }
        AK::TextStyle t;
        t.font = lcd.font;
        t.color = Color::gray(0x3c);
        t.elide = false;
        const double tw = timeWidth(), ty = bar.y + std::floor((bar.h - 14) / 2);
        const int played = int(position), whole = int(duration);
        t.align = Align::Right;
        drawText(cr, formatTime(played * 1000), Rect{bar.x - tw - 2, ty, tw, 14}, t);
        if (duration > 0) {
            t.align = Align::Left;
            const std::string right = showTotal ? formatTime(whole * 1000) : "-" + formatTime(std::max(0, whole - played) * 1000);
            drawText(cr, right, Rect{bar.right() + 2, ty, tw, 14}, t);
        }
    }

    void mouseDown(Event &e) override
    {
        if (!showing)
            return;
        const Rect bar = scrubberRect();
        if (duration > 0 && Rect{bar.right() + 2, bar.y - 1, timeWidth(), std::max(14.0, bar.h)}.contains(e.location)) {
            // the time on the right: the time left and the whole time in turn (FUN_0013658a)
            showTotal = !showTotal;
            if (onToggleTime)
                onToggleTime();
            setNeedsDisplay();
            return;
        }
        m_seeking = duration > 0 && bar.contains(e.location);
        if (m_seeking)
            seekAt(e.location.x);
    }
    void mouseDragged(Event &e) override
    {
        if (m_seeking)
            seekAt(e.location.x);
    }
    void mouseUp(Event &) override { m_seeking = false; }

private:
    // A LCDStaticTextView's state
    struct Line {
        std::string text, old;
        double width = 0, room = 0, oldScroll = 0;
        int scroll = 0, pause = 0, slide = -1;   // slide: the new text's rise, −1 when still
        void set(const std::string &t, bool animate)
        {
            if (t == text)
                return;
            if (animate) {
                old = text;
                oldScroll = scroll;
                slide = 0;
            } else {
                slide = -1;
            }
            text = t;
            scroll = 0;
            pause = 0;
        }
        bool scrolls() const { return width > room; }
        void drawText(cairo_t *cr, const std::string &s, double x, double y, double h, double offset, const iTunes::TextStyle &style)
        {
            AK::TextStyle t;
            t.font = style.font;
            t.color = style.color;
            t.elide = false;
            const double w = textWidth(s, style.font);
            if (w <= room) {
                t.align = Align::Center;
                AK::drawText(cr, s, Rect{x, y, room, h}, t);
                return;
            }
            t.align = Align::Left;
            AK::drawText(cr, s, Rect{x - offset, y, w + 2, h}, t);
            AK::drawText(cr, s, Rect{x - offset + w + 10, y, w + 2, h}, t);
        }
        void draw(cairo_t *cr, const Rect &r, const iTunes::TextStyle &style)
        {
            width = textWidth(text, style.font);
            room = r.w;
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_clip(cr);
            double y = r.y;
            if (slide >= 0) {
                drawText(cr, old, r.x, y - slide, r.h, oldScroll, style);
                y = y - slide + r.h;
            }
            drawText(cr, text, r.x, y, r.h, scroll, style);
            cairo_restore(cr);
        }
        // a frame (FUN_00051e3a): the slide, else the scroll after its pause
        bool tick(double height)
        {
            if (slide >= 0) {
                if (++slide > height)
                    slide = -1;
                return true;
            }
            if (!scrolls())
                return false;
            if (pause < 22) {   // 1.5 s
                ++pause;
                return false;
            }
            if (++scroll >= width + 10) {
                scroll = 0;
                pause = 0;
            }
            return true;
        }
    };

    double timeWidth() const { return std::ceil(textWidth("-00:00:00", textStyle(302).font)); }

    void seekAt(double x)
    {
        const Rect bar = scrubberRect();
        const double f = std::clamp((x - bar.x - 2 - 4) / std::max(1.0, bar.w - 4 - 8), 0.0, 1.0);
        if (onSeek)
            onSeek(f * duration);
    }

    // the second line's next kind from `from`, the empty ones skipped (kinds 1 artist, 2 album; 3 to 5,
    // a stream's, never here)
    int nextKind(int from, bool advance) const
    {
        const int start = (from < 1 || from > 2) ? 1 : from;
        for (int i = advance ? 1 : 0; i < (advance ? 3 : 2); ++i) {
            const int k = (start - 1 + i) % 2 + 1;
            if (!m_fields[k].empty())
                return k;
        }
        return start;
    }

    void tick()
    {
        if (!showing)
            return;
        const double lineH = std::ceil(lineHeight(textStyle(302).font));
        bool moved = false;
        for (Line &l : m_lines)
            moved |= l.tick(lineH);
        // every 10 s the second line goes to the next field (FUN_00051dfe), sliding
        if (++m_cycle >= 150) {
            m_cycle = 0;
            const int k = nextKind(m_kind, true);
            if (k != m_kind) {
                m_kind = k;
                m_lines[1].set(m_fields[k], true);
                moved = true;
            }
        }
        if (moved)
            setNeedsDisplay();
    }

    int m_timer = 0, m_cycle = 0, m_kind = 1;
    bool m_seeking = false;
    std::string m_name, m_fields[3];
    Line m_lines[2];
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
    TopBand() { m_titleBar = eleven() ? TopHeight11 : TitleBarHeight; }
    std::string title;
    void draw(cairo_t *cr) override
    {
        if (eleven()) {
            // 11.4: BrowserWindowFrameView's top part, picture 5000 (its column 1 when inactive:
            // _BrowserWindowFrameViewBodyPartView, FUN_0001d572), no title
            drawCell(cr, 5000, windowIsKey() ? 0 : 1, 0, bounds());
            return;
        }
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
        if (eleven()) {
            // 11.4's status bar: picture 5001, the totals in TextStyles' 301 (Helvetica Neue 11, centred)
            drawCell(cr, 5001, key ? 0 : 1, 0, bounds());
            const iTunes::TextStyle s = textStyle(301);
            AK::TextStyle t;
            t.font = s.font;
            t.color = key ? Color::gray(0x3c) : Color::gray(0x80);
            t.align = Align::Center;
            drawText(cr, totals, Rect{100, 4, std::max(0.0, frame().w - 200), 17}, t);
            return;
        }
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

// ---- 11.4's nav bar (BWNavBarController; docs/itunes-reverse.md, section 8)
//   29 points (picture 870, its light theme, under the top part), a line of 887 under it. The library's
//   (BWLibraryNavBarController, FUN_00100874): the media kind's picture 8251 at the left (24 × 24, its
//   rows Music, Movies, TV Shows, Podcasts, Books, Apps, iTunes U, Tones…), the views as buttons
//   (BWNavBarRadioButtonDataSource; their names STR# 163: Songs 0x63, Albums 0x64, Artists 0x65,
//   Genres 0x66, Playlists 0x68, Radio 0x6a) 3 apart in the middle, on 871 (11-point ends) or 869 when
//   chosen, their text in TextStyles' 310 (FUN_0045b6b2); "iTunes Store" (163.054) on 871 at the right
//   (FUN_00103c9e). The Store's (BWStoreNavBarController): back 882 and forward 883, home 879, the
//   store's menus as its pages give them (addProtocol's navbar ▸ menus) on the pills 876, text 311;
//   "Library" (163.092) at the right.
class NavBar : public View {
public:
    bool store = false;
    std::vector<std::string> views;   // the library's
    std::vector<bool> viewEnabled;
    int selectedView = 0;
    std::vector<StoreMenu> menus;     // the store's
    std::string selectedMenu;
    bool canBack = false, canForward = false;
    std::string storeTitle, libraryTitle;
    std::function<void(int)> onView;
    std::function<void()> onStore, onLibrary, onBack, onForward, onHome;
    std::function<void(const std::string &)> onMenu;

    static constexpr double Height = 29, ButtonH = 23, Margin = 8, Gap = 3, Pad = 12;

    void draw(cairo_t *cr) override
    {
        drawCell(cr, 870, 0, 0, Rect{0, 0, frame().w, Height});
        layoutItems();
        const double by = std::floor((Height - ButtonH) / 2);
        const iTunes::TextStyle light = textStyle(310), dark = textStyle(311);
        if (!store) {
            drawCell(cr, 8251, 0, 0, Rect{Margin, std::floor((Height - 24) / 2), 24, 24});
            for (size_t i = 0; i < m_items.size(); ++i) {
                const bool on = int(i) == selectedView;
                const bool enabled = i < viewEnabled.size() && viewEnabled[i];
                const int state = m_pressed == int(i) ? 2 : enabled ? 1 : 0;
                drawCell(cr, on ? 869 : 871, stateColumn(3, on && state == 0 ? 1 : state), 0, m_items[i]);
                label(cr, views[i], m_items[i], light, on ? Color::gray(255) : enabled ? Color::gray(0x40) : Color::gray(0x40, 0.4));
            }
            drawCell(cr, 871, stateColumn(3, m_pressed == 1000 ? 2 : 1), 0, m_right);
            label(cr, storeTitle, m_right, light, Color::gray(0x40));
            return;
        }
        drawCell(cr, 882, stateColumn(3, !canBack ? 0 : m_pressed == 2000 ? 2 : 1), 0, m_back);
        drawCell(cr, 883, stateColumn(3, !canForward ? 0 : m_pressed == 2001 ? 2 : 1), 0, m_forward);
        const Image home = picture(879);
        home.draw(cr, Point{m_home.x + std::floor((m_home.w - home.width()) / 2), std::floor((Height - home.height()) / 2)},
                  m_pressed == 2002 ? 0.6 : 1);
        for (size_t i = 0; i < m_items.size(); ++i) {
            // a menu with items: the pill with its arrow (886, 4 columns of kind 4, 14-point right end)
            const bool on = menus[i].title == selectedMenu, drop = !menus[i].items.empty();
            const int col = on || m_pressed == int(i) ? (drop ? 3 : 2) : 1;
            drawCell(cr, drop ? 886 : 876, col, 1, m_items[i]);
            Rect text = m_items[i];
            if (drop)
                text.w -= 14 - Pad / 2;
            label(cr, menus[i].title, text, dark, on ? Color::gray(255) : Color::gray(0x40));
        }
        drawCell(cr, 871, stateColumn(3, m_pressed == 1000 ? 2 : 1), 0, m_right);
        label(cr, libraryTitle, m_right, light, Color::gray(0x40));
        (void)by;
    }

    void mouseDown(Event &e) override
    {
        m_pressed = hit(e.location);
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        const int p = m_pressed;
        m_pressed = -1;
        setNeedsDisplay();
        if (p < 0 || hit(e.location) != p)
            return;
        if (p == 1000) {
            if (store ? bool(onLibrary) : bool(onStore))
                store ? onLibrary() : onStore();
        } else if (p == 2000 && canBack && onBack) {
            onBack();
        } else if (p == 2001 && canForward && onForward) {
            onForward();
        } else if (p == 2002 && onHome) {
            onHome();
        } else if (store && size_t(p) < menus.size() && onMenu) {
            const StoreMenu &m = menus[size_t(p)];
            const Rect r = m_items[size_t(p)];
            if (!m.items.empty() && e.location.x >= r.right() - 18) {
                // its drop-down: the items, the dividers as lines
                std::vector<MenuItem> items;
                for (const StoreMenu &i : m.items)
                    items.push_back(i.divider ? MenuItem::line() : MenuItem(i.title));
                const std::vector<StoreMenu> list = m.items;
                Menu::popUp(window(), toWindow(Point{r.x, r.bottom()}), items, r.w, Font::system(13), [this, list](int c) {
                    if (c >= 0 && size_t(c) < list.size() && !list[size_t(c)].url.empty() && onMenu)
                        onMenu(list[size_t(c)].url);
                });
            } else {
                onMenu(m.url);
            }
        } else if (!store && size_t(p) < viewEnabled.size() && viewEnabled[size_t(p)] && onView) {
            selectedView = p;
            onView(p);
        }
    }

private:
    static void label(cairo_t *cr, const std::string &t, const Rect &r, const iTunes::TextStyle &s, const Color &c)
    {
        AK::TextStyle ts;
        ts.font = s.font;
        ts.color = c;
        ts.align = Align::Center;
        drawText(cr, t, Rect{r.x, r.y, r.w, r.h - 1}, ts);
    }
    double buttonWidth(const std::string &t, const iTunes::TextStyle &s) const { return std::ceil(textWidth(t, s.font)) + 2 * Pad; }

    void layoutItems()
    {
        const double W = frame().w, by = std::floor((Height - ButtonH) / 2);
        const iTunes::TextStyle light = textStyle(310), dark = textStyle(311);
        m_items.clear();
        const double rw = buttonWidth(store ? libraryTitle : storeTitle, light);
        m_right = Rect{W - Margin - rw, by, rw, ButtonH};
        std::vector<double> widths;
        double total = 0;
        const size_t n = store ? menus.size() : views.size();
        for (size_t i = 0; i < n; ++i) {
            widths.push_back(buttonWidth(store ? menus[i].title : views[i], store ? dark : light) +
                             (store && !menus[i].items.empty() ? 14 : 0));
            total += widths.back() + (i ? Gap : 0);
        }
        double x = std::floor((W - total) / 2);
        for (double w : widths) {
            m_items.push_back(Rect{x, by, w, ButtonH});
            x += w + Gap;
        }
        const Size back = cellSize(882), fwd = cellSize(883);
        m_back = Rect{Margin, by, back.w, back.h};
        m_forward = Rect{m_back.right(), by, fwd.w, fwd.h};
        m_home = Rect{m_forward.right() + Margin, 0, 24, Height};
    }

    int hit(Point p)
    {
        layoutItems();
        if (m_right.contains(p))
            return 1000;
        if (store) {
            if (m_back.contains(p))
                return 2000;
            if (m_forward.contains(p))
                return 2001;
            if (m_home.contains(p))
                return 2002;
        }
        for (size_t i = 0; i < m_items.size(); ++i)
            if (m_items[i].contains(p))
                return int(i);
        return -1;
    }

    std::vector<Rect> m_items;
    Rect m_right, m_back, m_forward, m_home;
    int m_pressed = -1;
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
            if (eleven()) {
                // 11.4's header (ITListHeaderView): flat; its own pictures (9137–9140) not traced yet
                fill(cr, Rect{x, 0, w, HeaderHeight}, Color::gray(m_pressed == int(c) ? 230 : 250));
                fill(cr, Rect{x + w - 1, 3, 1, HeaderHeight - 6}, Color::gray(217));
                fill(cr, Rect{x, HeaderHeight - 1, w, 1}, Color::gray(199));
            } else {
                drawCell(cr, 304, stateColumn(pictureInfo(304).kind, m_pressed == int(c) ? 2 : state), row, Rect{x, 0, w, HeaderHeight});
            }
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
                            if (r == list->playing && eleven())   // 11.4: the volume's speaker (its status icons not traced)
                                drawCell(cr, 328, 1, 2, Rect{x + std::floor((col.width - 14) / 2), y + std::floor((h - 14) / 2), 14, 14});
                            else if (r == list->playing)
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
        m_window = std::make_unique<Window>(Size{1000, 620 + (eleven() ? 0 : TitleBarHeight)}, "iTunes");
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
        // 11.4's transport: TransportControlView's buttons 317, 321, 319 (FUN_001d2e24)
        m_prev = m_top->add<PictureButton>(eleven() ? 317 : 316);
        m_play = m_top->add<PictureButton>(eleven() ? 321 : 320);
        m_next = m_top->add<PictureButton>(eleven() ? 319 : 318);
        m_prev->onClick = [this] { previous(); };
        m_play->onClick = [this] { playPause(); };
        m_next->onClick = [this] { advance(1, false); };
        m_speakerLow = m_top->add<PictureButton>(328, 0);
        m_speakerHigh = m_top->add<PictureButton>(328, eleven() ? 3 : 2);   // 11.4: four rows, quiet to loud
        m_speakerLow->onClick = [this] { setVolume(0); };
        m_speakerHigh->onClick = [this] { setVolume(1); };
        m_volume = m_top->add<VolumeSlider>();
        m_volume->onChange = [this](double v) { m_player.setVolume(v); };
        m_volume->setValue(Settings("itunes").integer("Volume", 100) / 100.0);
        m_player.setVolume(m_volume->value);
        m_lcd = m_top->add<LCD>();
        m_lcd->showTotal = Settings("itunes").integer("stim", 1) == 2;
        m_lcd->onToggleTime = [this] { Settings("itunes").set("stim", m_lcd->showTotal ? 2 : 1); };
        m_lcd->onSeek = [this](double s) {
            m_player.seek(s);
            refreshLCD();
        };
        for (int i = 0; i < 3; ++i) {
            m_views[i] = m_top->add<PictureButton>(540 + i, i == 0 ? 1 : 0);
            m_views[i]->setEnabled(i == 0);   // the list alone: no grid, no Cover Flow
            m_views[i]->setHidden(eleven());  // 11.4: no view buttons in the top part
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

        if (eleven())
            buildNavAndStore(c);

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
        if (eleven())   // 11.4's status bar has the totals alone (shuffle and repeat: the Controls menu)
            for (PictureButton *b : {m_newPlaylist, m_shuffleButton, m_repeatButton, m_artwork, m_geniusSidebar, m_genius})
                b->setHidden(true);

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

    // 11.4: the nav bar and the Store (BWNavBarController, BWStoreController)
    void buildNavAndStore(View *c)
    {
        m_nav = c->add<NavBar>();
        // STR# 163: Songs, Albums, Artists, Genres, Playlists, Radio; iTunes Store, Library
        for (const char *k : {"163.099", "163.100", "163.101", "163.102", "163.104", "163.106"})
            m_nav->views.push_back(tr(k));
        m_nav->viewEnabled = {true, false, false, false, false, false};   // the songs' list alone so far
        m_nav->storeTitle = tr("163.084");
        m_nav->libraryTitle = tr("163.092");
        m_nav->onView = [this](int) { setStoreMode(false); };
        m_nav->onStore = [this] { setStoreMode(true); };
        m_nav->onLibrary = [this] { setStoreMode(false); };
        if (!StoreView::available())
            m_nav->onStore = nullptr;
        m_store = c->add<StoreView>();
        m_store->setHidden(true);
        m_nav->onBack = [this] { m_store->command("back"); };
        m_nav->onForward = [this] { m_store->command("forward"); };
        m_nav->onHome = [this] { m_store->command("home"); };
        m_nav->onMenu = [this](const std::string &url) { m_store->load(url); };
        m_store->onNavigation = [this] {
            m_nav->canBack = m_store->canGoBack();
            m_nav->canForward = m_store->canGoForward();
            m_nav->setNeedsDisplay();
        };
        m_store->onMenus = [this](const std::vector<StoreMenu> &menus, const std::string &sel) {
            m_nav->menus = menus;
            m_nav->selectedMenu = sel;
            m_nav->setNeedsDisplay();
        };
        m_store->onLibrary = [this] { setStoreMode(false); };
    }

    // The library or the Store in the body (iTunes Store / Library in the nav bar, Store ▸ Home)
    void setStoreMode(bool on)
    {
        if (!m_nav)
            return;
        m_nav->store = on;
        m_store->setHidden(!on);
        for (View *v : {static_cast<View *>(m_sourceScroll), m_divider, static_cast<View *>(m_list)})
            v->setHidden(on);
        layout();
        if (on) {
            m_store->start();
            m_window->makeFirstResponder(m_store);
        } else {
            m_window->makeFirstResponder(m_list);
        }
        m_nav->setNeedsDisplay();
    }

    // BrowserWindow's layout pass (FUN_000249e4) and the top bar's places (FUN_00142cb2)
    // The zoom button: iTunes' mini player (the window down to its top band), not yet ported
    void zoom() {}

    void layout()
    {
        const Size s = m_window->contentView()->frame().w > 0 ? Size{m_window->contentView()->frame().w, m_window->contentView()->frame().h}
                                                              : m_window->size();
        const double W = s.w, H = s.h;
        if (eleven()) {
            layout11(W, H);
            return;
        }
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

    // 11.4's layout: BWHeaderController's (FUN_001ebd78), the stoplights' (ITWindowControlsView,
    // FUN_001e8e54), the volume's (VolumeControlView, FUN_001d4cc4); the body between the top part
    // and the status bar
    void layout11(double W, double H)
    {
        m_top->setFrame({0, 0, W, TopHeight11});
        // the stoplights: 14 × 16 each, 7 apart, at (7, 5)
        m_frameButtons->setFrame({7, 5, 56, 16});
        // the transport at (60, 16): Previous, Play 13 after it, Next 13 after Play; the others on
        // Play's middle
        const Size prev = cellSize(317), play = cellSize(321), next = cellSize(319);
        double tx = 60;
        const double mid = 16 + play.h / 2;
        // the LCD: 40% of the width, centred, the picture's height
        const double lcdW = std::floor(W * 0.4), lcdH = picture(150).height();
        const Rect lcd{std::floor((W - lcdW) / 2), 0, lcdW, lcdH};
        m_lcd->setFrame(lcd);
        // the volume: 10 after the transport, at most 110 wide, ending 8 + 5 before the LCD; under 68
        // the transport moves left to keep 68
        const double transportW = prev.w + 13 + play.w + 13 + next.w;
        double vx = tx + transportW + 10, vw = lcd.x - 5 - 8 - vx;
        if (vw > 110)
            vw = 110;
        else if (vw < 68) {
            const double short_ = 68 - vw;
            tx -= short_;
            vx -= short_;
            vw = 68;
        }
        m_prev->setFrame({tx, std::round(mid - prev.h / 2), prev.w, prev.h});
        m_play->setFrame({tx + prev.w + 13, 16, play.w, play.h});
        m_next->setFrame({tx + prev.w + 13 + play.w + 13, std::round(mid - next.h / 2), next.w, next.h});
        // VolumeControlView: the speaker, the slider up to the loud speaker less 4
        const Size speaker = cellSize(328);
        const double sy = std::round(mid - speaker.h / 2);
        m_speakerLow->setFrame({vx, sy, speaker.w, speaker.h});
        m_volume->setFrame({vx + speaker.w, sy, std::max(0.0, vw - 2 * speaker.w - 4), speaker.h});
        m_speakerHigh->setFrame({vx + vw - speaker.w, sy, speaker.w, speaker.h});
        // the search field: 175 wide, its right 36 from the window's right
        const double fieldH = 22;
        m_search->setFrame({W - 36 - 175, std::round(mid - fieldH / 2), 175, fieldH});

        double bodyTop = TopHeight11;
        if (m_nav) {
            m_nav->setFrame({0, TopHeight11, W, NavBar::Height});
            bodyTop += NavBar::Height;
        }
        const double bodyH = std::max(0.0, H - bodyTop - BottomHeight11);
        if (m_store)
            m_store->setFrame({0, bodyTop, W, bodyH});
        const double sourceW = std::clamp(m_sourceWidth, 150.0, std::max(150.0, W - 300 - 1));
        m_sourceScroll->setFrame({0, bodyTop, sourceW, bodyH});
        m_sources->setFrame({0, 0, sourceW - 15, std::max(bodyH, m_sources->contentHeight())});
        m_sourceScroll->setDocumentHeight(m_sources->frame().h);
        m_divider->setFrame({sourceW, bodyTop, 1, bodyH});
        m_list->setFrame({sourceW + 1, bodyTop, W - sourceW - 1, bodyH});
        m_bottom->setFrame({0, H - BottomHeight11, W, BottomHeight11});
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
        // 11.4 has them elsewhere in the same tables: 135.006, 135.008, 135.009, 9000.034
        const bool e = eleven();
        header(e ? "135.006" : "135.011");
        SourceRow music;
        music.title = tr(e ? "9000.034" : "9000.036");
        music.icon = 0;
        music.tag = -1;
        m_sources->rows.push_back(music);
        header(e ? "135.008" : "135.013");
        SourceRow store;
        store.title = tr("137.001");
        store.icon = 7;
        store.selectable = false;
        store.tag = -2;
        m_sources->rows.push_back(store);
        header(e ? "135.009" : "135.014");
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

    void stop()
    {
        m_player.stop();
        m_current = -1;
        markPlaying();
        m_list->body()->setNeedsDisplay();
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
            m_lcd->setTrack(t.name, t.artist, t.album);
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
        OpenPanel::run(eleven() ? menuText(0x75f8000c) : tr("kMenuItemAddToLib"), Library::musicFolder(),
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
            v.title = eleven() ? tr(m_player.isPlaying() ? "136.004" : "136.002") : tr(m_player.isPlaying() ? "kMenuItemPause" : "kMenuItemPlay");
            if (v.title.rfind("kMenuItem", 0) == 0)
                v.title = m_player.isPlaying() ? "Pause" : "Play";
            const size_t gap = v.title.find("  ");
            if (gap != std::string::npos)
                v.title = v.title.substr(0, gap);
            return !m_list->ids.empty() || !m_player.isStopped();
        }
        if (a == "ntrk" || a == "back" || a == "ptrk")
            return !m_queue.empty();
        if (m_store && (a == "Msgh" || a == "Msrp"))   // 11.4's Store ▸ Home, Reload Page
            return true;
        if (m_store && a == "Msgb")
            return m_nav->store && m_store->canGoBack();
        if (m_store && a == "Msgf")
            return m_nav->store && m_store->canGoForward();
        if (a == "stop")   // 11.4's Controls ▸ Stop
            return !m_player.isStopped();
        if (a == "volu" || a == "vold" || a == "mute" || a == "TFnd" || a == "add " || a == "nply" || a == "minw" ||
            a == "zoom" || a == "brwn" || a == "batf" || a == "abou" || a == "terminate:" || a == "hide:" ||
            a == "hideOtherApplications:" || a == "unhideAllApplications:" || a == "clos" || a == "CLOS")
            return true;
        if (a == "npls" || a == "revl" || a == "clea" || a == "plps")
            return hasSel;
        if (a == "SCur")
            return m_current >= 0;
        if (a == "shuf") {
            v.title = eleven() ? menuText(m_shuffle ? 0x76c0000c : 0x76c0000b) : m_shuffle ? "Turn Off Shuffle" : tr("kMenuItemShuffleOn");
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
        else if (a == "back" || a == "ptrk")
            previous();
        else if (a == "stop")
            stop();
        else if (m_store && (a == "Msgh" || a == "Msrp" || a == "Msgb" || a == "Msgf")) {
            if (!m_nav->store)
                setStoreMode(true);
            m_store->command(a == "Msgh" ? "home" : a == "Msrp" ? "reload" : a == "Msgb" ? "back" : "forward");
        }
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
        else if (a == "clos" || a == "CLOS")
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
    NavBar *m_nav = nullptr;
    StoreView *m_store = nullptr;
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
    double m_sourceWidth = eleven() ? 185 : 200;   // ACfg's Default Source Width (11.4's AppConfig.plist: 185)
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
