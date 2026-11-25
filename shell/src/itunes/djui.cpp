// SPDX-License-Identifier: LGPL-2.1-or-later
#include "djui.h"

#include "resources.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace iTunes {

namespace {

AK::TextStyle txts(int id, double fallbackSize, Align align = Align::Left)
{
    const iTunes::TextStyle s = textStyle(id);
    AK::TextStyle t;
    t.font = s.font.size > 0 ? s.font : Font::system(fallbackSize);
    t.color = s.color;
    t.align = align;
    return t;
}

void drawPlacardBackground(cairo_t *cr, const Rect &b, double height)
{
    const Image g = picture(1034);   // the placard gradient, 1 × 63, stretched (its top rows)
    if (!g.isNull())
        g.draw(cr, Rect{0, 0, 1, std::min(height, g.size().h)}, b);
    else
        fill(cr, b, Color::gray(220));
    fill(cr, Rect{0, 0, b.w, 1}, Color::gray(150));
}

} // namespace

// The source's popup: PBUT 786 (three 26-point cells, kind 3; 4 kept at the left and 12 at the right,
// the arrows there), its title in TXTS 510
class DJPlacard::SourcePopup : public View {
public:
    std::vector<std::string> names;
    int chosen = 0;
    std::function<void(int)> *target = nullptr;
    void draw(cairo_t *cr) override
    {
        drawCell(cr, 786, stateColumn(3, !isEnabled() ? 0 : m_pressed ? 2 : 1), 0, bounds());
        AK::TextStyle t = txts(510, 11);
        if (chosen >= 0 && size_t(chosen) < names.size())
            drawText(cr, names[size_t(chosen)], Rect{9, 0, frame().w - 9 - 16, frame().h - 1}, t);
    }
    void mouseDown(Event &) override
    {
        m_pressed = true;
        setNeedsDisplay();
        std::vector<MenuItem> items;
        for (size_t i = 0; i < names.size(); ++i)
            items.emplace_back(names[i], false, true, int(i) == chosen);
        Menu::popUp(window(), toWindow(Point{0, frame().h}), items, frame().w, Font::system(13), [this](int c) {
            m_pressed = false;
            setNeedsDisplay();
            if (c >= 0 && target && *target)
                (*target)(c);
        });
    }

private:
    bool m_pressed = false;
};

DJPlacard::DJPlacard()
{
    m_popup = add<SourcePopup>();
    m_popup->target = &onSource;
    auto button = [this](const std::string &title, const char *command) {
        Button *b = add<Button>(title, Button::Textured);
        b->small = true;
        b->action = [this, command] {
            if (onCommand)
                onCommand(command);
        };
        return b;
    };
    m_settings = button("Settings…", "PSSS");
    m_refresh = button("Refresh", "PSRB");
}

void DJPlacard::setSources(const std::vector<std::string> &names, int chosen)
{
    m_popup->names = names;
    m_popup->chosen = chosen;
    m_popup->setNeedsDisplay();
}

void DJPlacard::setFrame(const Rect &r)
{
    View::setFrame(r);
    // the pane a point down, under the line (FUN_000249e4's 47 points)
    m_popup->setFrame({68, 14, 170, 20});
    // bound to the right: as far from it as in the nib's 594-point pane
    m_settings->setFrame({r.w - (594 - 408), 13, 80, 22});
    m_refresh->setFrame({r.w - (594 - 496), 13, 80, 22});
}

void DJPlacard::draw(cairo_t *cr)
{
    drawPlacardBackground(cr, bounds(), Height + 1);
    drawText(cr, "Source:", Rect{9, 16, 52, 14}, txts(544, 11, Align::Right));
}

DJIntro::DJIntro()
{
    m_continue = add<Button>("Continue", Button::Textured);
    m_continue->action = [this] {
        if (onCommand)
            onCommand("PDon");
    };
}

Point DJIntro::origin() const
{
    // the box (795 × 517) in the middle of the list's room, its pane 65 down in it (by eye: centred)
    return {std::floor((frame().w - 795) / 2), std::floor((frame().h - 517) / 2) + 65};
}

void DJIntro::setFrame(const Rect &r)
{
    View::setFrame(r);
    const Point o = origin();
    m_continue->setFrame({o.x + 685, o.y + 318, 90, 22});
}

void DJIntro::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(255));
    const Point o = origin();
    auto at = [&](double x, double y, double w, double h) { return Rect{o.x + x, o.y + y, w, h}; };
    drawWrappedText(cr, "Welcome to iTunes DJ", at(20, 36, 460, 45), txts(1210, 36));
    drawWrappedText(cr, "iTunes DJ automatically picks songs to make a continuous mix of your music.", at(20, 94, 420, 60),
                    txts(1211, 18));
    for (const auto &[id, r] : {std::pair<int, Rect>{9203, at(508, 10, 233, 200)}, {9201, at(20, 234, 62, 61)}, {9202, at(407, 234, 58, 58)}}) {
        const Image p = picture(id);
        if (!p.isNull())
            p.draw(cr, Rect{0, 0, p.size().w, p.size().h}, r);
    }
    drawText(cr, "Always in Control", at(95, 234, 163, 23), txts(1212, 14));
    drawWrappedText(cr, "See the list of upcoming songs and add to, remove from, or reorder them at any time.", at(95, 256, 293, 40),
                    txts(1213, 11));
    drawText(cr, "Make a Request", at(479, 234, 163, 23), txts(1212, 14));
    drawWrappedText(cr, "Allow guests to request songs using the Remote application for iPhone and iPod touch.", at(479, 256, 300, 40),
                    txts(1213, 11));
    drawText(cr, "Learn more about iTunes DJ and Remote", at(20, 318, 257, 17), txts(1214, 12));
    drawCell(cr, 671, m_linkDown ? 1 : 0, 0, at(280, 322, 12, 12));
}

