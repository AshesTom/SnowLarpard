// SPDX-License-Identifier: LGPL-2.1-or-later
#include "columnview.h"

#include "controls.h"
#include "foundation.h"
#include "text.h"
#include "toolbar.h"

#include <strings.h>
#include <sys/stat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Finder {

namespace {

constexpr double Divider = 1;          // between columns
constexpr double GripHeight = 15;      // the resize handle at the bottom of a divider
constexpr double SBW = CoreUI::ScrollerWidth;

} // namespace

void ColumnView::setRoot(const std::string &folder)
{
    m_columns.clear();
    Column c;
    c.folder = folder;
    c.items = list(folder);
    m_columns.push_back(std::move(c));
    m_active = 0;
    m_hoffset = 0;
    setNeedsDisplay();
    if (onChanged)
        onChanged();
}

std::string ColumnView::currentFolder() const
{
    // the deepest column's folder
    return m_columns.empty() ? std::string() : m_columns.back().folder;
}

size_t ColumnView::itemCount() const { return m_columns.empty() ? 0 : m_columns[m_active].items.size(); }

std::vector<Item> ColumnView::selectedItems() const
{
    if (m_columns.empty() || m_columns[m_active].selected < 0)
        return {};
    return {m_columns[m_active].items[size_t(m_columns[m_active].selected)]};
}

size_t ColumnView::selectedCount() const { return m_columns.empty() || m_columns[m_active].selected < 0 ? 0 : 1; }

bool ColumnView::hasPreview() const
{
    if (m_columns.empty())
        return false;
    const Column &last = m_columns.back();
    return showPreviewColumn && last.selected >= 0 && !last.items[size_t(last.selected)].folder;
}

double ColumnView::contentWidth() const
{
    double w = 0;
    for (const Column &c : m_columns)
        w += c.width + Divider;
    if (hasPreview())
        w += previewWidth;
    return w;
}

double ColumnView::columnsHeight() const { return frame().h - (contentWidth() > frame().w ? SBW : 0); }

double ColumnView::columnX(size_t i) const
{
    double x = -m_hoffset;
    for (size_t k = 0; k < i && k < m_columns.size(); ++k)
        x += m_columns[k].width + Divider;
    return x;
}

void ColumnView::reveal()
{
    // NSBrowser scrolls to show its last column (the preview with it)
    const double over = contentWidth() - frame().w;
    m_hoffset = std::max(0.0, over);
}

void ColumnView::scrollColumn(size_t column, double offset)
{
    Column &c = m_columns[column];
    const double total = c.items.size() * rowHeight();
    c.offset = std::clamp(offset, 0.0, std::max(0.0, total - columnsHeight()));
    setNeedsDisplay();
}

void ColumnView::select(size_t column, int row)
{
    if (column >= m_columns.size())
        return;
    m_columns.erase(m_columns.begin() + long(column) + 1, m_columns.end());
    Column &c = m_columns[column];
    c.selected = row;
    m_active = column;
    if (row >= 0) {
        // the row in view
        const double top = row * rowHeight(), h = columnsHeight();
        if (top < c.offset)
            c.offset = top;
        else if (top + rowHeight() > c.offset + h)
            c.offset = top + rowHeight() - h;
        const Item &item = c.items[size_t(row)];
        if (item.folder) {
            Column next;
            next.folder = item.path;
            next.items = list(item.path);
            next.width = c.width;
            m_columns.push_back(std::move(next));
        }
    }
    reveal();
    setNeedsDisplay();
    if (onChanged)
        onChanged();
}

// ---- drawing

void ColumnView::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(255));
    const double h = columnsHeight();
    for (size_t i = 0; i < m_columns.size(); ++i) {
        const Rect r{columnX(i), 0, m_columns[i].width, h};
        if (r.right() < 0 || r.x > frame().w)
            continue;
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_clip(cr);
        drawColumn(cr, i, r);
        cairo_restore(cr);
        // the divider and its resize handle at the bottom
        fill(cr, Rect{r.right(), 0, Divider, h}, Color::gray(201));
        const Rect grip{r.right() - 8, h - GripHeight, 9, GripHeight};
        fill(cr, grip, Color::gray(236));
        fill(cr, Rect{grip.x, grip.y, grip.w, 1}, Color::gray(190));
        fill(cr, Rect{grip.x + 3, grip.y + 4, 1, 7}, Color::gray(140));
        fill(cr, Rect{grip.x + 5, grip.y + 4, 1, 7}, Color::gray(140));
    }
    if (hasPreview()) {
        const Rect r{columnX(m_columns.size()), 0, std::max(previewWidth, frame().w - columnX(m_columns.size())), h};
        drawPreview(cr, r);
    }
    const double total = contentWidth();
    if (total > frame().w) {
        const double range = total - frame().w;
        CoreUI::scroller(cr, Rect{0, h, frame().w, SBW}, range > 0 ? m_hoffset / range : 0, frame().w / total, controlState(*this));
    }
}

