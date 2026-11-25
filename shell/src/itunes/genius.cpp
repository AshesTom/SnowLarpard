// SPDX-License-Identifier: LGPL-2.1-or-later
#include "genius.h"

#include "controls.h"
#include "foundation.h"
#include "resources.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace iTunes {

namespace {

AK::TextStyle txts(int id, Align align = Align::Left)
{
    const iTunes::TextStyle s = textStyle(id);
    AK::TextStyle t;
    t.font = s.font.size > 0 ? s.font : Font::system(11);
    t.color = s.color;
    t.align = align;
    return t;
}

} // namespace

void GeniusBar::set(const std::string &song, const std::string &artist, int limit, bool canSave)
{
    m_song = song;
    m_artist = artist;
    m_limit = limit;
    m_canSave = canSave;
    setNeedsDisplay();
}

Rect GeniusBar::part(int which) const
{
    const double W = frame().w;   // bound to the right of the nib's 762-point pane
    switch (which) {
    case 1: return {W - (762 - 665), 4, 89, 17};
    case 2: return {W - (762 - 598), 4, 62, 17};
    default: return {W - (762 - 508), 4, 85, 17};
    }
}

void GeniusBar::draw(cairo_t *cr)
{
    const Image bg = picture(1200);
    if (!bg.isNull())
        bg.draw(cr, Rect{0, 0, bg.size().w, bg.size().h}, Rect{0, 0, frame().w, Height});
    fill(cr, Rect{0, Height, frame().w, frame().h - Height}, Color::gray(64));   // the line under it (by eye)
    // "Based On: <song> by <artist>", each after the one before it (their places in the nib by their
    // sample texts "xxx" and "yyy")
    double x = 9;
    const AK::TextStyle based = txts(1202), name = txts(1203), by = txts(1204);
    for (const auto &[text, style] : {std::pair<std::string, AK::TextStyle>{"Based On:", based}, {m_song, name}, {"by", by}, {m_artist, name}}) {
        if (text.empty())
            continue;
        const double w = std::ceil(textWidth(text, style.font));
        const double room = part(3).x - 60 - x;
        if (room <= 0)
            break;
        drawText(cr, text, Rect{x, 4, std::min(w, room), 13}, style);
        x += std::min(w, room) + 4;
    }
    drawText(cr, "Limit to:", Rect{frame().w - (762 - 453), 4, 53, 13}, txts(1201, Align::Right));
    // PBUT 1204 (the popup, three 25-point cells: 8 kept at the left, 16 at the right) and 1202 (the
    // buttons, three 17-point cells, 8 kept at each end), kind 3
    auto button = [&](int which, int pbut, const std::string &title, bool enabled) {
        const Rect r = part(which);
        drawCell(cr, pbut, stateColumn(3, !enabled ? 0 : m_pressed == which ? 2 : 1), 0, r);
        AK::TextStyle t = txts(1200, pbut == 1204 ? Align::Left : Align::Center);
        if (!enabled)
            t.color = Color{t.color.r, t.color.g, t.color.b, 0.5};
        drawText(cr, title, pbut == 1204 ? Rect{r.x + 8, r.y, r.w - 24, r.h} : Rect{r.x + 4, r.y, r.w - 8, r.h}, t);
    };
    button(3, 1204, std::to_string(m_limit) + " songs", true);
    button(2, 1202, "Refresh", true);
    button(1, 1202, "Save Playlist", m_canSave);
}

void GeniusBar::mouseDown(Event &e)
{
    m_pressed = 0;
    for (int i = 1; i <= 3; ++i)
        if (part(i).contains(e.location))
            m_pressed = i;
    if (m_pressed == 1 && !m_canSave)
        m_pressed = 0;
    setNeedsDisplay();
    if (m_pressed == 3) {
        std::vector<MenuItem> items;
        for (int n : {25, 50, 75, 100})
            items.emplace_back(std::to_string(n) + " songs", false, true, n == m_limit);
        const Rect r = part(3);
        Menu::popUp(window(), toWindow(Point{r.x, r.bottom()}), items, r.w, Font::system(11), [this](int c) {
            m_pressed = 0;
            setNeedsDisplay();
            if (c >= 0 && onLimit)
                onLimit(25 * (c + 1));
        });
    }
}

void GeniusBar::mouseUp(Event &e)
{
    const int was = m_pressed;
    if (was == 3)
        return;
    m_pressed = 0;
    setNeedsDisplay();
    if (was && part(was).contains(e.location) && onCommand)
        onCommand(was == 1 ? "SGPl" : "RGPl");
}

void GeniusSidebar::draw(cairo_t *cr)
{
    // the line left of it (the source list's divider's grey), then the empty page (white: by eye)
    fill(cr, bounds(), Color::gray(255));
    fill(cr, Rect{0, 0, 1, frame().h}, Color::gray(64));
}

void GeniusSidebar::mouseDown(Event &e)
{
    m_grab = e.location.x;
}

void GeniusSidebar::mouseDragged(Event &e)
{
    if (m_grab <= 3 && onResize)
        onResize(toWindow(e.location).x);
}

void GeniusService::turnOn(std::function<void(const std::string &, const std::string &)> failed)
{
    if (m_task)
        return;
    // the bag (FUN_00034a3a: http://ax.init.itunes.apple.com/bag.xml?ix=2, a signed plist whose "bag" is
    // a plist in base64), its genius-welcome, and that page asked as iTunes asks the Store's pages
    static const char *const script =
        "ua='iTunes/8.2.1 (Macintosh; U; Intel Mac OS X 10.6)'\n"
        "bag=$(curl -sSL --max-time 30 -A \"$ua\" 'http://ax.init.itunes.apple.com/bag.xml?ix=2') || exit 2\n"
        "data=$(printf '%s' \"$bag\" | tr -d ' \\n\\t\\r' | sed -n 's:.*<key>bag</key><data>\\([^<]*\\)</data>.*:\\1:p' | base64 -d 2>/dev/null)\n"
        "[ -n \"$data\" ] || data=$bag\n"
        "url=$(printf '%s' \"$data\" | tr -d '\\n\\t\\r' | sed -n 's:.*<key>genius-welcome</key> *<string>\\([^<]*\\)</string>.*:\\1:p')\n"
        "[ -n \"$url\" ] || exit 3\n"
        "curl -sSL --max-time 30 -A \"$ua\" \"$url\"\n";
    m_task = std::make_unique<Task>();
    m_task->onExit = [this, failed](int status, const std::string &out) {
        App::shared().later([this] { m_task.reset(); });
        // Apple's answer: a Goto to showUpgradePage (the page asking for a newer iTunes, itself gone):
        // iTunes' own words for it, STR# 9009.016 and .017; anything else, .024 and .022
        if (status == 0 && out.find("showUpgradePage") != std::string::npos)
            failed(tr("9009.016"), tr("9009.017"));
        else
            failed(tr("9009.024"), tr("9009.022"));
    };
    if (!m_task->launch({"sh", "-c", script})) {
        m_task.reset();
        failed(tr("9009.024"), tr("9009.022"));
    }
}

} // namespace iTunes
