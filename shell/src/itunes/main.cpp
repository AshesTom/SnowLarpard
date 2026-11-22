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

#include "dialogs.h"
#include "equalizer.h"
#include "frame.h"
#include "getinfo.h"
#include "coverflow.h"
#include "grid.h"
#include "smartplaylist.h"
#include "viewoptions.h"
#include "visualizer.h"
#include "miniplayer11.h"
#include "upnext11.h"
#include "preferences.h"
#include "library.h"
#include "player.h"
#include "resources.h"
#include "store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <memory>
#include <random>
#include <sstream>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace AK;
using namespace iTunes;

namespace {

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
    Image artwork;   // the song's own (11.4 shows it)

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
            const double t = timeWidth(), off = artworkWidth(), w = frame().w - off;
            Rect r{off + 4 + t + 2, 56 - 2 - 21, w - 8 - 2 * (t + 2), 21};
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

    // FUN_002b1dae: with artwork, on an LCD wider than 360, the lines and the scrubber begin at its right
    // edge (12 + its side) instead of 12
    double artworkWidth() const { return eleven() && showing && !artwork.isNull() && frame().w > 360 ? frame().h - 16 : 0; }

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
        // a scrubber of no width (a narrow mini player: FUN_0004ee6a hides nothing, the view is empty)
        if (!art.isNull() && bar.w > 0) {
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
        const double off = artworkWidth(), W = frame().w - off, lineH = std::ceil(lineHeight(lcd.font));
        const Rect bar = scrubberRect();
        if (off > 0) {
            // LCDPlayerView's artwork (FUN_002b1dae): at (12, 8), a square as tall as the LCD less 16
            const double side = frame().h - 16;
            const Size fit = scaledToFit(artwork.size(), {side, side});
            artwork.draw(cr, Rect{12 + std::floor((side - fit.w) / 2), 8 + std::floor((side - fit.h) / 2), fit.w, fit.h});
        }
        m_lines[0].draw(cr, Rect{off + 9, 6, W - 18, lineH}, lcd);
        m_lines[1].draw(cr, Rect{off + 9, bar.y - lineH + 4, W - 18, lineH}, lcd);
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
//   in rollover while the pointer is over any of them: frame.h). Its gradient, its controls aside, moves the
//   window; a double click in the title bar puts it in the Dock.

class TopBand : public FrameBand {
public:
    TopBand() { m_titleBar = eleven() ? TopHeight11 : TitleBarHeight; }
    std::string title;
    bool mini = false;   // 8.2.1's mini player: the band is the whole window
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
        drawGradient(cr, bounds(), key ? 197 : 233, key ? 150 : 207, int(mini ? frame().h : TitleBarHeight + TopBandHeight), 0);
        if (!mini)   // the mini player's band is the window's bottom too
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
        if (r.w < 1)
            return;
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
//   11.4 (FUN_00e5675c fills the list's pictures and styles, FUN_00024686 draws the header): the header
//   picture 304 again (cells 40 x 23, 2 and 16 kept at the ends), the header at least 22 high (26 with
//   the large text); its column by
//   FUN_000200da's kind 3 (normal 1, pressed 2, inactive window 0), its row the column's sort indicator;
//   the title in 9004, 5 in from the left, 5 from the right (16 when sorted: the arrow's room). The song
//   playing: 8455 (20 x 20 cells, two columns: the second on a highlighted row), its row by the track's
//   status through the table at 0x137277c (FUN_00026da7): 3 playing row 0 (the speaker with its waves),
//   2 paused row 1.

struct Column {
    std::string title;
    double width;
    Align align;
};

// the list's text: Txtr 9002 List Contents (Small), 9003 (Large) by the General pane's List Text
int g_listText = 9002;
double listRowHeight() { return std::ceil(lineHeight(textStyle(g_listText).font)) + 2; }

class TrackList : public View {
public:
    std::vector<Column> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<int> ids;
    // The selection (FUN_002fc392, the List Manager's LClick as iTunes has it; the songs' list's flags 8,
    // lNoRect: FUN_001b65da): the rows chosen, and `selected` the last click (+0x48), the focus
    std::set<int> sel;
    int selected = -1, playing = -1;   // rows
    bool isSelected(int r) const { return sel.count(r) != 0; }
    void selectOnly(int r)
    {
        sel.clear();
        if (r >= 0)
            sel.insert(r);
        selected = r;
    }
    // Edit ▸ Select All (FUN_002fb238: every cell), Select None (FUN_002fba90)
    void selectAll()
    {
        for (int r = 0; r < int(rows.size()); ++r)
            sel.insert(r);
        if (selected < 0 && !rows.empty())
            selected = 0;
    }
    void selectNone()
    {
        sel.clear();
        selected = -1;
    }
    std::vector<int> selectedRows() const { return std::vector<int>(sel.begin(), sel.end()); }
    // A click on row r (FUN_002fbfd6, the modifiers' bits 8 ⌘ and 9 ⇧: FUN_0025376e, FUN_003ae4be):
    //   ⌘   the row toggled (its sense: FUN_002fb9a8), the last click the row
    //   ⇧   lNoRect makes it add: the rows from the anchor to r chosen, the others kept (+0xB); the
    //       anchor the selection's first row when r is past it, else its last (the bounds of the
    //       cells chosen), or the last click when the chosen rows are not one run; r itself with none
    //   else a row not chosen: the rest cleared (FUN_002f8858) and r chosen; a row chosen: kept as it
    //       is, the others with it (the songs then dragged together)
    bool multiple = true;   // false: one row at a time (the browser's columns here)
    void click(int r, unsigned modifiers)
    {
        if (!multiple) {
            selectOnly(r);
            return;
        }
        if (modifiers & Command) {
            if (!sel.erase(r))
                sel.insert(r);
            selected = r;
            return;
        }
        if (modifiers & Shift) {
            int anchor = r;
            if (!sel.empty()) {
                const int first = *sel.begin(), last = *sel.rbegin();
                const bool run = last - first + 1 == int(sel.size());
                if (!run && selected >= 0)
                    anchor = selected;
                else
                    anchor = first <= r && r != first ? first : last;
            }
            for (int i = std::min(anchor, r); i <= std::max(anchor, r); ++i)
                sel.insert(i);
            selected = anchor;   // +0x48: the anchor
            return;
        }
        if (!isSelected(r))
            sel.clear();
        sel.insert(r);
        selected = r;
    }
    bool playingPaused = false;
    int sortColumn = 1;
    bool ascending = true;
    std::function<void(int)> onDoubleClick;
    std::function<void(int)> onSort;
    std::function<void(int)> onSelect;   // a row chosen by a click or the arrows
    bool statusColumn = true;            // column 0 the speaker's (the songs'); the browser's have none
    int ratingColumn = -1;               // its cells the rating (0 to 100) drawn as stars
    std::function<void(int, int)> onRate;   // a click on the selected row's stars: row, rating
    // 11.4: the header view's height at least 22 (26 with the large text), FUN_00e5675c's end
    static double headerHeight() { return eleven() ? std::max(22.0, std::ceil(lineHeight(textStyle(9004).font)) + 6) : 16; }
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
        m_scroll->setFrame({0, headerHeight(), frame().w, frame().h - headerHeight()});
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
            AK::TextStyle t;
            t.font = head.font;
            t.color = head.color;
            t.align = columns[c].align;
            if (eleven()) {
                // FUN_00024686: the cell, then the title 5 in (16 on the right of a sorted column)
                const int st = m_pressed == int(c) ? 1 : windowIsKey() ? 0 : 4;
                drawCell(cr, 304, stateColumn11(pictureInfo(304).kind, st), row, Rect{x, 0, w, headerHeight()});
                const double right = row == 1 || row == 2 ? 16 : 5;
                const double th = lineHeight(head.font);
                drawText(cr, columns[c].title, Rect{x + 5, std::floor((headerHeight() - th) / 2), w - 5 - right, th}, t);
            } else {
                drawCell(cr, 304, stateColumn(pictureInfo(304).kind, m_pressed == int(c) ? 2 : state), row, Rect{x, 0, w, headerHeight()});
                drawText(cr, columns[c].title, Rect{x + 4, 0, columns[c].width - (int(c) == sortColumn ? 20 : 8), headerHeight() - 1}, t);
            }
            x += w;
        }
    }
    void mouseDown(Event &e) override
    {
        if (e.location.y >= headerHeight())
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
            selectOnly(std::clamp(selected + (e.keysym == XKB_KEY_Down ? 1 : -1), 0, int(rows.size()) - 1));
            scrollToRow(selected);
            m_body->setNeedsDisplay();
            if (onSelect)
                onSelect(selected);
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
            const iTunes::TextStyle body = textStyle(g_listText);
            for (int r = 0; r < n; ++r) {
                const double y = r * h;
                const bool sel = list->isSelected(r) && size_t(r) < list->rows.size();
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
                        if (c == 0 && list->statusColumn) {
                            if (r == list->playing && eleven()) {   // 8455: playing row 0, paused row 1
                                const double side = std::min(cellSize(8455).h, h);
                                const Size cs{side, side};
                                drawCell(cr, 8455, sel && focused ? 1 : 0, list->playingPaused ? 1 : 0,
                                         Rect{x + std::floor((col.width - cs.w) / 2), y + std::floor((h - cs.h) / 2), cs.w, cs.h});
                            }
                            else if (r == list->playing)
                                drawCell(cr, 306, list->playingPaused ? 0 : 2, sel && focused ? 1 : 0,
                                         Rect{x + std::floor((col.width - 12) / 2), y + std::floor((h - 12) / 2), 12, 12});
                        } else if (int(c) == list->ratingColumn) {
                            // the user rating (PBUT 528 unselected, 529 selected, 531 selected without
                            // the keyboard): eleven rows of 11, a half star each
                            const std::string &cell = c < list->rows[size_t(r)].size() ? list->rows[size_t(r)][c] : std::string();
                            const int rating = std::clamp(std::atoi(cell.c_str()), 0, 100);
                            const int pic = sel ? (focused ? 529 : 531) : 528;
                            if (rating > 0 || sel)
                                drawCell(cr, pic, 0, (rating + 5) / 10, Rect{x + 5, y + std::floor((h - 11) / 2), 68, 11});
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
            // a click on the selected row's stars rates it: the star under the pointer, none left of
            // the first
            if (r >= 0 && r == list->selected && list->isSelected(r) && !(e.modifiers & (Shift | Command)) &&
                list->ratingColumn >= 0 && list->onRate) {
                double x = 0;
                for (int c = 0; c < list->ratingColumn; ++c)
                    x += list->columns[size_t(c)].width;
                const double dx = e.location.x - x - 5;
                if (dx >= -5 && dx < 68) {
                    list->onRate(r, std::clamp(int(std::ceil(dx / (68.0 / 5))), 0, 5) * 20);
                    return;
                }
            }
            if (r >= 0 && size_t(r) < list->rows.size()) {
                const std::set<int> before = list->sel;
                list->click(r, e.modifiers);
                const bool changed = list->sel != before;
                setNeedsDisplay();
                list->setNeedsDisplay();
                if (changed && list->onSelect)
                    list->onSelect(r);
                if (e.clickCount == 2 && list->onDoubleClick)
                    list->onDoubleClick(r);
                return;
            }
            setNeedsDisplay();
            list->setNeedsDisplay();
        }
    };
    ScrollView *m_scroll;
    Body *m_body;
    int m_pressed = -1;
};

// ---- 8.2.1's Store bar (MusicStoreBar.nib: a pane 448 × 28 on PBUT 612, stretched): Back (PBUT 600,
//   27 × 19 at 7, 4, Msgb), Forward (602, 26 × 19 at 34, Msgf), Home (626, 32 × 19 at 67, Msgh), bound
//   to the left; the breadcrumb (644, 3 points kept at each end, TXTS 600) 54 × 19 at 160 from the
//   right, and the account button (606, 11 kept at each end, TXTS 600; Msvc) 146 × 19, 7 from the
//   right, bound to the right. Each button three columns (kind 3: disabled, normal, pressed).
class StoreBar : public View {
public:
    // FUN_000249e4 (the Store source, kind 2): the bar takes the main area's top 27 points (the nib's
    // pane is 28, PBUT 612 27 high), the pages under it
    static constexpr double Height = 27;
    bool canBack = false, canForward = false;
    std::string account = "Sign In";
    std::function<void(const std::string &)> onCommand;   // Msgb, Msgf, Msgh, Msvc
    void draw(cairo_t *cr) override
    {
        drawCell(cr, 612, 0, 0, bounds());
        const struct { int pic; bool on; const char *cmd; } buttons[3] = {{600, canBack, "Msgb"}, {602, canForward, "Msgf"}, {626, true, "Msgh"}};
        for (int i = 0; i < 3; ++i)
            drawCell(cr, buttons[i].pic, stateColumn(3, !buttons[i].on ? 0 : m_pressed == i ? 2 : 1), 0, rectOf(i));
        AK::TextStyle t;
        t.font = textStyle(600).font;
        t.color = Color::gray(0);
        t.align = Align::Center;
        drawCell(cr, 606, stateColumn(3, m_pressed == 3 ? 2 : 1), 0, rectOf(3));
        drawText(cr, account, rectOf(3).translated(0, -1), t);
    }
    void mouseDown(Event &e) override
    {
        m_pressed = -1;
        for (int i = 0; i < 4; ++i)
            if (rectOf(i).contains(e.location))
                m_pressed = i;
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        static const char *const cmds[4] = {"Msgb", "Msgf", "Msgh", "Msvc"};
        const int i = m_pressed;
        m_pressed = -1;
        setNeedsDisplay();
        if (i >= 0 && rectOf(i).contains(e.location) && onCommand) {
            if ((i == 0 && !canBack) || (i == 1 && !canForward))
                return;
            onCommand(cmds[i]);
        }
    }

private:
    Rect rectOf(int i) const
    {
        switch (i) {
        case 0: return {7, 4, 27, 19};
        case 1: return {34, 4, 26, 19};
        case 2: return {67, 4, 32, 19};
        default: return {frame().w - 7 - 146, 4, 146, 19};
        }
    }
    int m_pressed = -1;
};

// ---- the browser (View ▸ Show Browser, ⌘B): BrowserWindow's layout (FUN_000249e4 at 0x25454–0x25640)
//   puts three ColumnDisplays over the songs, as tall as the browser's height (+0x452: at first half the
//   list's room less 10, FUN_000683fa's 0.5; at least 80, and the songs keep 120 below, FUN_0013cbbe),
//   across the list's width: with the genre (the preference at +0x25B) Genre from the left a third,
//   Artist the middle, Album the last third; without, Artist and Album halves. A ColumnDisplay
//   (FUN_0040e0ac) is the songs' list engine (FUN_002fa50e) with one column, its header in Txtr 9004
//   (0x232C); its first row "All (N Genres)" (STR# 166). The songs start 10 points under it: the
//   splitter, the window's pictures 313 (+8, stretched) and 312 (+4, its grip, centred), loaded with the
//   window (FUN_0000fe66 at 0x10744, 0x10e71).

class BrowserSplitter : public View {
public:
    std::function<void()> onPress, onRelease;
    std::function<void(double)> onDrag;   // the pointer's move since the press
    void draw(cairo_t *cr) override
    {
        drawCell(cr, 313, 0, 0, bounds());
        const Size grip = cellSize(312);
        drawCell(cr, 312, 0, 0, Rect{std::floor((frame().w - grip.w) / 2), 0, grip.w, grip.h});
    }
    void mouseDown(Event &e) override
    {
        m_from = e.screen.y;
        if (onPress)
            onPress();
    }
    void mouseDragged(Event &e) override
    {
        if (onDrag)
            onDrag(e.screen.y - m_from);
    }
    void mouseUp(Event &) override
    {
        if (onRelease)
            onRelease();
    }

private:
    double m_from = 0;
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
    bool folder = false, open = false;
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
            // a folder's triangle (FUN_0002d3b2: at 14n + 5; PBUT 314 "TrackDisclosure", 10 × 10 cells:
            // rows 0 closed, 2 open, 3 to 5 the same white on a chosen row; column 1, 2 pressed)
            if (r.folder) {
                const Image tri = picture(314);   // not in the picture table: its cells cut here
                if (!tri.isNull())
                    tri.draw(cr, Rect{10.0 * (int(i) == m_pressed ? 2 : 1), 10.0 * ((r.open ? 2 : 0) + (sel ? 3 : 0)), 10, 10},
                             Rect{14 * r.level + 5, row.y + 5, 10, 10});
            }
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
        // the drop's place: the folder ringed (4), or the whole list (3), in the selection's blue (by eye)
        if (m_dragging && m_drop) {
            const Rect ring = m_drop == 4 && m_dropOver >= 0 ? Rect{1, m_dropOver * RowHeight + 1, frame().w - 2, RowHeight - 2}
                                                            : Rect{1, 1, frame().w - 2, std::max(contentHeight(), frame().h) - 2};
            cairo_save(cr);
            const Color c = colour(10);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 1);
            cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, ring.x + 0.5, ring.y + 0.5, ring.w - 1, ring.h - 1);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }
    std::function<void(int)> onToggle;   // a folder's triangle clicked: its row
    std::function<void(unsigned)> onDelete;   // Delete with the sources' keyboard: the modifiers
    // Renaming in place (FUN_00166b90 → FUN_0058b80c: a field over the row's name, after a delay in
    // ticks): onRename(row, the name) when it is kept
    std::function<void(int, const std::string &)> onRename;
    void startRename(int row, double delay)
    {
        cancelRename();
        if (row < 0 || size_t(row) >= rows.size() || rows[size_t(row)].tag < 0)
            return;
        m_renameTimer = App::shared().addTimer(delay, false, [this, row] {
            m_renameTimer = 0;
            beginEditing(row);
        });
    }
    void cancelRename()
    {
        if (m_renameTimer) {
            App::shared().removeTimer(m_renameTimer);
            m_renameTimer = 0;
        }
        if (m_editor) {
            TextField *f = m_editor;
            m_editor = nullptr;
            m_editRow = -1;
            if (window())
                window()->makeFirstResponder(this);
            App::shared().later([this, f] { removeSubview(f); });
        }
    }
    void commitRename()
    {
        if (!m_editor)
            return;
        const int row = m_editRow;
        const std::string name = m_editor->text;
        cancelRename();
        if (onRename)
            onRename(row, name);
    }
    bool renaming() const { return m_editor != nullptr; }
    // A playlist dragged (FUN_00169d0a tracks it, FUN_00166758 says what a drop there does, +0x2D9):
    // dropState(from row, over row; -1 under the rows) 4 into that folder, 3 out to the top, 0 nothing;
    // onDrop(from, over) then
    std::function<int(int, int)> dropState;
    std::function<void(int, int)> onDrop;
    void mouseDragged(Event &e) override
    {
        if (m_dragFrom < 0 || m_pressed >= 0)
            return;
        if (!m_dragging && std::hypot(e.location.x - m_downAt.x, e.location.y - m_downAt.y) < 4)
            return;
        if (!m_dragging)
            cancelRename();
        m_dragging = true;
        int over = int(e.location.y / RowHeight);
        if (e.location.y < 0 || over >= int(rows.size()))
            over = -1;
        m_dropOver = over;
        m_drop = dropState ? dropState(m_dragFrom, over) : 0;
        setNeedsDisplay();
    }
    void mouseDown(Event &e) override
    {
        window()->makeFirstResponder(this);
        const int i = int(e.location.y / RowHeight);
        m_downAt = e.location;
        m_dragging = false;
        m_drop = 0;
        m_dragFrom = i >= 0 && size_t(i) < rows.size() && rows[size_t(i)].tag >= 0 && !rows[size_t(i)].header ? i : -1;
        commitRename();
        // FUN_0016e5fe: a click on the row already chosen renames it after the double-click time
        // (GetDblTime; 0.5 s here), a second click before then cancels it
        if (e.clickCount > 1)
            cancelRename();
        else if (i == m_selected && m_dragFrom >= 0 && !(e.modifiers & Command))
            startRename(i, 0.5);
        if (i >= 0 && size_t(i) < rows.size() && rows[size_t(i)].folder) {
            const double tx = 14 * rows[size_t(i)].level + 5;
            if (e.location.x >= tx - 3 && e.location.x < tx + 13) {
                m_pressed = i;
                setNeedsDisplay();
                return;
            }
        }
        select(i, true);
    }
    void mouseUp(Event &e) override
    {
        if (m_dragging) {
            const int from = m_dragFrom, over = m_dropOver, state = m_drop;
            m_dragging = false;
            m_dragFrom = -1;
            m_drop = 0;
            setNeedsDisplay();
            if (state && onDrop)
                onDrop(from, over);
            return;
        }
        m_dragFrom = -1;
        const int i = m_pressed;
        m_pressed = -1;
        setNeedsDisplay();
        if (i >= 0 && int(e.location.y / RowHeight) == i && onToggle)
            onToggle(i);
    }
    void keyDown(Event &e) override
    {
        // FUN_0016b9ea with FUN_003ae408: Enter, or ⌘-Return, renames the row chosen at once
        if ((e.keysym == XKB_KEY_KP_Enter || (e.keysym == XKB_KEY_Return && (e.modifiers & Command))) && m_selected >= 0) {
            startRename(m_selected, 0);
            return;
        }
        // Delete (FUN_001677bc → FUN_00165bec): the playlist chosen, the modifiers with it
        if ((e.keysym == XKB_KEY_BackSpace || e.keysym == XKB_KEY_Delete) && onDelete) {
            onDelete(e.modifiers);
            return;
        }
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
    void beginEditing(int row)
    {
        if (row < 0 || size_t(row) >= rows.size())
            return;
        const SourceRow &r = rows[size_t(row)];
        const double tx = 14 * r.level + 39;
        m_editor = add<TextField>();
        m_editor->font = textStyle(g_listText).font;
        m_editor->setFrame({tx - 3, row * RowHeight, std::max(40.0, frame().w - tx - 1), RowHeight});
        m_editor->setText(r.title);
        m_editor->selectAll();
        m_editRow = row;
        m_editor->onEnter = [this] { commitRename(); };
        m_editor->onCancel = [this] { cancelRename(); };
        window()->makeFirstResponder(m_editor);
    }
    TextField *m_editor = nullptr;
    int m_editRow = -1, m_renameTimer = 0;
    int m_selected = -1;
    int m_pressed = -1;   // a triangle held
    int m_dragFrom = -1, m_dropOver = -1, m_drop = 0;
    bool m_dragging = false;
    Point m_downAt;
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
        if (!eleven())
            m_equalizer.apply();
        applyPreferences();
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

    // Documents opened (the Finder, the Dock, `open`): added to the library and the first song played,
    // in the Library, as iTunes does with a file double-clicked in the Finder
    void openFiles(const std::vector<std::string> &paths)
    {
        if (paths.empty())
            return;
        addFiles(paths);
        int id = 0;
        for (const std::string &p : paths)
            if ((id = m_library.idForPath(p)))
                break;
        if (!id)
            return;
        if (m_search && !m_search->text.empty())
            m_search->setText(std::string());
        setStoreMode(false);
        if (m_sources->selected() != 1)   // Music, under LIBRARY
            m_sources->select(1);
        loadSource();
        const auto it = std::find(m_list->ids.begin(), m_list->ids.end(), id);
        if (it != m_list->ids.end())
            playRow(int(it - m_list->ids.begin()));
        else
            playTrack(id);
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
        m_window->onClose = [this] {
            if (!m_toMini11)   // 11.4: the main window goes for the MiniPlayer's, iTunes stays
                App::shared().quit();
        };
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
            m_views[i] = m_top->add<PictureButton>(540 + i, 0);
            m_views[i]->setEnabled(true);   // the list, the grid, Cover Flow
            m_views[i]->onClick = [this, i] { setViewMode(i); };
            m_views[i]->setHidden(eleven());  // 11.4: no view buttons in the top part
        }
        m_search = m_top->add<SearchField>();
        m_search->placeholder = tr("kSearchFieldPrompt") == "kSearchFieldPrompt" ? "Search" : tr("kSearchFieldPrompt");
        m_search->onChange = [this](const std::string &) { loadSource(); };

        m_sourceScroll = c->add<ScrollView>();
        m_sources = m_sourceScroll->document()->add<SourceList>();
        for (const std::string &id : Settings("itunes").list("OpenFolders"))
            m_openFolders.insert(id);
        m_sources->onSelect = [this](int) { loadSource(); };
        // FUN_00166758: a folder takes a playlist or folder of the library that is not already in it, not
        // itself and not one of its own folders (FUN_000db944); under the rows, one in a folder goes
        // out to the top (3)
        m_sources->dropState = [this](int from, int over) {
            const int ft = m_sources->rows[size_t(from)].tag;
            if (ft < 0)
                return 0;
            const Playlist &p = m_library.playlists()[size_t(ft)];
            if (over < 0)
                return p.parentId.empty() ? 0 : 3;
            const int ot = m_sources->rows[size_t(over)].tag;
            if (ot < 0 || ot == ft || !m_library.playlists()[size_t(ot)].folder)
                return 0;
            const Playlist &folder = m_library.playlists()[size_t(ot)];
            if (p.parentId == folder.persistentId || inside(folder, p.persistentId))
                return 0;
            return 4;
        };
        m_sources->onDrop = [this](int from, int over) {
            Playlist &p = m_library.playlists()[size_t(m_sources->rows[size_t(from)].tag)];
            // FUN_000dd8c8: out of its folder, into the new one (opened to show it)
            p.parentId = over < 0 ? std::string() : m_library.playlists()[size_t(m_sources->rows[size_t(over)].tag)].persistentId;
            const int moved = m_sources->rows[size_t(from)].tag;
            if (!p.parentId.empty()) {
                m_openFolders.insert(p.parentId);
                Settings("itunes").set("OpenFolders", std::vector<std::string>(m_openFolders.begin(), m_openFolders.end()));
            }
            m_library.save();
            const int chosen = selectedPlaylist();
            rebuildSources();
            for (size_t i = 0; i < m_sources->rows.size(); ++i)
                if (m_sources->rows[i].tag == chosen)
                    m_sources->select(int(i));
            (void)moved;
            loadSource();
            layout();
        };
        m_sources->onDelete = [this](unsigned mods) { deletePlaylist(mods); };
        // the name kept (FUN_000dbc2e); an empty one leaves the old
        m_sources->onRename = [this](int row, const std::string &name) {
            const int tag = m_sources->rows[size_t(row)].tag;
            if (tag < 0 || size_t(tag) >= m_library.playlists().size() || name.empty() ||
                m_library.playlists()[size_t(tag)].name == name)
                return;
            m_library.playlists()[size_t(tag)].name = name;
            m_library.save();
            rebuildSources();   // its place by its new name
            for (size_t i = 0; i < m_sources->rows.size(); ++i)
                if (m_sources->rows[i].tag == tag)
                    m_sources->select(int(i));
            layout();
        };
        m_sources->onToggle = [this](int row) {
            const int tag = m_sources->rows[size_t(row)].tag;
            const std::string id = m_library.playlists()[size_t(tag)].persistentId;
            if (!m_openFolders.erase(id))
                m_openFolders.insert(id);
            Settings("itunes").set("OpenFolders", std::vector<std::string>(m_openFolders.begin(), m_openFolders.end()));
            // the chosen playlist kept chosen, unless it was in the folder closed: then the folder
            const int chosen = selectedPlaylist();
            rebuildSources();
            int at = row;
            for (size_t i = 0; i < m_sources->rows.size(); ++i)
                if (m_sources->rows[i].tag == chosen)
                    at = int(i);
            m_sources->select(at, at != m_sources->selected());
            layout();
        };
        m_divider = c->add<View>();

        m_list = c->add<TrackList>();
        // the status column (Play Status, the speaker), then the columns View Options chose (at first
        // Name, Time, Artist, Album, Genre, Play Count, My Rating): STR# 9006's titles, CWid 9000's widths
        {
            std::istringstream in(Settings("itunes").string("Columns"));
            for (int f; in >> f;)
                if (listField(f))
                    m_columnFields.push_back(f);
            if (m_columnFields.empty())
                m_columnFields = defaultColumns();
        }
        applyColumns();
        m_list->onRate = [this](int row, int rating) { rate(m_list->ids[size_t(row)], rating); };
        m_list->onDoubleClick = [this](int row) { playRow(row); };
        m_list->onSelect = [this](int row) {
            if (m_viewMode == 2 && row >= 0 && size_t(row) < m_list->ids.size())
                m_flow->showSong(m_list->ids[size_t(row)]);
        };
        m_list->onSort = [this](int c) {
            if (c <= 0 || size_t(c) > m_columnFields.size())
                return;
            if (m_list->sortColumn == c)
                m_list->ascending = !m_list->ascending;
            else {
                m_list->sortColumn = c;
                m_list->ascending = true;
            }
            m_sortField = m_columnFields[size_t(c) - 1];
            loadSource();
        };

        // the browser: Genre, Artist, Album (STR# 9006's titles)
        static const char *const titles[3] = {"9006.006", "9006.003", "9006.002"};
        for (int i = 0; i < 3; ++i) {
            m_browse[i] = c->add<TrackList>();
            m_browse[i]->statusColumn = false;
            m_browse[i]->multiple = false;
            m_browse[i]->sortColumn = -1;
            m_browse[i]->columns = {{tr(titles[i]), 100, Align::Left}};
            m_browse[i]->onSelect = [this, i](int row) { chooseInBrowser(i, row); };
            m_browse[i]->setHidden(true);
        }
        // the grid (View ▸ as Grid)
        m_grid = c->add<GridView>(m_library);
        m_grid->setHidden(true);
        m_grid->onPlay = [this](const std::vector<int> &ids) {
            if (ids.empty())
                return;
            m_queue = ids;
            m_queuePos = 0;
            if (m_shuffle)
                shuffleQueue(0);
            playTrack(m_queue[0]);
        };
        // Cover Flow (View ▸ as Cover Flow)
        m_flow = c->add<CoverFlowView>(m_library);
        m_flow->setHidden(true);
        m_flow->onChoose = [this](int id) {
            const auto it = std::find(m_list->ids.begin(), m_list->ids.end(), id);
            if (it == m_list->ids.end())
                return;
            m_list->selectOnly(int(it - m_list->ids.begin()));
            m_list->scrollToRow(m_list->selected);
            m_list->body()->setNeedsDisplay();
        };
        m_flow->onPlay = m_grid->onPlay;
        m_flow->onFullScreen = [this] { fullScreenCoverFlow(); };
        m_viewMode = eleven() ? 0 : std::clamp(Settings("itunes").integer("ViewMode", 0), 0, 2);
        // 8.2.1's iTunes Store, a source: the Store bar over the pages (iTunesStore's)
        if (!eleven() && StoreView::available()) {
            m_store = c->add<StoreView>();
            m_store->setHidden(true);
            m_storeBar = c->add<StoreBar>();
            m_storeBar->setHidden(true);
            m_storeBar->account = tr("137.080");
            m_storeBar->onCommand = [this](const std::string &cmd) {
                if (cmd == "Msvc")
                    return;   // Sign In: no account here
                m_store->command(cmd == "Msgb" ? "back" : cmd == "Msgf" ? "forward" : "home");
            };
            m_store->onNavigation = [this] {
                m_storeBar->canBack = m_store->canGoBack();
                m_storeBar->canForward = m_store->canGoForward();
                m_storeBar->setNeedsDisplay();
            };
            m_store->onLibrary = [this] { m_sources->select(1, true); };
        }
        m_splitter = c->add<BrowserSplitter>();
        m_splitter->setHidden(true);
        m_splitter->onPress = [this] { m_browserDragFrom = m_browse[1]->frame().h; };
        m_splitter->onRelease = [this] { Settings("itunes").set("BrowserHeight", int(m_browserHeight)); };
        m_splitter->onDrag = [this](double dy) {
            m_browserHeight = m_browserDragFrom + dy;
            layout();
        };
        {
            Settings s("itunes");
            m_showBrowser = !eleven() && s.boolean("Browser", false);
            m_browseGenre = s.boolean("BrowserGenre", true);
            m_browserHeight = s.integer("BrowserHeight", -1);
        }

        if (eleven())
            buildNavAndStore(c);

        // the visualizer over the body (View ▸ Show Visualizer): 8.2.1's Magnetosphere
        if (!eleven()) {
            m_visualizer = c->add<Visualizer>(m_player);
            m_visualizer->setHidden(true);
        }

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
        m_window->makeFirstResponder(m_viewMode == 2 ? static_cast<View *>(m_flow) : m_viewMode == 1 ? static_cast<View *>(m_grid) : m_list);
    }