void ColumnView::drawColumn(cairo_t *cr, size_t i, const Rect &r)
{
    const Column &c = m_columns[i];
    const double rh = rowHeight();
    const bool focused = isFirstResponder() && windowIsKey() && i == m_active;
    const double total = c.items.size() * rh;
    const bool scrolls = total > r.h;
    const double w = r.w - (scrolls ? SBW : 0);
    const Font font = Font::system(textSize);
    static const Image arrow = CoreUI::named("ChildContainer");
    const int first = int(c.offset / rh);
    for (int n = first; n < int(c.items.size()); ++n) {
        const double y = n * rh - c.offset;
        if (y > r.h)
            break;
        const Item &item = c.items[size_t(n)];
        const bool selected = n == c.selected;
        if (selected)   // the keyboard's column blue, the columns before it grey
            fill(cr, Rect{r.x, y, w, rh}, focused ? Color::rgb(56, 117, 215) : Color::gray(212));
        const Image img = showIcons ? icon(item, iconSize, showIconPreview) : Image();
        if (!img.isNull()) {
            const Size s = scaledToFit(img.size(), {double(iconSize), double(iconSize)});
            img.draw(cr, Rect{std::round(r.x + 3 + (iconSize - s.w) / 2), std::round(y + (rh - s.h) / 2), s.w, s.h});
        }
        double right = r.x + w - 4;
        if (item.folder && !arrow.isNull()) {
            const Rect ar{std::floor(right - arrow.width()), std::floor(y + (rh - arrow.height()) / 2), double(arrow.width()),
                          double(arrow.height())};
            if (selected && focused) {
                cairo_save(cr);
                cairo_set_source_rgb(cr, 1, 1, 1);
                cairo_mask_surface(cr, arrow.surface(), ar.x, ar.y);
                cairo_restore(cr);
            } else {
                arrow.draw(cr, ar);
            }
            right = ar.x - 3;
        }
        TextStyle t;
        t.font = font;
        t.color = selected && focused ? Color::gray(255) : Color::gray(0);
        const double tx = showIcons ? r.x + 3 + iconSize + 4 : r.x + 6;
        drawText(cr, item.name, Rect{tx, y, right - tx, rh}, t);
    }
    if (scrolls)
        CoreUI::scroller(cr, Rect{r.x + w, 0, SBW, r.h}, c.offset / (total - r.h), r.h / total, controlState(*this));
}

void ColumnView::drawPreview(cairo_t *cr, const Rect &r)
{
    // TColumnPreviewViewController: the item large, its name, then its kind, size and dates
    const Column &c = m_columns.back();
    const Item &item = c.items[size_t(c.selected)];
    const double side = std::min(128.0, std::max(32.0, r.w - 40));
    const Image img = icon(item, int(side), showIconPreview);
    double y = 20;
    if (!img.isNull()) {
        const Size s = scaledToFit(img.size(), {side, side});
        img.draw(cr, Rect{std::round(r.x + (r.w - s.w) / 2), std::round(y + side - s.h), s.w, s.h});
    }
    y += side + 14;
    struct stat st {};
    stat(item.path.c_str(), &st);
    // the labels Get Info's InfoWindowGeneralView.nib has ("Kind:", "Size:", "Created:", "Modified:")
    const std::vector<std::pair<std::string, std::string>> lines = {
        {"Name:", item.name},
        {"Kind:", kind(item)},
        {"Size:", formatBytes(sizeOnDisk(item))},
        {"Created:", formatDate(st.st_ctime, DateLong, false)},
        {"Modified:", formatDate(st.st_mtime, DateLong, false)},
        {"Last opened:", formatDate(st.st_atime, DateLong, false)},
    };
    const Font font = Font::system(11);
    const double labelW = std::floor(r.w * 0.38), gap = 6;
    for (const auto &l : lines) {
        TextStyle label;
        label.font = font;
        label.align = Align::Right;
        label.color = Color::gray(128);
        drawText(cr, l.first, Rect{r.x, y, labelW, 15}, label);
        TextStyle value;
        value.font = font;
        drawText(cr, l.second, Rect{r.x + labelW + gap, y, r.w - labelW - gap - 8, 15}, value);
        y += 15;
    }
}

