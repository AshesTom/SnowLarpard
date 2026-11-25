// SPDX-License-Identifier: LGPL-2.1-or-later
#include "podcastui.h"

#include "resources.h"

#include <cmath>

using namespace AK;

namespace iTunes {

// "Podcast Directory" and "Report a Concern": small text (TXTS 544) with the link arrow (PBUT 650, its
// PNG 250's 12 × 12 cells) right of it
class PodcastPlacard::Link : public View {
public:
    Link(std::string text, std::string command) : m_text(std::move(text)), m_command(std::move(command)) {}
    std::function<void(const std::string &)> *target = nullptr;
    void draw(cairo_t *cr) override
    {
        AK::TextStyle t;
        t.font = textStyle(544).font.size > 0 ? textStyle(544).font : Font::system(11);
        t.color = Color::gray(m_pressed ? 0 : 40);
        t.align = Align::Right;
        drawText(cr, m_text, Rect{0, 0, frame().w - 19, frame().h}, t);
        const Image arrow = picture(250);
        if (!arrow.isNull())
            arrow.draw(cr, Rect{m_pressed ? 12.0 : 0.0, 0, 12, 12}, Rect{frame().w - 12, std::floor((frame().h - 12) / 2) + 1, 12, 12});
    }
    void mouseDown(Event &) override
    {
        m_pressed = true;
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        m_pressed = false;
        setNeedsDisplay();
        if (bounds().contains(e.location) && target && *target)
            (*target)(m_command);
    }

private:
    std::string m_text, m_command;
    bool m_pressed = false;
};

PodcastPlacard::PodcastPlacard()
{
    auto button = [this](const std::string &title, const char *command) {
        Button *b = add<Button>(title, Button::Textured);
        b->small = true;
        b->action = [this, command] {
            if (onCommand)
                onCommand(command);
        };
        return b;
    };
    m_unsubscribe = button(tr("163.081"), "PCSB");
    m_settings = button("Settings…", "PCSe");
    m_settings->setEnabled(false);   // the podcast's own settings: not done
    m_refresh = button("Refresh", "PCRB");
    m_directory = add<Link>("Podcast Directory", "STPD");
    m_directory->target = &onCommand;
}

void PodcastPlacard::setSubscribed(bool subscribed)
{
    m_unsubscribe->title = tr(subscribed ? "163.081" : "163.080");
    m_unsubscribe->setNeedsDisplay();
}

void PodcastPlacard::setPodcastChosen(bool chosen)
{
    m_unsubscribe->setEnabled(chosen);
}

void PodcastPlacard::setFrame(const Rect &r)
{
    View::setFrame(r);
    const double W = r.w;
    // the pane a point down, under the line (FUN_000249e4's 47 points)
    m_unsubscribe->setFrame({20, 13, 88, 22});
    m_settings->setFrame({118, 13, 80, 22});
    // bound to the right: as far from the right as in the nib's 594-point pane
    m_directory->setFrame({W - (594 - 300), 14, 485 - 300, 18});   // its text right-aligned on 366…468
    m_refresh->setFrame({W - (594 - 500), 13, 76, 22});
}

void PodcastPlacard::draw(cairo_t *cr)
{
    const Image g = picture(1034);   // the placard gradient, 1 × 63, stretched (by eye: its top 46 rows)
    if (!g.isNull())
        g.draw(cr, Rect{0, 0, 1, std::min(Height + 1, g.size().h)}, bounds());
    else
        fill(cr, bounds(), Color::gray(220));
    fill(cr, Rect{0, 0, frame().w, 1}, Color::gray(150));
}

Rect podcastTriangle(const Rect &cell) { return {cell.x + 4, cell.y + std::floor((cell.h - 10) / 2), 10, 10}; }

Rect podcastButton(const Rect &cell, int button)
{
    const double w = button == GetAll ? 50 : 31;
    return {cell.x + cell.w - w - 4, cell.y + std::floor((cell.h - 13) / 2), w, 13};
}

void drawPodcastName(cairo_t *cr, const Rect &cell, const std::string &name, bool header, bool open, bool selected,
                     int button, bool pressed, const AK::TextStyle &style)
{
    if (header) {
        const Image tri = picture(314);
        if (!tri.isNull())
            tri.draw(cr, Rect{10, 10.0 * ((open ? 2 : 0) + (selected ? 3 : 0)), 10, 10}, podcastTriangle(cell));
    }
    double right = cell.w;
    if (button) {
        const Image b = localizedButton(button);
        const Rect at = podcastButton(cell, button);
        if (!b.isNull())
            b.draw(cr, Rect{pressed ? at.w : 0, 0, at.w, 13}, at);
        right = at.x - cell.x - 4;
    }
    drawText(cr, name, Rect{cell.x + 18, cell.y - 1, std::max(0.0, right - 22), cell.h}, style);
}

void drawPodcastStatus(cairo_t *cr, const Rect &cell, int column, bool selected)
{
    drawCell(cr, 306, column, selected ? 1 : 0, Rect{cell.x + std::floor((cell.w - 12) / 2), cell.y + std::floor((cell.h - 12) / 2), 12, 12});
}

} // namespace iTunes
