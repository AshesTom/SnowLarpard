// SPDX-License-Identifier: LGPL-2.1-or-later
#include "menutoolbar.h"

#include <algorithm>
#include <cmath>

using namespace AK;
using SystemUI::MenuExtra;
using SystemUI::MenuExtraView;

// An item's place: its menu's highlight, the extra's view 4 points in
class MenuToolbar::ItemView : public View {
public:
    explicit ItemView(MenuExtra *extra) : extra(extra) { addSubview(extra->view()); }
    MenuExtra *extra;
    void draw(cairo_t *cr) override
    {
        if (extra->isMenuDown())
            MenuExtraView::drawHighlight(cr, Rect{0, 0, frame().w, frame().h - 1});
    }
    View *hitTest(Point p) override
    {
        // the whole place answers, the view's margins too
        return frame().contains(p) && !isHidden() ? extra->view() : nullptr;
    }
};

MenuToolbar::~MenuToolbar()
{
    unloadAll();
}

void MenuToolbar::unloadAll()
{
    for (MenuExtra *e : m_items)
        e->willUnload();
    removeAllSubviews();   // the views first: they know their extras
    for (MenuExtra *e : m_items)
        delete e;
    m_items.clear();
    m_views.clear();
}

void MenuToolbar::addItem(MenuExtra *extra) { insertItem(extra, m_items.size()); }

void MenuToolbar::insertItem(MenuExtra *extra, size_t index)
{
    index = std::min(index, m_items.size());
    extra->host = this;
    auto *v = new ItemView(extra);
    addSubview(v);
    m_items.insert(m_items.begin() + long(index), extra);
    m_views.insert(m_views.begin() + long(index), v);
    relayout();
}

void MenuToolbar::removeItem(MenuExtra *extra)
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i] == extra) {
            extra->willUnload();
            removeSubview(m_views[i]);   // its view with it
            m_items.erase(m_items.begin() + long(i));
            m_views.erase(m_views.begin() + long(i));
            delete extra;
            relayout();
            return;
        }
}

void MenuToolbar::relayout()
{
    // -relayout: from 4, each at its length + 8 (lengths rounded up, as the window server's offsets)
    double x = 4;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const double length = std::ceil(m_items[i]->length());
        m_views[i]->setFrame(Rect{x - 4, 0, length + 8, SystemUI::MenuBarHeight});
        m_items[i]->view()->setFrame(Rect{4, 0, length, SystemUI::MenuBarHeight});
        x += length + 8;
    }
    m_width = std::max(0.0, x - 4);
    setNeedsDisplay();
    if (resized)
        resized();
}

void MenuToolbar::extraViewChanged(MenuExtra *extra, MenuExtraView *old)
{
    for (ItemView *v : m_views)
        if (v->extra == extra) {
            v->removeSubview(old);   // deletes it
            v->addSubview(extra->view());
        }
    relayout();
}

MenuExtra *MenuToolbar::extraAt(Point p, Rect *place) const
{
    for (ItemView *v : m_views)
        if (v->frame().contains(p)) {
            if (place)
                *place = v->frame();
            return v->extra;
        }
    return nullptr;
}

Rect MenuToolbar::placeOf(MenuExtra *extra) const
{
    for (ItemView *v : m_views)
        if (v->extra == extra)
            return v->frame();
    return {};
}

void MenuToolbar::popUpMenu(MenuExtra *extra, const std::vector<MenuItem> &items, std::function<void()> closed)
{
    if (openMenu)
        openMenu(extra, items, std::move(closed));
}

// NSMenuToolbarView's Command-drag: the extra follows the pointer along the bar, the others making
// room; released off the bar (below it), it goes (-removeItem:, "poof"); else it stays where it was
// dropped and the list is written in the new order
void MenuToolbar::commandDrag(MenuExtra *extra, Drag phase, Point screen)
{
    if (extra == fixed)
        return;
    const Point origin = toWindow(Point{0, 0});
    const Window *w = window();
    const Point o = w ? w->toScreen(origin) : origin;
    const double x = screen.x - o.x;
    size_t at = 0;
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i] == extra)
            at = i;
    if (phase == Drag::Begin) {
        m_dragged = extra;
        return;
    }
    if (m_dragged != extra)
        return;
    const bool off = screen.y - o.y > SystemUI::MenuBarHeight + 12;
    if (phase == Drag::Move) {
        // its place: after the others whose middle is left of the pointer (Spotlight stays last)
        size_t to = 0;
        for (size_t i = 0; i < m_items.size(); ++i)
            if (m_items[i] != extra && m_items[i] != fixed && m_views[i]->frame().x + m_views[i]->frame().w / 2 < x)
                ++to;
        if (to != at) {
            ItemView *v = m_views[at];
            m_items.erase(m_items.begin() + long(at));
            m_views.erase(m_views.begin() + long(at));
            m_items.insert(m_items.begin() + long(to), extra);
            m_views.insert(m_views.begin() + long(to), v);
            relayout();
            at = to;
        }
        m_views[at]->setHidden(off);   // off the bar: about to go
        return;
    }
    m_dragged = nullptr;
    std::vector<std::string> order;
    for (MenuExtra *e : m_items)
        if (e != fixed && (!off || e != extra))
            order.push_back(e->bundlePath());
    const std::string removed = off ? extra->bundlePath() : std::string();
    if (off)
        removeItem(extra);
    if (listChanged)
        listChanged(order, removed);
}