// ---- events

void ColumnView::mouseDown(Event &e)
{
    if (window())
        window()->makeFirstResponder(this);
    const double h = columnsHeight();
    m_drag = None;
    // the horizontal scroller
    if (e.location.y >= h) {
        const double total = contentWidth(), range = total - frame().w;
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(Rect{0, h, frame().w, SBW}, range > 0 ? m_hoffset / range : 0, frame().w / total);
        if (p.knob.contains(e.location)) {
            m_drag = HKnob;
            m_dragFrom = e.location.x;
            m_dragStart = m_hoffset;
        } else {
            const double step = p.up.contains(e.location) ? -19 : p.down.contains(e.location) ? 19 : (e.location.x < p.knob.x ? -1 : 1) * (frame().w - 19);
            m_hoffset = std::clamp(m_hoffset + step, 0.0, std::max(0.0, range));
            setNeedsDisplay();
        }
        return;
    }
    for (size_t i = 0; i < m_columns.size(); ++i) {
        const double x = columnX(i), w = m_columns[i].width;
        // the resize handle
        if (e.location.x >= x + w - 8 && e.location.x <= x + w + Divider && e.location.y >= h - GripHeight) {
            m_drag = Resize;
            m_dragColumn = i;
            m_dragFrom = e.location.x;
            m_dragStart = w;
            return;
        }
        if (e.location.x < x || e.location.x >= x + w)
            continue;
        Column &c = m_columns[i];
        const double total = c.items.size() * rowHeight();
        if (total > h && e.location.x >= x + w - SBW) {
            // the column's own scroller
            const CoreUI::ScrollerParts p = CoreUI::scrollerParts(Rect{x + w - SBW, 0, SBW, h}, c.offset / (total - h), h / total);
            if (p.knob.contains(e.location)) {
                m_drag = VKnob;
                m_dragColumn = i;
                m_dragFrom = e.location.y;
                m_dragStart = c.offset;
            } else {
                const double step = p.up.contains(e.location) ? -rowHeight() : p.down.contains(e.location) ? rowHeight() : (e.location.y < p.knob.y ? -1 : 1) * (h - rowHeight());
                scrollColumn(i, c.offset + step);
            }
            return;
        }
        const int row = int((e.location.y + c.offset) / rowHeight());
        if (row < 0 || row >= int(c.items.size())) {
            // a click under the items: this column's selection goes
            select(i, -1);
            return;
        }
        if (e.clickCount == 2 && !c.items[size_t(row)].folder) {
            if (onOpen)
                onOpen(c.items[size_t(row)]);
            return;
        }
        select(i, row);
        return;
    }
}

void ColumnView::mouseDragged(Event &e)
{
    switch (m_drag) {
    case Resize:
        m_columns[m_dragColumn].width = std::max(MinColumnWidth, m_dragStart + e.location.x - m_dragFrom);
        setNeedsDisplay();
        break;
    case HKnob: {
        const double total = contentWidth();
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(Rect{0, columnsHeight(), frame().w, SBW}, 0, frame().w / total);
        const double travel = p.track.w - p.knob.w;
        if (travel > 0)
            m_hoffset = std::clamp(m_dragStart + (e.location.x - m_dragFrom) * (total - frame().w) / travel, 0.0, total - frame().w);
        setNeedsDisplay();
        break;
    }
    case VKnob: {
        Column &c = m_columns[m_dragColumn];
        const double h = columnsHeight(), total = c.items.size() * rowHeight();
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(Rect{0, 0, SBW, h}, 0, h / total);
        const double travel = p.track.h - p.knob.h;
        if (travel > 0)
            scrollColumn(m_dragColumn, m_dragStart + (e.location.y - m_dragFrom) * (total - h) / travel);
        break;
    }
    case None:
        break;
    }
}

void ColumnView::mouseUp(Event &) { m_drag = None; }