void DJIntro::mouseDown(Event &e)
{
    const Point o = origin();
    m_linkDown = Rect{o.x + 20, o.y + 318, 272, 17}.contains(e.location);
    setNeedsDisplay();
}

void DJIntro::mouseUp(Event &e)
{
    const Point o = origin();
    if (m_linkDown && Rect{o.x + 20, o.y + 318, 272, 17}.contains(e.location) && onCommand)
        onCommand("PInf");
    m_linkDown = false;
    setNeedsDisplay();
}

void DJSettingsWindow::show(const DJSettings &now, std::function<void(const DJSettings &)> done)
{
    if (m_window) {
        m_window->orderFront();
        return;
    }
    m_window = std::make_unique<Window>(Size{543, 356}, "iTunes DJ Settings");
    m_window->setStyleMask(1);   // a movable modal (class 4): no buttons in its title bar
    m_window->onClose = [this] { App::shared().later([this] { m_window.reset(); }); };
    View *c = m_window->contentView();
    static const int counts[9] = {0, 5, 10, 15, 20, 25, 50, 75, 100};
    // FUN_002b0246: a number's item
    auto item = [](int n) { return n < 50 ? n / 5 : n / 25 + 4; };
    auto popup = [&](Rect r, int value) {
        PopUpButton *p = c->add<PopUpButton>();
        for (int n : counts)
            p->items.emplace_back(std::to_string(n));
        p->setSelected(std::clamp(item(value), 0, 8));
        p->setFrame(r);
        return p;
    };
    auto label = [&](const std::string &t, Rect r, bool small = false) {
        Label *l = c->add<Label>(t, Font::system(small ? 11 : 13));
        l->wrap = small;
        l->setFrame(r);
        return l;
    };
    label("Display:", {107, 26, 62, 16});
    PopUpButton *played = popup({168, 25, 65, 20}, now.played);
    label("recently played songs", {238, 26, 176, 16});
    PopUpButton *upcoming = popup({168, 55, 65, 20}, now.upcoming);
    label("upcoming songs", {238, 56, 114, 16});
    CheckBox *rated = c->add<CheckBox>("Play higher rated songs more often");
    rated->setFrame({168, 88, 259, 18});
    rated->setChecked(now.higherRated);
    Box *line = c->add<Box>();
    line->kind = Box::Separator;
    line->setFrame({31, 114, 482, 1});
    CheckBox *requests = c->add<CheckBox>("Allow guests to request songs with Remote for iPhone or iPod touch");
    requests->setFrame({31, 129, 454, 18});
    requests->setChecked(now.requests);
    Label *welcome = label("Welcome Message:", {54, 151, 128, 16});
    TextField *message = c->add<TextField>();
    message->setFrame({54, 171, 441, 53});
    message->setText(now.message);
    CheckBox *voting = c->add<CheckBox>("Enable voting");
    voting->setFrame({54, 233, 107, 18});
    voting->setChecked(now.voting);
    Label *votingText = label("Guests can vote to control when songs play.", {76, 252, 409, 21}, true);
    CheckBox *password = c->add<CheckBox>("Require password: ");
    password->setFrame({54, 277, 141, 18});
    password->setChecked(now.password);
    TextField *pass = c->add<TextField>();
    pass->secure = true;
    pass->setFrame({197, 276, 298, 22});
    pass->setText(now.pass);
    // LAB_004cbf86 on box 10, LAB_004cb706 on box 15: the requests' items with their box, the password's
    // field with its own
    auto enable = [=] {
        const bool on = requests->checked();
        for (View *v : std::initializer_list<View *>{welcome, message, voting, votingText, password})
            v->setEnabled(on);
        pass->setEnabled(on && password->checked());
    };
    requests->onToggle = [enable](CheckBox *) { enable(); };
    password->onToggle = [enable](CheckBox *) { enable(); };
    enable();
    Button *help = c->add<Button>("", Button::Help);
    help->setFrame({18, 315, 21, 18});
    help->setEnabled(false);
    Button *cancel = c->add<Button>("Cancel");
    cancel->setFrame({371, 310, 70, 32});
    cancel->action = [this] { App::shared().later([this] { if (m_window) m_window->close(); }); };
    Button *ok = c->add<Button>("OK");
    ok->isDefault = true;
    ok->setFrame({453, 310, 70, 32});
    ok->action = [=, this] {
        DJSettings s;
        s.played = counts[std::clamp(played->selected(), 0, 8)];
        s.upcoming = counts[std::clamp(upcoming->selected(), 0, 8)];
        s.higherRated = rated->checked();
        s.requests = requests->checked();
        s.message = message->text;
        s.voting = voting->checked();
        // FUN_002b0286: the password kept on only with one
        s.pass = pass->text;
        s.password = password->checked() && !s.pass.empty();
        if (done)
            done(s);
        App::shared().later([this] { if (m_window) m_window->close(); });
    };
    m_window->show();
}

} // namespace iTunes
