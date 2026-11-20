// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 11.4's MiniPlayer (docs/itunes-reverse.md section 17):
//   MiniPlayerFrameView (FUN_00a0ae3a, its vtable 0x1778578; FUN_00a0af9e builds it, FUN_00a0c95c draws
//   it): picture 8100 (300 × 48) behind it all, 8101 when the window isn't key, stretched; its content
//   pinned 5 from the left and 4 from the top.
//   MiniPlayerController (its vtable 0x17781b8; FUN_009f87d6 builds its views with ITView's layout
//   constraints: FUN_00976440 left, FUN_0097672c top, FUN_009765b6 right, FUN_00976a18 bottom,
//   FUN_00976b8e left and right, FUN_00978c36 one under another, FUN_00979bc4 width = height × m,
//   FUN_00979d36 the height): the artwork (picture 8117 requested 38 × 38; 8125 when there is none, in
//   8116's frame) pinned to the left, top and bottom, as wide as it is tall; the window 48 high; the
//   area right of it at least 220 wide (FUN_00978f34, priority 1000) and 240 by preference (740);
//   the buttons 9, 8 and 10 from its right. FUN_00a062da sets the pictures by the theme (light 81xx,
//   dark 81Ex): Play 8112 (three rows Play, Pause, Stop), Previous 8113, Next 8111, the close button
//   8110, the volume 8123 (four rows, quiet to loud) and AirPlay 8124, Up Next 8114 (its rows the
//   list and the Genius atom, plain and on), the search 8118, the expand button 8160.
// By eye: where the buttons are within the area (the transport centred, the volume at its left, Up
// Next at its right, close and expand at its top corners), the lines' styles (TextStyles' 302) and
// that the controls show while the pointer is over the window, the lines otherwise.
#include "miniplayer11.h"

#include "resources.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace iTunes {

namespace {
constexpr double Height = 48;         // FUN_00979d36's 48
constexpr double AreaMin = 220;       // FUN_00978f34
constexpr double AreaPreferred = 240; // priority 740
} // namespace

class MiniPlayer11::Body : public FrameBand {
public:
    explicit Body(MiniPlayer11 &owner) : m_owner(owner) {}
    std::string name, artist;
    Image artwork;
    bool playing = false, over = false;
    double volume = 1;