    // 11.4: the nav bar and the Store (BWNavBarController, BWStoreController)
    void buildNavAndStore(View *c)
    {
        m_nav = c->add<NavBar>();
        // STR# 163: Songs, Albums, Artists, Genres, Playlists, Radio; iTunes Store, Library
        for (const char *k : {"163.099", "163.100", "163.101", "163.102", "163.104", "163.106"})
            m_nav->views.push_back(tr(k));
        // Songs, and Albums, Artists and Genres as grids (AlbumsGridViewController and the like)
        m_nav->viewEnabled = {true, true, true, true, false, false};
        m_nav->storeTitle = tr("163.084");
        m_nav->libraryTitle = tr("163.092");
        m_nav->onView = [this](int v) {
            m_view11 = v;
            if (v >= 1 && v <= 3)
                m_grid->setGrouping(GridView::Grouping(v - 1));
            setStoreMode(false);
        };
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
    // The zoom button and Window ▸ Zoom: iTunes' mini player (FUN_00149a4a, the flag +0x172). Each mode
    // keeps its own size (+0x176 the full one, +0x180 the mini one, 200 wide at first: FUN_0000fe66 at
    // 0xff71); the limits are kEventWindowGetMinimum/MaximumSize's (FUN_001440ca): mini 65 high, 65 to
    // 450 wide; full at least 700 x 400, unbounded. 11.4's MiniPlayer is a window of its own (miniplayer11.cpp).
    void zoom()
    {
        if (eleven()) {
            switchToMini11(!(m_mini11 && m_mini11->isShown()));
            return;
        }
        const Size now = m_window->size();
        m_mini = !m_mini;
        if (m_mini) {
            m_fullSize = now;
            m_window->setResizable({65, TitleBarHeight + 65}, {450, TitleBarHeight + 65});
            m_window->setContentSize({std::clamp(m_miniWidth, 65.0, 450.0), TitleBarHeight + 65});
        } else {
            m_miniWidth = now.w;
            m_window->setResizable({700, 400});
            m_window->setContentSize(m_fullSize);
        }
        for (View *v : std::initializer_list<View *>{m_sourceScroll, m_divider, m_list, m_bottom, m_search, m_views[0], m_views[1], m_views[2],
                                                    m_browse[0], m_browse[1], m_browse[2], m_splitter, m_grid, m_flow})
            v->setHidden(m_mini);
        m_prev->setPicture(m_mini ? 322 : 316);
        m_play->setPicture(m_mini ? 326 : 320);
        m_next->setPicture(m_mini ? 324 : 318);
        m_top->mini = m_mini;
        if (m_mini && m_visualizerShown)
            showVisualizer(false);
        // Advanced ▸ Keep MiniPlayer on top of all other windows
        m_window->setFloating(m_mini && Settings("itunes").boolean("MiniOnTop", false));
        layout();
    }

    // 11.4: Window ▸ Switch to MiniPlayer (`smin`, ⌥⌘M) hides the main window for the MiniPlayer's;
    // its expand button and its close button bring the main window back
    void switchToMini11(bool on)
    {
        if (!m_mini11) {
            MiniPlayer11::Hooks h;
            h.playPause = [this] { playPause(); };
            h.next = [this] { advance(1, false); };
            h.previous = [this] { previous(); };
            h.expand = h.close = [this] { switchToMini11(false); };
            h.setVolume = [this](double v) { setVolume(v); };
            m_mini11 = std::make_unique<MiniPlayer11>(h);
        }
        if (on) {
            m_mini11->show();
            m_mini11->setFloating(Settings("itunes").boolean("MiniOnTop", false));
            m_mini11->setVolume(m_volume->value);
            m_toMini11 = true;
            m_window->close();
            m_toMini11 = false;
            refreshLCD();
        } else {
            m_mini11->hide();
            m_window->show();
        }
    }

    // 11.4's Up Next: the queue's songs after the one playing (View ▸ Show Up Next, `PQsh`); a row's
    // play button plays it, its remove button takes it out, Clear (`PQcl`) empties what is to come
    void refreshUpNext()
    {
        if (!m_upNext || !m_upNext->isShown())
            return;
        std::vector<UpNext11::Song> songs;
        for (size_t i = size_t(m_queuePos) + 1; i < m_queue.size() && songs.size() < 50; ++i)
            if (const Track *t = m_library.track(m_queue[i]))
                songs.push_back({t->name, t->artist, Image::fromData(artworkData(t->path), {40, 40})});
        m_upNext->setSongs(std::move(songs));
    }

    void showUpNext()
    {
        if (!m_upNext) {
            m_upNext = std::make_unique<UpNext11>();
            m_upNext->onPlay = [this](size_t i) {
                m_queuePos = int(std::min(m_queue.size() - 1, size_t(m_queuePos) + 1 + i));
                playTrack(m_queue[size_t(m_queuePos)]);
                refreshUpNext();
            };
            m_upNext->onRemove = [this](size_t i) {
                const size_t at = size_t(m_queuePos) + 1 + i;
                if (at < m_queue.size())
                    m_queue.erase(m_queue.begin() + long(at));
                refreshUpNext();
            };
            m_upNext->onClear = [this] { clearUpNext(); };
        }
        m_upNext->toggle();
        refreshUpNext();
    }

    void clearUpNext()
    {
        if (size_t(m_queuePos) + 1 < m_queue.size())
            m_queue.erase(m_queue.begin() + m_queuePos + 1, m_queue.end());
        refreshUpNext();
    }

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
        m_frameButtons->setFrame({8, 4, 56, 16});
        if (m_mini) {
            // the mini player (FUN_00142cb2's mini cases, FUN_0013be44, FUN_000249e4): the top band is
            // the whole window
            m_top->setFrame({0, 0, W, H});
            m_prev->setFrame({3, T + 11, 29, 29});
            m_play->setFrame({32, T + 9, 33, 33});
            m_next->setFrame({65, T + 11, 29, 29});
            m_volume->setFrame({19, T + 47, 58, 12});
            m_speakerLow->setFrame({7, T + 46, 13.5, 13});
            m_speakerHigh->setFrame({81, T + 46, 13.5, 13});
            // the LCD from (106, 10) to (W − 10, 56); none when that leaves it no room
            const Rect lcd{106, T + 10, W - 10 - 106, 46};
            m_lcd->setHidden(lcd.w < 40);
            m_lcd->setFrame(lcd.w < 40 ? Rect{106, T + 10, 40, 46} : lcd);
            m_window->contentView()->setNeedsDisplay();
            return;
        }
        m_lcd->setHidden(false);
        m_top->setFrame({0, 0, W, T + TopBandHeight});
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
        layoutBrowser({sourceW + 1, bodyTop, W - sourceW - 1, bodyH});
        if (m_visualizer)
            m_visualizer->setFrame({0, bodyTop, W, bodyH});

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

    // The list's room: the browser over the songs when shown (FUN_000249e4, FUN_0013cbbe)
    void layoutBrowser(const Rect &area)
    {
        if (m_store && !eleven()) {
            const bool store = m_store8 && !m_mini;
            m_store->setHidden(!store);
            m_storeBar->setHidden(!store);
            if (store) {
                for (View *v : std::initializer_list<View *>{m_list, m_grid, m_flow, m_splitter, m_browse[0], m_browse[1], m_browse[2]})
                    v->setHidden(true);
                m_storeBar->setFrame({area.x, area.y, area.w, StoreBar::Height});
                m_store->setFrame({area.x, area.y + StoreBar::Height, area.w, std::max(0.0, area.h - StoreBar::Height)});
                return;
            }
        }
        // the view buttons' row: the chosen one's second row
        for (int i = 0; i < 3; ++i)
            m_views[i]->setRow(i == m_viewMode ? 1 : 0);
        // the grid takes the list's room, with its own bar instead of the browser
        const bool grid = m_viewMode == 1 && !m_mini, flow = m_viewMode == 2 && !m_mini;
        m_grid->setHidden(!grid);
        m_flow->setHidden(!flow);
        m_list->setHidden(grid || m_mini);
        if (flow) {
            // Cover Flow over the songs (FUN_000249e4 with the view mode +0x152 = 2): its height the
            // preference's (+0x593a; nothing kept: 0), at least 300, at most what leaves the songs their
            // 120 (FUN_0013cbbe); the songs right under it (the browser's 10 between: not here)
            for (TrackList *b : m_browse)
                b->setHidden(true);
            m_splitter->setHidden(true);
            const double kept = Settings("itunes").integer("CoverFlowHeight", 0);
            const double h = std::max(0.0, std::min(std::max(kept, 300.0), area.h - 120));
            m_flow->setFrame({area.x, area.y, area.w, h});
            m_list->setFrame({area.x, area.y + h, area.w, area.h - h});
            return;
        }
        if (grid) {
            for (TrackList *b : m_browse)
                b->setHidden(true);
            m_splitter->setHidden(true);
            m_grid->setFrame(area);
            return;
        }
        const bool shown = m_showBrowser && !m_mini;
        for (TrackList *b : m_browse)
            b->setHidden(!shown);
        m_browse[0]->setHidden(!shown || !m_browseGenre);
        m_splitter->setHidden(!shown);
        if (!shown) {
            m_list->setFrame(area);
            return;
        }
        // its height: half the room less the splitter at first; at least 80, the songs keeping 120
        const double room = area.h - 10;
        if (m_browserHeight < 0)
            m_browserHeight = std::floor(room * 0.5);
        const double h = std::max(std::min(80.0, room), std::min(m_browserHeight, room - 120));
        const double third = std::floor(area.w / 3), half = std::floor(area.w / 2);
        Rect cols[3];
        if (m_browseGenre) {
            cols[0] = {area.x, area.y, third, h};
            cols[1] = {area.x + third, area.y, area.w - 2 * third, h};
            cols[2] = {area.right() - third, area.y, third, h};
        } else {
            cols[1] = {area.x, area.y, half, h};
            cols[2] = {area.x + half, area.y, area.w - half, h};
        }
        for (int i = 0; i < 3; ++i) {
            if (cols[i].w <= 0)
                continue;
            // a line between the columns: each but the last a point narrower
            m_browse[i]->columns[0].width = cols[i].w;
            m_browse[i]->setFrame(i < 2 ? Rect{cols[i].x, cols[i].y, cols[i].w - 1, cols[i].h} : cols[i]);
        }
        m_splitter->setFrame({area.x, area.y + h, area.w, 10});
        m_list->setFrame({area.x, area.y + h + 10, area.w, std::max(0.0, area.h - h - 10)});
    }

    void setViewMode(int mode)
    {
        if (mode == m_viewMode || eleven())
            return;
        m_viewMode = mode;
        Settings("itunes").set("ViewMode", mode);
        layout();
        loadSource();
        m_window->makeFirstResponder(mode == 1 ? static_cast<View *>(m_grid) : mode == 2 ? static_cast<View *>(m_flow) : m_list);
    }

    void toggleBrowser()
    {
        m_showBrowser = !m_showBrowser;
        Settings("itunes").set("Browser", m_showBrowser);
        if (!m_showBrowser)
            for (std::string &s : m_browseSel)
                s.clear();
        layout();
        loadSource();
    }

    // A row of a browser column chosen: row 0 is All; the columns right of it start again at All
    void chooseInBrowser(int column, int row)
    {
        TrackList *b = m_browse[column];
        m_browseSel[column] = row <= 0 || size_t(row) >= b->rows.size() ? std::string() : b->rows[size_t(row)][0];
        for (int i = column + 1; i < 3; ++i)
            m_browseSel[i].clear();
        loadSource();
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
        // Albums, Artists, Genres: the grid in the songs' place
        const bool grid = m_view11 >= 1 && m_view11 <= 3 && !(m_nav && m_nav->store);
        m_grid->setHidden(!grid);
        m_list->setHidden(grid || (m_nav && m_nav->store));
        if (grid)
            m_grid->setFrame({sourceW + 1, bodyTop, W - sourceW - 1, bodyH});
        m_bottom->setFrame({0, H - BottomHeight11, W, BottomHeight11});
        m_window->contentView()->setNeedsDisplay();
    }

    // ---- the sources: LIBRARY (Music), STORE, PLAYLISTS

    void rebuildSources()
    {
        m_sources->cancelRename();
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
        store.selectable = !eleven() && StoreView::available();   // 8.2.1: the Store in the window
        store.tag = -2;
        m_sources->rows.push_back(store);
        header(e ? "135.009" : "135.014");
        // the playlists by their folders (Parent Persistent ID), a folder's own below it when it is open
        const std::vector<Playlist> &lists = m_library.playlists();
        std::set<std::string> ids;
        for (const Playlist &p : lists)
            ids.insert(p.persistentId);
        std::function<void(const std::string &, int)> add = [&](const std::string &parent, int level) {
            if (level > 16)
                return;
            // FUN_00166e76 / FUN_00166d96: a level's rows by their group (FUN_00168254, row +0x20: 0x84
            // a folder, 0x86 a smart playlist, 0x88 a playlist), then their names (FUN_0028efca with
            // 0x1B), then their IDs
            std::vector<size_t> order;
            for (size_t i = 0; i < lists.size(); ++i) {
                const Playlist &p = lists[i];
                const std::string up = ids.count(p.parentId) ? p.parentId : std::string();
                if (!p.master && up == parent)
                    order.push_back(i);
            }
            auto group = [](const Playlist &p) { return p.folder ? 0x84 : p.smart ? 0x86 : 0x88; };
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const int ga = group(lists[a]), gb = group(lists[b]);
                if (ga != gb)
                    return ga < gb;
                const int c = compareNames(lists[a].name, lists[b].name);
                return c != 0 ? c < 0 : a < b;
            });
            for (size_t i : order) {
                const Playlist &p = lists[i];
                SourceRow r;
                r.title = p.name;
                // 441's rows: a playlist, a smart playlist (the gear), a folder
                r.icon = p.folder ? 20 : p.smart ? 16 : 15;
                r.tag = int(i);
                r.level = level;
                r.folder = p.folder;
                r.open = p.folder && m_openFolders.count(p.persistentId);
                m_sources->rows.push_back(r);
                if (r.open)
                    add(p.persistentId, level + 1);
            }
        };
        add({}, 0);
        m_sources->setNeedsDisplay();
        m_sources->select(keep >= 0 && size_t(keep) < m_sources->rows.size() ? keep : 1);
    }

    // The songs of the selected source, filtered by the search field, sorted by the column
    bool storeShown() const { return m_nav ? m_nav->store : m_store8; }

    void loadSource()
    {
        // 8.2.1: the iTunes Store source shows the Store in the songs' place
        if (!eleven() && m_store) {
            const int sel = m_sources->selected();
            const bool store = sel >= 0 && size_t(sel) < m_sources->rows.size() && m_sources->rows[size_t(sel)].tag == -2;
            if (store != m_store8) {
                m_store8 = store;
                layout();
                if (store) {
                    m_store->start();
                    m_window->makeFirstResponder(m_store);
                }
            }
            if (store) {
                m_bottom->totals.clear();
                m_bottom->setNeedsDisplay();
                return;
            }
        }
        std::vector<int> ids;
        const int sel = m_sources->selected();
        const int tag = sel >= 0 && size_t(sel) < m_sources->rows.size() ? m_sources->rows[size_t(sel)].tag : -1;
        if (tag >= 0 && size_t(tag) < m_library.playlists().size()) {
            const Playlist &p = m_library.playlists()[size_t(tag)];
            // a smart playlist's songs: its rules applied now when it updates live, else as they were
            ids = p.folder ? m_library.folderItems(p) : p.smart && p.info.live ? m_library.evaluate(p.info) : p.items;
        }
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
        if (!eleven()) {
            const auto d = m_dups.find(tag);
            if (d != m_dups.end() && d->second)
                shown = duplicates(m_library, shown, d->second == 2);
        }
        if ((m_viewMode == 1 && !eleven()) || eleven())   // 11.4: for its Albums, Artists, Genres
            m_grid->setSongs(shown);
        else if (m_showBrowser && !eleven() && m_viewMode == 0)
            shown = browse(shown);
        // sorted by the field of the column chosen
        const int field = m_sortField;
        const bool asc = m_list->ascending;
        const ListField *lf = listField(field);
        const bool numeric = lf && lf->numeric;
        std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) {
            const Track &ta = *m_library.track(a), &tb = *m_library.track(b);
            if (numeric) {
                const long long x = cellNumber(ta, field), y = cellNumber(tb, field);
                return asc ? x < y : y < x;
            }
            const std::string x = cellKey(ta, field), y = cellKey(tb, field);
            return asc ? x < y : y < x;
        });
        // the selection kept by song: the rows chosen and the last click follow their songs
        std::set<int> chosenIds;
        for (int r : m_list->sel)
            if (r >= 0 && size_t(r) < m_list->ids.size())
                chosenIds.insert(m_list->ids[size_t(r)]);
        const int focusId = m_list->selected >= 0 && size_t(m_list->selected) < m_list->ids.size() ? m_list->ids[size_t(m_list->selected)] : -1;
        m_list->ids = shown;
        m_list->sel.clear();
        m_list->selected = -1;
        for (size_t r = 0; r < shown.size(); ++r) {
            if (chosenIds.count(shown[r]))
                m_list->sel.insert(int(r));
            if (shown[r] == focusId)
                m_list->selected = int(r);
        }
        if (m_list->selected < 0 && !m_list->sel.empty())
            m_list->selected = *m_list->sel.begin();
        m_list->rows.clear();
        long long ms = 0, bytes = 0;
        for (int id : shown) {
            const Track &t = *m_library.track(id);
            std::vector<std::string> row{std::string()};
            for (int f : m_columnFields)
                row.push_back(cellText(t, f));
            m_list->rows.push_back(std::move(row));
            ms += t.totalTime;
            bytes += t.size;
        }
        markPlaying();
        m_list->reload();
        if (m_viewMode == 2 && !eleven()) {
            m_flow->setSongs(m_list->ids);
            if (m_list->selected >= 0)
                m_flow->showSong(m_list->ids[size_t(m_list->selected)]);
        }
        m_bottom->totals = totals(shown.size(), ms, bytes);
        m_bottom->setNeedsDisplay();
    }

    // The browser's columns from the songs, each filtered by the choices left of it; the songs that
    // pass all three (an empty field is in All only)
    std::vector<int> browse(const std::vector<int> &ids)
    {
        static const char *const counts[3][2] = {{"166.001", "166.002"}, {"166.003", "166.004"}, {"166.005", "166.006"}};
        auto field = [&](const Track &t, int i) -> const std::string & { return i == 0 ? t.genre : i == 1 ? t.artist : t.album; };
        std::vector<int> pass = ids;
        for (int i = 0; i < 3; ++i) {
            if (i == 0 && !m_browseGenre) {
                m_browseSel[0].clear();
                continue;
            }
            std::vector<std::string> values;
            for (int id : pass) {
                const std::string &v = field(*m_library.track(id), i);
                if (!v.empty())
                    values.push_back(v);
            }
            std::sort(values.begin(), values.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
            values.erase(std::unique(values.begin(), values.end()), values.end());
            if (!m_browseSel[i].empty() && std::find(values.begin(), values.end(), m_browseSel[i]) == values.end())
                m_browseSel[i].clear();
            TrackList *b = m_browse[i];
            b->rows.clear();
            std::string all = tr(counts[i][values.size() == 1 ? 0 : 1]);
            const size_t at = all.find("%1");
            if (at != std::string::npos)
                all.replace(at, 2, std::to_string(values.size()));
            b->rows.push_back({all});
            b->selectOnly(0);
            for (const std::string &v : values) {
                if (v == m_browseSel[i])
                    b->selectOnly(int(b->rows.size()));
                b->rows.push_back({v});
            }
            b->reload();
            if (!m_browseSel[i].empty()) {
                std::vector<int> next;
                for (int id : pass)
                    if (field(*m_library.track(id), i) == m_browseSel[i])
                        next.push_back(id);
                pass = next;
            }
        }
        return pass;
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
        updateCells(id, 22);
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
        if (m_mini11)
            m_mini11->setPlaying(m_player.isPlaying());
        if (m_list->playingPaused != m_player.isPaused()) {
            m_list->playingPaused = m_player.isPaused();
            m_list->body()->setNeedsDisplay();
        }
        m_lcd->showing = active;
        if (active) {
            const Track &t = *m_library.track(m_current);
            if (t.path != m_artworkPath) {
                m_artworkPath = t.path;
                m_lcd->artwork = eleven() ? Image::fromData(artworkData(t.path), {128, 128}) : Image();
            }
            m_lcd->setTrack(t.name, t.artist, t.album);
            if (m_visualizer)
                m_visualizer->setTrack(t.name, t.artist);
            if (m_fullVisualizer)
                m_fullVisualizer->setTrack(t.name, t.artist);
            if (m_upNext && t.path != m_upNextPath) {
                m_upNextPath = t.path;
                refreshUpNext();
            }
            if (m_mini11)
                m_mini11->setSong(t.name, t.artist.empty() ? t.album : t.artist + " — " + t.album, m_lcd->artwork);
            m_lcd->position = m_player.position();
            m_lcd->duration = m_player.duration() > 0 ? m_player.duration() : t.totalTime / 1000.0;
        }
        m_lcd->setNeedsDisplay();
    }

    // ---- editing

    // Two names as FUN_0028ec76 compares them with flags 0x1B: leading spaces and controls skipped (2),
    // the case ignored (1), the accents ignored (8: UCCompareText's diacritic-insensitive), digits as
    // numbers (0x10: the digit masks 0x38000); -1, 0, 1
    static int compareNames(const std::string &a, const std::string &b)
    {
        auto fold = [](const std::string &s) {
            std::u32string out;
            size_t i = 0;
            while (i < s.size() && static_cast<unsigned char>(s[i]) < 0x21)
                ++i;
            for (; i < s.size();) {
                unsigned char c = static_cast<unsigned char>(s[i]);
                char32_t u = c;
                int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                if (n > 1) {
                    u = c & (0x3F >> (n - 1));
                    for (int k = 1; k < n && i + size_t(k) < s.size(); ++k)
                        u = (u << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
                }
                i += size_t(n);
                // Latin-1's accented letters to their base letter
                static const char *const base = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
                if (u >= 0xC0 && u <= 0xFF)
                    u = char32_t(static_cast<unsigned char>(base[u - 0xC0]));
                if (u >= 'a' && u <= 'z')
                    u -= 32;
                out.push_back(u);
            }
            return out;
        };
        const std::u32string x = fold(a), y = fold(b);
        size_t i = 0, j = 0;
        while (i < x.size() && j < y.size()) {
            if (x[i] >= '0' && x[i] <= '9' && y[j] >= '0' && y[j] <= '9') {
                size_t ie = i, je = j;
                while (ie < x.size() && x[ie] >= '0' && x[ie] <= '9')
                    ++ie;
                while (je < y.size() && y[je] >= '0' && y[je] <= '9')
                    ++je;
                std::u32string nx = x.substr(i, ie - i), ny = y.substr(j, je - j);
                nx.erase(0, std::min(nx.find_first_not_of(U'0'), nx.size() - 1));
                ny.erase(0, std::min(ny.find_first_not_of(U'0'), ny.size() - 1));
                if (nx.size() != ny.size())
                    return nx.size() < ny.size() ? -1 : 1;
                if (nx != ny)
                    return nx < ny ? -1 : 1;
                i = ie;
                j = je;
                continue;
            }
            if (x[i] != y[j])
                return x[i] < y[j] ? -1 : 1;
            ++i;
            ++j;
        }
        return x.size() - i == y.size() - j ? 0 : (x.size() - i < y.size() - j ? -1 : 1);
    }
    // Delete a playlist (FUN_00165bec): asked first, STR# 135.002 "Are you sure you want to delete the
    // playlist “^1”?", with ⌥ (the modifiers' bit 11, FUN_003ae4b0; not for a folder) 135.003 "… and all
    // contents from your iTunes library?", a folder 135.004 with 135.005 under it; Delete / Cancel
    // (23987.015 / .004) and "Do not ask me again" (23987.019; kept at +0x9DC, +0xC09, +0x300B: not
    // shown here). Not asked with ⌘ (bit 8), nor for a playlist with no songs (not a smart one) or a
    // folder with nothing in it. ⌥ deletes the songs from the library too (FUN_00454edc, FUN_000ea56a);
    // a folder goes with all it holds (FUN_000dfeb0)
    void deletePlaylist(unsigned mods)
    {
        const int tag = selectedPlaylist();
        if (tag < 0 || size_t(tag) >= m_library.playlists().size())
            return;
        const Playlist &p = m_library.playlists()[size_t(tag)];
        const bool withSongs = !p.folder && (mods & Option);
        const std::string id = p.persistentId;
        bool empty;
        if (p.folder)
            empty = std::none_of(m_library.playlists().begin(), m_library.playlists().end(), [&](const Playlist &q) { return q.parentId == id; });
        else
            empty = !p.smart && p.items.empty();
        auto doDelete = [this, id, withSongs] {
            auto &lists = m_library.playlists();
            auto at = std::find_if(lists.begin(), lists.end(), [&](const Playlist &q) { return q.persistentId == id; });
            if (at == lists.end())
                return;
            if (withSongs) {
                const std::vector<int> songs = at->smart ? m_library.evaluate(at->info) : at->items;
                for (int song : songs) {
                    if (song == m_current) {
                        m_player.stop();
                        m_current = -1;
                    }
                    m_library.remove(song);
                }
            }
            // the playlist and, for a folder, all in it
            std::set<std::string> gone{id};
            for (bool more = true; more;) {
                more = false;
                for (const Playlist &q : lists)
                    if (!q.parentId.empty() && gone.count(q.parentId) && gone.insert(q.persistentId).second)
                        more = true;
            }
            const int row = m_sources->selected();
            lists.erase(std::remove_if(lists.begin(), lists.end(), [&](const Playlist &q) { return !q.master && gone.count(q.persistentId); }),
                        lists.end());
            m_library.save();
            rebuildSources();
            // the row that took its place, else the one above (by eye)
            int next = std::min(row, int(m_sources->rows.size()) - 1);
            while (next > 0 && !m_sources->rows[size_t(next)].selectable)
                --next;
            m_sources->select(next >= 0 ? next : 1, true);
            layout();
        };
        if ((mods & Command) || empty) {
            doDelete();
            return;
        }
        auto withName = [&](std::string t) {
            const size_t at = t.find("^1");
            if (at != std::string::npos)
                t.replace(at, 2, p.name);
            return t;
        };
        const std::string message = withName(tr(p.folder ? "135.004" : withSongs ? "135.003" : "135.002"));
        const std::string informative = p.folder ? tr("135.005") : std::string();
        Alert::run(Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/iTunes.icns"), {64, 64}), message, informative,
                   {tr("23987.015"), tr("23987.004")}, [doDelete](int b) {
                       if (b == 0)
                           doDelete();
                   });
    }
    // A folder is `id` or in it (FUN_000db944): its parents up from it
    bool inside(const Playlist &folder, const std::string &id)
    {
        std::string at = folder.persistentId;
        for (int depth = 0; !at.empty() && depth < 32; ++depth) {
            if (at == id)
                return true;
            std::string up;
            for (const Playlist &q : m_library.playlists())
                if (q.persistentId == at)
                    up = q.parentId;
            at = up;
        }
        return false;
    }
    // File ▸ Library ▸ Export Playlist… (expr: FUN_000a8ef2's panel, STR# 149's types: Plain Text,
    // Unicode Text, XML; OPML for a podcast list) and Export Library… (expl: XML). The panel the
    // desktop's: the type by the name's extension, .xml XML, else Unicode Text
    void exportPlaylist(bool library)
    {
        const int tag = library ? -1 : selectedPlaylist();
        const Playlist *p = tag >= 0 && size_t(tag) < m_library.playlists().size() ? &m_library.playlists()[size_t(tag)] : nullptr;
        const std::string name = library ? "Library" : p ? p->name : tr("9000.036");
        std::vector<FileFilter> filters;
        if (!library)
            filters.push_back({tr("149.002"), {"*.txt"}});
        filters.push_back({tr("149.003"), {"*.xml"}});
        const std::string title = library ? "Export Library" : "Export";
        SavePanel::run(title, Library::musicFolder() + "/" + name + (library ? ".xml" : ".txt"), filters,
                       [this, library, tag, p](std::string path) {
                           if (path.empty())
                               return;
                           const bool xml = path.size() > 4 && lower(path.substr(path.size() - 4)) == ".xml";
                           std::vector<int> ids;
                           if (p)
                               ids = p->folder ? m_library.folderItems(*p) : p->smart && p->info.live ? m_library.evaluate(p->info) : p->items;
                           else
                               for (const auto &[id, t] : m_library.tracks())
                                   ids.push_back(id);
                           if (xml && library) {
                               m_library.saveTo(path, nullptr, nullptr);
                           } else if (xml) {
                               // the playlist's songs and the playlist alone (the master for the library)
                               std::set<int> songs(ids.begin(), ids.end());
                               std::vector<size_t> lists;
                               for (size_t i = 0; i < m_library.playlists().size(); ++i)
                                   if (int(i) == tag || (tag < 0 && m_library.playlists()[i].master))
                                       lists.push_back(i);
                               m_library.saveTo(path, &songs, &lists);
                           } else {
                               m_library.exportText(path, ids, true);
                           }
                       });
    }
    // File ▸ Library ▸ Import Playlist… (impt: FUN_000a9198's panel titled STR# 149.005 "Import"): a
    // playlist made of each list the file has (its name the file's, or its XML's playlists')
    void importPlaylist()
    {
        OpenPanel::run(tr("149.005"), Library::musicFolder(),
                       {{"Playlists", {"*.xml", "*.txt", "*.m3u", "*.m3u8", "*.pls"}}}, false, [this](std::vector<std::string> files) {
                           if (files.empty())
                               return;
                           std::vector<std::pair<std::string, std::vector<int>>> lists;
                           if (!m_library.importPlaylist(files.front(), lists))
                               return;
                           int last = -1;
                           for (auto &[name, songs] : lists) {
                               Playlist p;
                               p.name = name.empty() ? untitled("9000.001", "9000.002") : name;
                               p.items = songs;
                               p.persistentId = Library::newPersistentId();
                               m_library.playlists().push_back(p);
                               last = int(m_library.playlists().size()) - 1;
                           }
                           m_library.save();
                           rebuildSources();
                           for (size_t i = 0; i < m_sources->rows.size(); ++i)
                               if (m_sources->rows[i].tag == last)
                                   m_sources->select(int(i), true);
                           layout();
                       });
    }
    // Where a new playlist or folder goes (FUN_0013c60c, FUN_0014ec46's nfld): in the folder chosen,
    // beside the playlist chosen (in its folder), else at the top
    std::string newParent()
    {
        const int tag = selectedPlaylist();
        if (tag < 0 || size_t(tag) >= m_library.playlists().size())
            return {};
        const Playlist &p = m_library.playlists()[size_t(tag)];
        return p.folder ? p.persistentId : p.parentId;
    }
    // A new playlist or folder in the library, in its parent (opened to show it), and chosen
    void addPlaylist(Playlist p)
    {
        p.persistentId = Library::newPersistentId();
        p.parentId = newParent();
        if (!p.parentId.empty()) {
            m_openFolders.insert(p.parentId);
            Settings("itunes").set("OpenFolders", std::vector<std::string>(m_openFolders.begin(), m_openFolders.end()));
        }
        m_library.playlists().push_back(p);
        m_library.save();
        rebuildSources();
        const int tag = int(m_library.playlists().size()) - 1;
        for (size_t i = 0; i < m_sources->rows.size(); ++i)
            if (m_sources->rows[i].tag == tag) {
                m_sources->select(int(i), true);
                m_sources->startRename(int(i), 1.0 / 60);   // FUN_00166d0a → FUN_00166b90 with 1 tick
            }
        layout();
    }
    // A name not taken: "untitled playlist", then "untitled playlist 2"… (FUN_000dbb94 with STR# 9000's
    // pair: .001 / .002 for a playlist, .003 / .004 for a folder)
    std::string untitled(const char *first, const char *numbered)
    {
        auto taken = [&](const std::string &name) {
            return std::any_of(m_library.playlists().begin(), m_library.playlists().end(), [&](const Playlist &q) { return q.name == name; });
        };
        std::string name = tr(first);
        for (int n = 2; taken(name); ++n) {
            name = tr(numbered);
            const size_t at = name.find("^1");
            if (at != std::string::npos)
                name.replace(at, 2, std::to_string(n));
        }
        return name;
    }
    void newPlaylist()
    {
        Playlist p;
        p.name = untitled("9000.001", "9000.002");
        addPlaylist(p);
    }
    // File ▸ New Playlist Folder (nfld; FUN_002e5f90): "untitled folder", its icon 441's row 20 (+0x4B6)
    void newFolder()
    {
        Playlist p;
        p.name = untitled("9000.003", "9000.004");
        p.folder = true;
        addPlaylist(p);
    }

    // Advanced ▸ Open Audio Stream… (FUN_000b24d6): Get Long String (DLOG 10003) titled STR# 130.001
    // "Open Audio Stream" (130.002 "Edit URL" when it has a text), its prompt 130.003 "URL:"; the URL
    // trimmed, then FUN_000b21c4 by its scheme (FUN_00261b94):
    //   itms: / itmss: (FUN_000fa2f4) the Store's page (FUN_0022cf82), unless an .m3u or .pls (then http)
    //   pcast: / itpc: http, and a podcast subscribed to (FUN_003cd11c): not ported
    //   else a stream: the library's URL track (FUN_000e29c6 → FUN_0017fd64), played (FUN_000c30b8)
    // A URL ending in a media or playlist extension (FUN_00262270: mp3, m4a, m3u, pls…) goes through
    // FUN_003cd11c in iTunes; here libmpv opens those as streams too.
    void openStream()
    {
        iTunes::getString(m_askWindow, 10003, tr("130.001"), tr("130.003"), {}, [this](std::string url) {
            const size_t from = url.find_first_not_of(" \t\r\n"), to = url.find_last_not_of(" \t\r\n");
            if (from == std::string::npos)
                return;
            url = url.substr(from, to - from + 1);
            const size_t colon = url.find(':');
            const std::string scheme = colon == std::string::npos ? std::string() : lower(url.substr(0, colon));
            const size_t dot = url.find_last_of("./?");
            const std::string ext = dot == std::string::npos || url[dot] != '.' ? std::string() : lower(url.substr(dot + 1));
            if ((scheme == "itms" || scheme == "itmss") && ext != "m3u" && ext != "pls") {
                if (m_store) {
                    m_sources->select(3, true);   // the iTunes Store source
                    m_store->load("https" + url.substr(colon));
                }
                return;
            }
            if (scheme == "pcast" || scheme == "itpc") {
                Alert::run(Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/iTunes.icns"), {64, 64}),
                           tr("131.001"), {}, {"OK"}, [](int) {});
                return;
            }
            if (scheme == "itms" || scheme == "itmss")
                url = "http" + url.substr(colon);
            const int id = m_library.addURL(url);
            m_library.save();
            loadSource();
            const auto row = std::find(m_list->ids.begin(), m_list->ids.end(), id);
            if (row != m_list->ids.end()) {
                m_list->selectOnly(int(row - m_list->ids.begin()));
                m_list->scrollToRow(m_list->selected);
                playRow(m_list->selected);
            } else
                playTrack(id);
        });
    }

    // File ▸ Show Duplicates / Show Exact Duplicates (⌥), or Show All (FUN_00150918, FUN_0014eae4,
    // FUN_00138cea): the list sorted by Name while they are shown, its sort before put back after
    void showDuplicates(bool exact)
    {
        int &mode = m_dups[selectedPlaylist()];
        if (mode) {
            mode = 0;
            m_sortField = m_sortBeforeDups;
            m_list->ascending = m_ascBeforeDups;
        } else {
            mode = exact ? 2 : 1;
            m_sortBeforeDups = m_sortField;
            m_ascBeforeDups = m_list->ascending;
            m_sortField = 2;
            m_list->ascending = true;
        }
        const auto c = std::find(m_columnFields.begin(), m_columnFields.end(), m_sortField);
        m_list->sortColumn = c == m_columnFields.end() ? -1 : int(c - m_columnFields.begin()) + 1;
        loadSource();
    }
    int selectedPlaylist() const
    {
        const int sel = m_sources->selected();
        return sel >= 0 && size_t(sel) < m_sources->rows.size() ? m_sources->rows[size_t(sel)].tag : -1;
    }
    // File ▸ New Smart Playlist… (nspl), Edit Smart Playlist (esmp)
    void newSmartPlaylist()
    {
        m_smartWindow = std::make_unique<SmartPlaylistWindow>(SmartInfo{}, [this](const SmartInfo &info) {
            Playlist p;
            p.name = untitled("9000.001", "9000.002");
            p.smart = true;
            p.info = info;
            p.items = m_library.evaluate(info);
            addPlaylist(p);
        });
        m_smartWindow->show();
    }
    void editSmartPlaylist()
    {
        const int tag = selectedPlaylist();
        if (tag < 0 || !m_library.playlists()[size_t(tag)].smart)
            return;
        m_smartWindow = std::make_unique<SmartPlaylistWindow>(m_library.playlists()[size_t(tag)].info, [this, tag](const SmartInfo &info) {
            Playlist &p = m_library.playlists()[size_t(tag)];
            p.info = info;
            p.items = m_library.evaluate(info);
            m_library.save();
            loadSource();
        });
        m_smartWindow->show();
    }

    // The songs chosen, in the list's order
    std::vector<int> selectedIds() const
    {
        std::vector<int> ids;
        for (int r : m_list->sel)
            if (r >= 0 && size_t(r) < m_list->ids.size())
                ids.push_back(m_list->ids[size_t(r)]);
        return ids;
    }

    void newPlaylistFromSelection()
    {
        const std::vector<int> ids = selectedIds();
        if (ids.empty())
            return;
        newPlaylist();
        for (int id : ids)
            m_library.playlists().back().items.push_back(id);
        m_library.save();
        loadSource();
    }

    void deleteSelection()
    {
        const int sel = m_sources->selected();
        const int tag = sel >= 0 ? m_sources->rows[size_t(sel)].tag : -1;
        // a smart playlist's songs are its rules': nothing to take out of it
        const std::vector<int> ids = selectedIds();
        if (ids.empty() || (tag >= 0 && m_library.playlists()[size_t(tag)].smart))
            return;
        for (int id : ids) {
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
        }
        m_list->selectNone();
        m_library.save();
        loadSource();
    }

    // File ▸ Rating, a click on the stars: the song's rating, kept in the library
    void rate(int id, int rating)
    {
        Track *t = m_library.track(id);
        if (!t)
            return;
        t->rating = rating;
        m_library.save();
        updateCells(id, 25);
        m_list->body()->setNeedsDisplay();
    }

    // A song's cell of a field, again (its play count, its rating)
    void updateCells(int id, int field)
    {
        const auto col = std::find(m_columnFields.begin(), m_columnFields.end(), field);
        const Track *t = m_library.track(id);
        if (col == m_columnFields.end() || !t)
            return;
        const size_t c = size_t(col - m_columnFields.begin()) + 1;
        for (size_t i = 0; i < m_list->ids.size(); ++i)
            if (m_list->ids[i] == id && m_list->rows[i].size() > c)
                m_list->rows[i][c] = cellText(*t, field);
    }

    // The list's columns from the fields chosen (View Options), the sorted one kept
    void applyColumns()
    {
        m_list->columns = {{"", 45, Align::Left}};
        m_list->ratingColumn = -1;
        for (int f : m_columnFields) {
            const ListField *lf = listField(f);
            if (f == 25)
                m_list->ratingColumn = int(m_list->columns.size());
            m_list->columns.push_back({tr(lf->titleKey), lf->width, lf->align});
        }
        const auto s = std::find(m_columnFields.begin(), m_columnFields.end(), m_sortField);
        if (s == m_columnFields.end()) {
            m_sortField = 2;
            m_list->ascending = true;
        }
        m_list->sortColumn = int(std::find(m_columnFields.begin(), m_columnFields.end(), m_sortField) - m_columnFields.begin()) + 1;
    }

    // View ▸ View Options… (VOpt)
    void showViewOptions()
    {
        const int sel = m_sources->selected();
        const SourceRow row = sel >= 0 && size_t(sel) < m_sources->rows.size() ? m_sources->rows[size_t(sel)] : SourceRow{};
        m_viewOptions = std::make_unique<ViewOptionsWindow>(row.title, std::max(0, row.icon), m_columnFields, [this](const std::vector<int> &cols) {
            m_columnFields = cols;
            std::string s;
            for (int f : cols)
                s += (s.empty() ? "" : " ") + std::to_string(f);
            Settings("itunes").set("Columns", s);
            applyColumns();
            loadSource();
        });
        m_viewOptions->show();
    }

    // The preferences the port acts on (section 14): the list's text size, Sound Check, the MiniPlayer
    // on top
    void applyPreferences()
    {
        if (eleven())
            return;
        Settings s("itunes");
        g_listText = s.integer("ListTextLarge", 0) == 1 ? 9003 : 9002;
        m_list->rowHeight = listRowHeight();
        m_list->reload();
        m_list->body()->setNeedsDisplay();
        m_player.setSoundCheck(s.boolean("SoundCheck", false));
        m_window->setFloating(m_mini && s.boolean("MiniOnTop", false));
    }

    // Cover Flow's full screen button (`full`, PBUT 2020; AlbumCoverDisplay's mode flag 1): the covers
    // over the whole screen, the scroller 57 from the foot; the button again or Escape ends it
    void fullScreenCoverFlow()
    {
        if (m_fullFlow) {
            m_fullFlowWindow.reset();
            m_fullFlow = nullptr;
            return;
        }
        const Rect screen = App::shared().screenFrame();
        m_fullFlowWindow = std::make_unique<Window>(Size{screen.w, screen.h}, "Cover Flow", Window::FullScreen);
        m_fullFlow = m_fullFlowWindow->contentView()->add<CoverFlowView>(m_library);
        m_fullFlow->fullScreen = true;
        m_fullFlow->setFrame({0, 0, screen.w, screen.h});
        m_fullFlow->setSongs(m_list->ids);
        m_fullFlow->onPlay = m_grid->onPlay;
        m_fullFlow->onChoose = m_flow->onChoose;
        m_fullFlow->onFullScreen = [this] { fullScreenCoverFlow(); };
        if (m_current >= 0)
            m_fullFlow->showSong(m_current);
        m_fullFlowWindow->makeFirstResponder(m_fullFlow);
        m_fullFlowWindow->show();
    }

    // View ▸ Full Screen (`full`, ⌘F) with the visualizer on: Magnetosphere over the whole screen, a
    // click or a key back to the window
    void fullScreenVisualizer()
    {
        if (m_fullVisualizer) {
            App::shared().later([this] {
                m_fullVisualizer->setRunning(false);
                m_fullWindow.reset();
                m_fullVisualizer = nullptr;
                if (m_visualizerShown)
                    m_visualizer->setRunning(true);
            });
            return;
        }
        const Rect screen = App::shared().screenFrame();
        m_fullWindow = std::make_unique<Window>(Size{screen.w, screen.h}, "Visualizer", Window::FullScreen);
        m_fullVisualizer = m_fullWindow->contentView()->add<Visualizer>(m_player);
        m_fullVisualizer->setFrame({0, 0, screen.w, screen.h});
        m_fullVisualizer->onDismiss = [this] { fullScreenVisualizer(); };
        m_visualizer->setRunning(false);
        m_fullVisualizer->setRunning(true);
        m_fullWindow->makeFirstResponder(m_fullVisualizer);
        m_fullWindow->show();
    }

    void showVisualizer(bool on)
    {
        if (!m_visualizer)
            return;
        m_visualizerShown = on && !m_mini;
        m_visualizer->setHidden(!m_visualizerShown);
        m_visualizer->setRunning(m_visualizerShown);
        // its keys (M, P, I, C, F, N, L, ?) go to it
        if (m_visualizerShown)
            m_window->makeFirstResponder(m_visualizer);
        layout();
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
                m_list->selectOnly(int(i));
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
        const bool hasSel = !m_list->sel.empty();
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
            return storeShown() && m_store->canGoBack();
        if (m_store && a == "Msgf")
            return storeShown() && m_store->canGoForward();
        if (a == "stop")   // 11.4's Controls ▸ Stop
            return !m_player.isStopped();
        if (a == "volu" || a == "vold" || a == "mute" || a == "TFnd" || a == "add " || a == "nply" || a == "minw" ||
            a == "zoom" || a == "smin" || a == "minp" || (eleven() && (a == "PQsh" || a == "PQcl")) || a == "brwn" || a == "batf" || a == "abou" || a == "terminate:" || a == "hide:" ||
            a == "hideOtherApplications:" || a == "unhideAllApplications:" || a == "clos" || a == "CLOS")
            return true;
        if (a == "eqwn")   // Window ▸ Equalizer: 8.2.1's EQWindow
            return !eleven();
        if (a.size() == 4 && a.rfind("SRT", 0) == 0 && a[3] >= '0' && a[3] <= '5') {   // File ▸ Rating
            const Track *t = hasSel && m_list->selected >= 0 ? m_library.track(m_list->ids[size_t(m_list->selected)]) : nullptr;
            v.checked = t && (t->rating + 10) / 20 == a[3] - '0';
            return t != nullptr;
        }
        if (a == "SRAT")
            return hasSel;
        if (a == "sall")   // Edit ▸ Select All, Select None: the songs' list (FUN_001b0ec2)
            return dynamic_cast<TextField *>(m_window->firstResponder()) || (!m_store8 && !m_list->rows.empty());
        if (a == "SNon")
            return !m_store8 && !m_list->sel.empty();
        if (a == "ourl")   // Advanced ▸ Open Audio Stream… (enabled unless +0x109bf)
            return !eleven();
        if (a == "SDup") {   // FUN_00140f50: "Show All" while they are shown
            const auto d = m_dups.find(selectedPlaylist());
            if (d != m_dups.end() && d->second)
                v.title = tr("kMenuItemShowAll");
            return !eleven() && !m_store8;
        }
        if (a == "nspl" || a == "nfld" || a == "impt" || a == "expl")
            return !eleven();
        if (a == "expr")   // Export Playlist…: the source chosen (the library or a playlist)
            return !eleven() && !m_store8;
        if (a == "full")
            return !eleven() && ((m_visualizer && m_visualizerShown) || m_viewMode == 2);
        if (a == "VisM") {   // View ▸ Visualizer ▸ iTunes Visualizer: the one there is
            v.checked = true;
            return !eleven();
        }
        if (a == "VisC" || a == "VisO")   // the classic visualizer and the options: not ported
            return false;
        if (a == "vtog") {   // View ▸ Show / Hide Visualizer
            v.title = tr(m_visualizerShown ? "kMenuItemHideVisualizer" : "kMenuItemShowVisualizer");
            return m_visualizer && !m_mini;
        }
        if (a == "pref")   // iTunes ▸ Preferences…: 8.2.1's
            return !eleven();
        if (a == "VOpt")
            return !eleven() && m_viewMode == 0;
        if (a == "esmp") {
            const int tag = selectedPlaylist();
            return !eleven() && tag >= 0 && m_library.playlists()[size_t(tag)].smart;
        }
        if (a == "geti")   // File ▸ Get Info: 8.2.1's Info Window
            return hasSel && !eleven();
        if (a == "SBro") {
            v.title = tr(m_showBrowser ? "kMenuItemHideBrowser" : "kMenuItemShowBrowser");
            return !eleven() && !m_mini && m_viewMode == 0;
        }
        if (a == "clea" && m_window->firstResponder() == m_sources)   // the playlist chosen
            return selectedPlaylist() >= 0;
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
        if (a == "lisv" || a == "thmv" || a == "covv") {   // View ▸ as List, as Grid, as Cover Flow
            v.checked = (a == "lisv" ? 0 : a == "thmv" ? 1 : 2) == m_viewMode;
            return a == "lisv" || !eleven();
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
            if (m_nav && !m_nav->store)
                setStoreMode(true);
            else if (!m_nav && !m_store8)
                m_sources->select(3, true);   // 8.2.1: the iTunes Store source
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
        } else if (a == "zoom") {
            zoom();
        } else if (a == "PQsh") {
            showUpNext();
        } else if (a == "PQcl") {
            clearUpNext();
        } else if (a == "smin" || a == "minp") {
            switchToMini11(!(m_mini11 && m_mini11->isShown()));
        } else if (a == "TFnd")
            m_window->makeFirstResponder(m_search);
        else if (a == "add ")
            addToLibrary();
        else if (a == "nply")
            newPlaylist();
        else if (a == "npls")
            newPlaylistFromSelection();
        else if (a == "clea" && m_window->firstResponder() == m_sources)
            deletePlaylist(0);
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
        else if (a == "eqwn")
            m_equalizer.show();
        else if (a == "SBro")
            toggleBrowser();
        else if (a == "lisv" || a == "thmv" || a == "covv")
            setViewMode(a == "lisv" ? 0 : a == "thmv" ? 1 : 2);
        else if (a == "ourl")
            openStream();
        else if (a == "sall" && dynamic_cast<TextField *>(m_window->firstResponder())) {
            auto *f = dynamic_cast<TextField *>(m_window->firstResponder());   // the search field's text
            f->selectAll();
            f->setNeedsDisplay();
        } else if (a == "sall" || a == "SNon") {
            if (a == "sall")
                m_list->selectAll();
            else
                m_list->selectNone();
            m_list->body()->setNeedsDisplay();
            m_list->setNeedsDisplay();
            if (m_list->onSelect)
                m_list->onSelect(m_list->selected);
        }
        else if (a == "SDup")
            showDuplicates(it.alternate);
        else if (a == "nspl")
            newSmartPlaylist();
        else if (a == "nfld")
            newFolder();
        else if (a == "expr")
            exportPlaylist(false);
        else if (a == "expl")
            exportPlaylist(true);
        else if (a == "impt")
            importPlaylist();
        else if (a == "VOpt")
            showViewOptions();
        else if (a == "vtog")
            showVisualizer(!m_visualizerShown);
        else if (a == "full") {
            if (m_visualizerShown)
                fullScreenVisualizer();
            else if (m_viewMode == 2)
                fullScreenCoverFlow();
        }
        else if (a == "pref") {
            m_prefs.onApply = [this] { applyPreferences(); };
            m_prefs.show();
        }
        else if (a == "esmp")
            editSmartPlaylist();
        else if (a == "geti" && m_list->sel.size() > 1) {   // several songs: FUN_000e9042
            m_multiInfo.onChanged = [this] { loadSource(); refreshLCD(); };
            m_multiInfo.show(selectedIds());
        }
        else if (a == "geti" && m_list->selected >= 0) {
            m_info.onChanged = [this](int) { loadSource(); refreshLCD(); };
            m_info.show(m_list->ids, m_list->selected);
        }
        else if (a.size() == 4 && a.rfind("SRT", 0) == 0)
            for (int id : selectedIds())   // File ▸ Rating: every song chosen
                rate(id, (a[3] - '0') * 20);
        else if (a == "terminate:")
            App::shared().quit();
    }

    Library m_library;
    Player m_player;
    Equalizer m_equalizer{m_player};
    InfoWindow m_info{m_library};
    MultipleInfoWindow m_multiInfo{m_library};
    std::unique_ptr<SmartPlaylistWindow> m_smartWindow;
    std::unique_ptr<ViewOptionsWindow> m_viewOptions;
    PreferencesWindow m_prefs;
    Visualizer *m_visualizer = nullptr;
    std::unique_ptr<Window> m_fullWindow;
    Visualizer *m_fullVisualizer = nullptr;
    bool m_visualizerShown = false;
    std::vector<int> m_columnFields;      // the list's columns, by field (View Options)
    int m_sortField = 2;                  // the field the songs are sorted by
    // File ▸ Show Duplicates: per source (its playlist's tag, -1 the library), 1 the duplicates, 2 the
    // exact ones (the playlist's +0x182 flag 8); the sort it had before (+0x10c), put back by Show All
    std::set<std::string> m_openFolders;   // the folders open in the sources (their Persistent IDs)
    std::map<int, int> m_dups;
    std::unique_ptr<Window> m_askWindow;   // Open Audio Stream's Get Long String
    int m_sortBeforeDups = 2;
    bool m_ascBeforeDups = true;
    MainMenu m_menu;
    std::unique_ptr<Window> m_window, m_about;
    TopBand *m_top = nullptr;
    NavBar *m_nav = nullptr;
    StoreView *m_store = nullptr;
    StoreBar *m_storeBar = nullptr;   // 8.2.1's
    bool m_store8 = false;            // 8.2.1: the iTunes Store source chosen
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
    TrackList *m_browse[3] = {};          // Genre, Artist, Album
    BrowserSplitter *m_splitter = nullptr;
    GridView *m_grid = nullptr;
    int m_view11 = 0;   // 11.4's nav bar view: 0 Songs, 1 Albums, 2 Artists, 3 Genres
    CoverFlowView *m_flow = nullptr;
    std::unique_ptr<Window> m_fullFlowWindow;
    CoverFlowView *m_fullFlow = nullptr;
    int m_viewMode = 0;                   // 0 the list, 1 the grid
    bool m_showBrowser = false, m_browseGenre = true;
    double m_browserHeight = -1;          // the browser's height (+0x452), -1 until laid out
    double m_browserDragFrom = 0;
    std::string m_browseSel[3];           // the chosen genre, artist, album ("" all)
    double m_sourceWidth = eleven() ? 185 : 200;   // ACfg's Default Source Width (11.4's AppConfig.plist: 185)
    std::vector<int> m_queue;
    int m_queuePos = 0, m_current = -1;
    std::string m_artworkPath;
    bool m_shuffle = false;
    int m_repeat = 0;   // 0 off, 1 all, 2 one (432's rows)
    double m_muted = 0;
    bool m_mini = false;
    std::unique_ptr<MiniPlayer11> m_mini11;
    bool m_toMini11 = false;
    std::unique_ptr<UpNext11> m_upNext;
    std::string m_upNextPath;
    Size m_fullSize{1000, 620 + TitleBarHeight};
    double m_miniWidth = 200;
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
    itunes.openFiles(files);
    Instance::listen("iTunes", [&itunes](std::vector<std::string> f) {
        itunes.openFiles(f);
        itunes.comeForward();
    });
    return app.run();
}
