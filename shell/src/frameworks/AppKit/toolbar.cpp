// SPDX-License-Identifier: LGPL-2.1-or-later
#include "toolbar.h"

#include <algorithm>
#include <cmath>

namespace AK {

void Toolbar::addItem(const std::string &id, const std::string &label, View *view, double width, double maxWidth)
{
    Item it;
    it.id = id;
    it.label = label;
    it.view = view;
    it.width = id == "NSToolbarSpaceItem" ? SpaceWidth : width;
    it.maxWidth = std::max(maxWidth, it.width);
    it.flexible = id == "NSToolbarFlexibleSpaceItem";
    if (view)
        addSubview(view);
    items.push_back(it);
}

double Toolbar::itemWidth(const Item &it) const
{
    if (it.flexible)
        return FlexibleMinimum;
    if (!it.view)
        return it.width;   // a space
    // -[NSToolbarItemViewer frameWidthForIconWidth:]: the control, its padding
    return (m_labels ? std::max(it.width, std::ceil(textWidth(it.label, Font::system(11))) + 4) : it.width) + 2 * Padding;
}

void Toolbar::layout()
{
    // what doesn't fit goes, from the end (NSToolbar's overflow)
    const double w = frame().w;
    double used = 2 * Margin;
    for (Item &it : items) {
        it.shown = true;
        used += itemWidth(it);
    }
    for (int i = int(items.size()) - 1; i >= 0 && used > w; --i)
        if (!items[size_t(i)].flexible) {
            items[size_t(i)].shown = false;
            used -= itemWidth(items[size_t(i)]);
        }
    double room = std::max(0.0, w - used);
    std::vector<double> grow(items.size(), 0);

    // the flexible spaces tracking a divider (_sizePartitioningSpacersInItemViewers:...): to its x
    double x = Margin;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].shown)
            continue;
        if (items[i].flexible && items[i].alignTo >= 0) {
            grow[i] = std::min(room, std::max(0.0, items[i].alignTo - x - FlexibleMinimum));
            room -= grow[i];
        }
        x += itemWidth(items[i]) + grow[i];
    }

    // the others by weight, each up to its maximum
    struct Share {
        size_t index;
        double weight, capacity;
    };
    std::vector<Share> shares;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item &it = items[i];
        if (!it.shown || (it.flexible && it.alignTo >= 0))
            continue;
        if (it.flexible)
            shares.push_back({i, 0.25, INFINITY});
        else if (it.maxWidth > it.width)
            shares.push_back({i, 1, it.maxWidth - it.width});
    }
    while (room > 0.001 && !shares.empty()) {
        double weights = 0;
        for (const Share &sh : shares)
            weights += sh.weight;
        double rate = room / weights;
        for (const Share &sh : shares)
            rate = std::min(rate, sh.capacity / sh.weight);
        for (Share &sh : shares) {
            grow[sh.index] += rate * sh.weight;
            sh.capacity -= rate * sh.weight;
            room -= rate * sh.weight;
        }
        shares.erase(std::remove_if(shares.begin(), shares.end(), [](const Share &sh) { return sh.capacity <= 0.001; }), shares.end());
    }

    x = Margin;
    for (size_t i = 0; i < items.size(); ++i) {
        Item &it = items[i];
        if (!it.shown) {
            if (it.view)
                it.view->setHidden(true);
            continue;
        }
        const double g = std::floor(grow[i]);
        const double iw = itemWidth(it) + g;
        const double cw = it.width + g;
        it.frame = {x, 0, iw, height()};
        if (it.view) {
            it.view->setHidden(false);
            const double h = it.view->frame().h > 0 ? std::min(it.view->frame().h, ControlHeight) : ControlHeight;
            it.view->setFrame({std::floor(x + (iw - cw) / 2), controlTop() + std::floor((ControlHeight - h) / 2), cw, h});
        }
        x += iw;
    }
    setNeedsDisplay();
}

void Toolbar::setFrame(const Rect &r)
{
    View::setFrame(r);
    layout();
}

Color Toolbar::rowColor(int row, int total, bool active)
{
    if (row <= 0)
        return active ? Color::gray(218) : Color::gray(236);
    const double t = std::clamp(double(row - 1) / std::max(1, total - 2), 0.0, 1.0);
    const double top = active ? 207 : 232, bottom = active ? 167 : 207;
    return Color::gray(int(std::lround(top + (bottom - top) * t)));
}

void Toolbar::draw(cairo_t *cr)
{
    const bool active = windowIsKey();
    const int total = 22 + int(gradient());
    for (int y = 0; y < gradient(); ++y)
        fill(cr, Rect{0, double(y), frame().w, 1}, rowColor(22 + y, total, active));
    fill(cr, Rect{0, gradient(), frame().w, 1}, active ? Color::gray(81) : Color::gray(135));
    if (!m_labels)
        return;
    TextStyle t;
    t.font = Font::system(11);
    t.align = Align::Center;
    t.engraved = true;
    t.engravedAlpha = 0.45;
    t.color = active ? Color::gray(0) : Color::gray(0, 0.5);
    for (const Item &it : items)
        if (it.shown && !it.flexible && !it.label.empty())
            drawText(cr, it.label, Rect{it.frame.x - 4, ControlTop + ControlHeight + 2, it.frame.w + 8, 15}, t);
}

void drawTemplate(cairo_t *cr, const Image &image, const Rect &r, bool enabled, bool active, bool highlighted)
{
    if (image.isNull())
        return;
    // the image's alpha as a mask: the emboss under it, then the glyph
    const double alpha = enabled ? (active ? 1 : 0.6) : 0.35;
    cairo_save(cr);
    if (highlighted)
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35 * alpha);
    else
        cairo_set_source_rgba(cr, 1, 1, 1, 0.55 * alpha);
    cairo_mask_surface(cr, image.surface(), r.x, r.y + 1);
    if (highlighted)
        cairo_set_source_rgba(cr, 1, 1, 1, alpha);
    else
        cairo_set_source_rgba(cr, 0.19, 0.19, 0.19, alpha);
    cairo_mask_surface(cr, image.surface(), r.x, r.y);
    cairo_restore(cr);
}

SegmentedControl *toolbarSegments(const std::vector<Image> &images, const std::vector<double> &widths, bool momentary, bool templates)
{
    auto *s = new SegmentedControl(CoreUI::SegmentStyle::Textured, std::vector<std::string>(images.size()), widths);
    s->momentary = momentary;
    s->glyph = [images, templates, s](cairo_t *cr, int i, const Rect &r, bool enabled) {
        const Image &img = images[size_t(i)];
        if (img.isNull())
            return;
        const Rect at{std::floor(r.x + (r.w - img.width()) / 2), std::floor(r.y + (r.h - img.height()) / 2), double(img.width()),
                      double(img.height())};
        if (templates)
            drawTemplate(cr, img, at, enabled, s->windowIsKey(), s->isHighlighted(i));
        else
            img.draw(cr, at, enabled ? 1 : 0.35);
    };
    return s;
}

} // namespace AK