void ColumnView::scrollWheel(Event &e)
{
    // the column under the pointer
    for (size_t i = 0; i < m_columns.size(); ++i) {
        const double x = columnX(i);
        if (e.location.x >= x && e.location.x < x + m_columns[i].width) {
            scrollColumn(i, m_columns[i].offset + e.deltaY * rowHeight() * 2);
            return;
        }
    }
}

void ColumnView::keyDown(Event &e)
{
    if (m_columns.empty()) {
        View::keyDown(e);
        return;
    }
    Column &c = m_columns[m_active];
    const int n = int(c.items.size());
    if (e.modifiers & Command) {
        if ((e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_o) && c.selected >= 0 && onOpen)
            onOpen(c.items[size_t(c.selected)]);
        else
            View::keyDown(e);
        return;
    }
    switch (e.keysym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        View::keyDown(e);   // the window's: renaming, Quick Look
        return;
    case XKB_KEY_Down:
        if (n)
            select(m_active, c.selected < 0 ? 0 : std::min(n - 1, c.selected + 1));
        return;
    case XKB_KEY_Up:
        if (n)
            select(m_active, c.selected < 0 ? 0 : std::max(0, c.selected - 1));
        return;
    case XKB_KEY_Right:
        // into the folder: its first item
        if (m_active + 1 < m_columns.size() && !m_columns[m_active + 1].items.empty())
            select(m_active + 1, 0);
        return;
    case XKB_KEY_Left:
        if (m_active > 0) {
            const size_t parent = m_active - 1;
            select(parent, m_columns[parent].selected);
        }
        return;
    default:
        break;
    }
    if (!e.text.empty() && static_cast<unsigned char>(e.text[0]) >= 0x20) {
        for (int i = 0; i < n; ++i)
            if (strncasecmp(c.items[size_t(i)].name.c_str(), e.text.c_str(), e.text.size()) == 0) {
                select(m_active, i);
                return;
            }
        return;
    }
    View::keyDown(e);
}

void ColumnView::refresh()
{
    for (size_t i = 0; i < m_columns.size(); ++i) {
        Column &c = m_columns[i];
        const std::string selected = c.selected >= 0 && size_t(c.selected) < c.items.size() ? c.items[size_t(c.selected)].name : "";
        c.items = list(c.folder);
        c.selected = -1;
        for (size_t k = 0; k < c.items.size(); ++k)
            if (c.items[k].name == selected)
                c.selected = int(k);
        if (c.selected < 0) {
            // its selection is gone: the columns after it too
            m_columns.erase(m_columns.begin() + long(i) + 1, m_columns.end());
            m_active = std::min(m_active, m_columns.size() - 1);
            break;
        }
    }
    setNeedsDisplay();
    if (onChanged)
        onChanged();
}

bool ColumnView::shows(const std::string &folder) const
{
    for (const Column &c : m_columns)
        if (c.folder == folder)
            return true;
    return false;
}

void ColumnView::selectPaths(const std::vector<std::string> &paths)
{
    for (const std::string &p : paths)
        for (size_t i = m_columns.size(); i-- > 0;)
            for (size_t k = 0; k < m_columns[i].items.size(); ++k)
                if (m_columns[i].items[k].path == p) {
                    select(i, int(k));
                    return;
                }
}

Rect ColumnView::nameRectInWindow(const std::string &path) const
{
    for (size_t i = 0; i < m_columns.size(); ++i)
        for (size_t k = 0; k < m_columns[i].items.size(); ++k)
            if (m_columns[i].items[k].path == path) {
                const double x = columnX(i) + (showIcons ? 3 + iconSize + 4 : 6) - 2;
                const double y = k * rowHeight() - m_columns[i].offset;
                const Point o = toWindow({x, y});
                return {o.x, o.y, m_columns[i].width - (x - columnX(i)) - 18, rowHeight()};
            }
    return {};
}

void ColumnView::rightMouseDown(Event &e)
{
    bool onItem = false;
    for (size_t i = 0; i < m_columns.size(); ++i) {
        const double x = columnX(i), w = m_columns[i].width;
        if (e.location.x < x || e.location.x >= x + w)
            continue;
        Column &c = m_columns[i];
        const int row = int((e.location.y + c.offset) / rowHeight());
        if (row >= 0 && row < int(c.items.size())) {
            onItem = true;
            if (c.selected != row || m_active != i)
                select(i, row);
        } else {
            m_active = i;
        }
        break;
    }
    if (onContextMenu)
        onContextMenu(toWindow(e.location), onItem);
}

} // namespace Finder