    void draw(cairo_t *cr) override
    {
        const double W = frame().w, H = frame().h;
        // FUN_00a0c95c: 8100, or 8101 in a window that isn't key, stretched over the view
        const Image bg = picture(windowIsKey() ? 8100 : 8101);
        if (!bg.isNull())
            bg.draw(cr, Rect{0, 0, W, H});
        // the artwork: square on the left, the whole height
        const Rect art{0, 0, H, H};
        if (!artwork.isNull())
            artwork.draw(cr, art);
        else {
            const Image none = picture(8125);
            if (!none.isNull())
                none.draw(cr, art);
        }
        const Rect area = this->area();
        if (over) {
            for (const Button &b : buttons())
                drawCell(cr, b.picture, stateColumn11(pictureInfo(b.picture).kind, m_pressed == b.id ? 1 : 0), b.row, b.rect);
            return;
        }
        // the two lines: the name, then the artist
        const iTunes::TextStyle lcd = textStyle(302);
        AK::TextStyle t;
        t.font = lcd.font;
        t.color = lcd.color;
        t.align = Align::Center;
        const double lh = std::ceil(lineHeight(t.font));
        drawText(cr, name, Rect{area.x + 6, H / 2 - lh, area.w - 12, lh}, t);
        t.color = Color::gray(0.35);
        drawText(cr, artist, Rect{area.x + 6, H / 2, area.w - 12, lh}, t);
    }
    void mouseMoved(Event &) override
    {
        if (!over) {
            over = true;
            setNeedsDisplay();
        }
    }
    void mouseExited(Event &) override
    {
        over = false;
        setNeedsDisplay();
    }
    void mouseDown(Event &e) override
    {
        for (const Button &b : buttons())
            if (over && b.rect.contains(e.location)) {
                m_pressed = b.id;
                setNeedsDisplay();
                return;
            }
        if (over && volumeRect().contains(e.location)) {
            setVolumeAt(e.location.x);
            m_pressed = VolumeId;
            return;
        }
        FrameBand::mouseDown(e);
    }
    void mouseDragged(Event &e) override
    {
        if (m_pressed == VolumeId)
            setVolumeAt(e.location.x);
        else if (m_pressed < 0)
            FrameBand::mouseDragged(e);
    }
    void mouseUp(Event &e) override
    {
        const int id = m_pressed;
        m_pressed = -1;
        setNeedsDisplay();
        for (const Button &b : buttons())
            if (b.id == id && b.rect.contains(e.location))
                fire(id);
    }

private:
    enum { CloseId, ExpandId, PrevId, PlayId, NextId, UpNextId, VolumeId };
    struct Button {
        int id, picture, row;
        Rect rect;
    };
    Rect area() const { return Rect{frame().h, 0, frame().w - frame().h, frame().h}; }
    Rect volumeRect() const
    {
        const Rect a = area();
        return Rect{a.x + 8, std::floor((a.h - 21) / 2), 21, 21};
    }
    std::vector<Button> buttons() const
    {
        const Rect a = area();
        const double H = frame().h;
        const Size play = cellSize(8112), side = cellSize(8113), small = cellSize(8110);
        const double cx = a.x + a.w / 2, cy = H / 2;
        std::vector<Button> v;
        v.push_back({CloseId, 8110, 0, Rect{a.x + 3, 3, small.w, small.h}});
        v.push_back({ExpandId, 8160, 0, Rect{a.right() - 9 - small.w, 3, small.w, small.h}});
        v.push_back({PrevId, 8113, 0, Rect{std::round(cx - play.w / 2 - 6 - side.w), std::round(cy - side.h / 2), side.w, side.h}});
        v.push_back({PlayId, 8112, playing ? 1 : 0, Rect{std::round(cx - play.w / 2), std::round(cy - play.h / 2), play.w, play.h}});
        v.push_back({NextId, 8111, 0, Rect{std::round(cx + play.w / 2 + 6), std::round(cy - side.h / 2), side.w, side.h}});
        const Size up = cellSize(8114);
        v.push_back({UpNextId, 8114, 0, Rect{a.right() - 10 - up.w, std::round(cy - up.h / 2), up.w, up.h}});
        // the volume's speaker: its row by the level, quiet to loud
        const int level = volume <= 0 ? 0 : volume < 0.34 ? 1 : volume < 0.67 ? 2 : 3;
        v.push_back({VolumeId, 8123, level, volumeRect()});
        return v;
    }
    void setVolumeAt(double)
    {
        // a click on the speaker steps through the four levels (by eye: 11.4 opens a slider)
        volume = volume >= 1 ? 0 : std::min(1.0, volume + 1.0 / 3);
        if (m_owner.m_hooks.setVolume)
            m_owner.m_hooks.setVolume(volume);
        setNeedsDisplay();
    }
    void fire(int id)
    {
        const Hooks &h = m_owner.m_hooks;
        auto call = [](const std::function<void()> &f) {
            if (f)
                f();
        };
        switch (id) {
        case CloseId: call(h.close); break;
        case ExpandId: call(h.expand); break;
        case PrevId: call(h.previous); break;
        case PlayId: call(h.playPause); break;
        case NextId: call(h.next); break;
        case UpNextId: call(h.upNext); break;
        }
    }
    MiniPlayer11 &m_owner;
    int m_pressed = -1;
};

MiniPlayer11::MiniPlayer11(Hooks hooks) : m_hooks(std::move(hooks)) {}

MiniPlayer11::~MiniPlayer11() = default;

void MiniPlayer11::show()
{
    if (!m_window) {
        m_window = std::make_unique<Window>(Size{Height + AreaPreferred, Height}, "MiniPlayer");
        m_window->setBorderless();
        m_window->setResizable({Height + AreaMin, Height}, {4000, Height});
        m_body = m_window->contentView()->add<Body>(*this);
        m_window->onResize = [this](Size s) { m_body->setFrame({0, 0, s.w, s.h}); };
        m_body->setFrame({0, 0, Height + AreaPreferred, Height});
    }
    m_window->show();
}

void MiniPlayer11::hide()
{
    if (m_window)
        m_window->close();
}

bool MiniPlayer11::isShown() const { return m_window && m_window->isVisible(); }

void MiniPlayer11::setSong(const std::string &name, const std::string &artist, const Image &artwork)
{
    if (!m_body)
        return;
    m_body->name = name;
    m_body->artist = artist;
    m_body->artwork = artwork;
    m_body->setNeedsDisplay();
}

void MiniPlayer11::setPlaying(bool playing)
{
    if (!m_body)
        return;
    m_body->playing = playing;
    m_body->setNeedsDisplay();
}

void MiniPlayer11::setVolume(double v)
{
    if (m_body)
        m_body->volume = v;
}

void MiniPlayer11::setFloating(bool on)
{
    if (m_window)
        m_window->setFloating(on);
}

} // namespace iTunes
