// SPDX-License-Identifier: LGPL-2.1-or-later
#include "listview.h"

#include "foundation.h"
#include "text.h"

#include <strings.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Finder {

namespace {

constexpr double HeaderHeight = 17;     // NSTableHeaderView in ListView.nib
constexpr double ColumnSpacing = 2;     // NSIntercellSpacingWidth
constexpr double TriangleWidth = 16;    // the outline column's disclosure triangle's place

std::string localized(const std::string &key, const std::string &fallback)
{
    return Assets::tr("/System/Library/CoreServices/Finder.app", "LocalizableCore", key, fallback);
}

} // namespace

// ---- the header: NSTableHeaderView's cells (CoreUI's listheader), the sorted column blue with its arrow

class ListHeader : public View {
public:
    explicit ListHeader(ListView *list) : m_list(list) {}

    void draw(cairo_t *cr) override
    {
        const bool active = windowIsKey();
        const double width = std::max(frame().w, m_list->contentWidth()) ;
        cairo_translate(cr, -m_list->m_hoffset, 0);
        double x = 0;
        const auto &cols = m_list->m_columns;
        for (size_t i = 0; i < cols.size(); ++i) {
            const bool last = i + 1 == cols.size();
            const double w = last ? std::max(0.0, width - x) : cols[i].width + ColumnSpacing;
            const Rect r{x, 0, w, HeaderHeight};
            const bool sorted = cols[i].id == m_list->m_sortColumn;
            const std::string direction = m_list->m_ascending ? "selectup" : "selectdown";
            // the sorted column: the blue cell (clear in an inactive window), its arrow apart
            CoreUI::Element bg;
            if (sorted)
                bg = CoreUI::element({"listheader", active ? "blue" : "clear", "regular"});
            if (bg.isNull())
                bg = CoreUI::element({"listheader", active ? "active" : "clear", "clear", "regular"});
            if (bg.isNull())
                bg = CoreUI::element({"listheader", "clear", "regular"});
            CoreUI::draw(cr, bg, r);
            // the separator at the cell's right edge
            if (!last) {
                const CoreUI::Element sep = CoreUI::element({"listheader", "clear", "regular", "separator"});
                if (!sep.isNull() && !sep.pieces.empty()) {
                    const Image &p = sep.pieces[sep.pieces.size() / 2];
                    p.draw(cr, Rect{r.right() - p.width(), 0, double(p.width()), HeaderHeight});
                } else {
                    fill(cr, Rect{r.right() - 1, 2, 1, HeaderHeight - 4}, Color::gray(0, 0.2));
                }
            }
            double textRight = r.right() - 4;
            if (sorted) {
                const CoreUI::Element arrow = CoreUI::element({"listheader", "arrow", active ? "blue" : "clear", "regular", direction});
                if (!arrow.isNull()) {
                    const Image &a = arrow.pieces.front();
                    const Rect ar{std::floor(r.right() - a.width() - 6), std::floor((HeaderHeight - a.height()) / 2), double(a.width()),
                                  double(a.height())};
                    a.draw(cr, ar);
                    textRight = ar.x - 2;
                }
            }
            TextStyle t;
            t.font = Font::system(11);
            t.align = cols[i].align;
            drawText(cr, cols[i].title, Rect{r.x + 5, 1, textRight - r.x - 5, HeaderHeight - 2}, t);
            x += w;
        }
    }

    void mouseDown(Event &e) override
    {
        // a click sorts by the column; again, the other way
        double x = 0;
        const double px = e.location.x + m_list->m_hoffset, width = std::max(frame().w, m_list->contentWidth());
        const auto &cols = m_list->m_columns;
        for (size_t i = 0; i < cols.size(); ++i) {
            const double w = i + 1 == cols.size() ? width - x : cols[i].width + ColumnSpacing;
            if (px >= x && px < x + w) {
                if (m_list->m_sortColumn == cols[i].id) {
                    m_list->m_ascending = !m_list->m_ascending;
                } else {
                    m_list->m_sortColumn = cols[i].id;
                    // dates newest first, the others A to Z
                    m_list->m_ascending = cols[i].id.rfind("date", 0) != 0;
                }
                m_list->sort();
                if (m_list->onSortChanged)
                    m_list->onSortChanged();
                return;
            }
            x += w;
        }
    }

private:
    ListView *m_list;
};

// ---- the rows

class ListBody : public View {
public:
    explicit ListBody(ListView *list) : m_list(list) {}

    void draw(cairo_t *cr) override
    {
        const ListView &l = *m_list;
        const double pitch = l.pitch();
        const bool focused = isFirstResponder() && windowIsKey();
        // NSTableView's alternating rows, on to the bottom of the view
        const int count = std::max(int(l.m_rows.size()), int(std::ceil(frame().h / pitch)));
        for (int i = 0; i < count; ++i) {
            const Rect row{0, i * pitch, frame().w, pitch};
            const bool selected = i < int(l.m_rows.size()) && l.m_selection.count(i);
            Color bg = i % 2 ? Color::rgb(237, 243, 254) : Color::gray(255);
            if (selected)   // alternateSelectedControlColor with the keyboard, else secondarySelectedControlColor
                bg = focused ? Color::rgb(56, 117, 215) : Color::gray(212);
            fill(cr, row, bg);
        }
        const Font font = Font::system(l.textSize);
        const double width = std::max(frame().w, l.contentWidth());
        cairo_translate(cr, -l.m_hoffset, 0);
        for (int i = 0; i < int(l.m_rows.size()); ++i) {
            const double y = i * pitch;
            if (y > frame().h)
                break;
            const ListView::Row &r = l.m_rows[size_t(i)];
            const bool selected = l.m_selection.count(i) > 0;
            TextStyle t;
            t.font = font;
            t.color = selected && focused ? Color::gray(255) : Color::gray(0);
            double x = 0;
            for (size_t c = 0; c < l.m_columns.size(); ++c) {
                const ListView::Column &col = l.m_columns[c];
                const bool last = c + 1 == l.m_columns.size();
                const double w = last ? std::max(0.0, width - x) : col.width + ColumnSpacing;
                const Rect cell{x + ColumnSpacing / 2, y + 1.5, w - ColumnSpacing, l.rowHeight};
                if (col.id == "name") {
                    const double indent = r.level * (l.iconSize + 5);
                    if (r.item.folder)
                        CoreUI::disclosureTriangle(cr, Rect{cell.x + indent + 1, cell.y + std::floor((cell.h - 13) / 2), 13, 13}, r.expanded,
                                                   false);
                    const double iconX = cell.x + indent + TriangleWidth;
                    const Image img = icon(r.item, l.iconSize, l.showIconPreview);
                    if (!img.isNull()) {
                        const Size s = scaledToFit(img.size(), {double(l.iconSize), double(l.iconSize)});
                        img.draw(cr, Rect{std::round(iconX + (l.iconSize - s.w) / 2), std::round(cell.y + (cell.h - s.h) / 2), s.w, s.h});
                    }
                    const double textX = iconX + l.iconSize + 4;
                    t.align = Align::Left;
                    drawText(cr, r.item.name, Rect{textX, cell.y, cell.right() - textX - 2, cell.h}, t);
                } else {
                    std::string text;
                    if (col.id == "dateModified")
                        text = dateText(modificationTime(r.item), cell.w - 6, font, l.relativeDates);
                    else if (col.id == "dateCreated")
                        text = dateText(creationTime(r.item), cell.w - 6, font, l.relativeDates);
                    else if (col.id == "dateLastOpened")
                        text = dateText(accessTime(r.item), cell.w - 6, font, l.relativeDates);
                    else if (col.id == "version")
                        text = "--";
                    else if (col.id == "size")
                        text = r.item.folder ? "--" : formatBytes(sizeOnDisk(r.item));
                    else if (col.id == "kind")
                        text = kind(r.item);
                    t.align = col.align;
                    drawText(cr, text, Rect{cell.x + 3, cell.y, cell.w - 6, cell.h}, t);
                }
                x += w;
            }
        }
    }

    void mouseDown(Event &e) override
    {
        if (window())
            window()->makeFirstResponder(this);
        ListView &l = *m_list;
        const int i = int(e.location.y / l.pitch());
        if (i < 0 || i >= int(l.m_rows.size())) {
            if (!(e.modifiers & (Command | Shift)))
                l.select(-1, false);
            return;
        }
        const ListView::Row &r = l.m_rows[size_t(i)];
        // the disclosure triangle opens the folder in place
        const double indent = r.level * (l.iconSize + 5), px = e.location.x + l.m_hoffset;
        if (r.item.folder && px >= ColumnSpacing / 2 + indent && px < ColumnSpacing / 2 + indent + TriangleWidth) {
            l.toggle(i);
            return;
        }
        if (e.clickCount == 2 && !(e.modifiers & (Command | Shift))) {
            if (l.onOpen)
                l.onOpen(r.item);
            return;
        }
        if (e.modifiers & Shift && l.m_anchor >= 0) {
            l.m_selection.clear();
            for (int k = std::min(l.m_anchor, i); k <= std::max(l.m_anchor, i); ++k)
                l.m_selection.insert(k);
            setNeedsDisplay();
            if (l.onSelectionChanged)
                l.onSelectionChanged();
            return;
        }
        l.select(i, e.modifiers & Command);
    }

    void keyDown(Event &e) override
    {
        ListView &l = *m_list;
        const int n = int(l.m_rows.size());
        const int at = l.m_anchor;
        if (e.modifiers & Command) {
            if ((e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_o) && l.m_selection.size() == 1 && l.onOpen) {
                l.onOpen(l.m_rows[size_t(*l.m_selection.begin())].item);
                return;
            }
            if (e.keysym == XKB_KEY_Up && l.onBack) {
                l.onBack();
                return;
            }
            if (e.keysym == XKB_KEY_a) {
                l.selectAll();
                return;
            }
            View::keyDown(e);
            return;
        }
        switch (e.keysym) {
        case XKB_KEY_Down:
            if (n)
                l.select(at < 0 ? 0 : std::min(n - 1, at + 1), false), l.reveal(l.m_anchor);
            return;
        case XKB_KEY_Up:
            if (n)
                l.select(at < 0 ? 0 : std::max(0, at - 1), false), l.reveal(l.m_anchor);
            return;
        case XKB_KEY_Right:
            if (at >= 0 && l.m_rows[size_t(at)].item.folder && !l.m_rows[size_t(at)].expanded)
                l.toggle(at);
            return;
        case XKB_KEY_Left:
            if (at >= 0 && l.m_rows[size_t(at)].expanded)
                l.toggle(at);
            return;
        default:
            break;
        }
        if (!e.text.empty() && static_cast<unsigned char>(e.text[0]) >= 0x20) {
            // type-select
            for (int i = 0; i < n; ++i)
                if (strncasecmp(l.m_rows[size_t(i)].item.name.c_str(), e.text.c_str(), e.text.size()) == 0) {
                    l.select(i, false);
                    l.reveal(i);
                    return;
                }
            return;
        }
        View::keyDown(e);
    }

    bool acceptsFirstResponder() const override { return true; }

private:
    // the longest of TDateFormatter's relative styles that fits the column (TListDateCell)
    static std::string dateText(long long time, double width, const Font &font, bool relative)
    {
        if (time <= 0)
            return "--";
        for (DateStyle s : {DateFull, DateLong, DateMedium, DateShort}) {
            const std::string text = formatDate(time, s, relative);
            if (textWidth(text, font) <= width || s == DateShort)
                return text;
        }
        return {};
    }

    ListView *m_list;
};

// ---- the horizontal scroller (CoreUI's, as Preview's pages have it)

class HScroller : public View {
public:
    explicit HScroller(ListView *list) : m_list(list) {}

    void draw(cairo_t *cr) override
    {
        const Rect r = track();
        const double total = m_list->contentWidth(), seen = r.w;
        const double range = total - seen;
        CoreUI::scroller(cr, r, range > 0 ? m_list->m_hoffset / range : 0, total > 0 ? seen / total : 1, controlState(*this));
        if (r.w < frame().w)   // the corner beside the vertical scroller
            fill(cr, Rect{r.w, 0, frame().w - r.w, frame().h}, Color::gray(237));
    }

    void mouseDown(Event &e) override
    {
        const Rect r = track();
        const double total = m_list->contentWidth(), seen = r.w, range = total - seen;
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(r, range > 0 ? m_list->m_hoffset / range : 0, seen / total);
        m_knob = p.knob.contains(e.location);
        m_from = e.location.x;
        m_start = m_list->m_hoffset;
        if (m_knob)
            return;
        if (p.up.contains(e.location))
            m_list->scrollHorizontally(m_list->m_hoffset - 19);
        else if (p.down.contains(e.location))
            m_list->scrollHorizontally(m_list->m_hoffset + 19);
        else   // a page towards the click
            m_list->scrollHorizontally(m_list->m_hoffset + (e.location.x < p.knob.x ? -1 : 1) * (seen - 19));
    }

    void mouseDragged(Event &e) override
    {
        if (!m_knob)
            return;
        const Rect r = track();
        const double total = m_list->contentWidth(), seen = r.w;
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(r, 0, seen / total);
        const double travel = p.track.w - p.knob.w;
        if (travel > 0)
            m_list->scrollHorizontally(m_start + (e.location.x - m_from) * (total - seen) / travel);
    }

private:
    // beside the vertical scroller when there is one
    Rect track() const { return {0, 0, m_list->m_scroll->document()->frame().w, frame().h}; }
    ListView *m_list;
    bool m_knob = false;
    double m_from = 0, m_start = 0;
};

// ---- ListView

ListView::ListView()
{
    // ListView.nib's columns (their titles the Finder's LocalizableCore when it has them), those shown
    // by default
    m_allColumns = {
        {"name", localized("N220", "Name"), 300, 110, Align::Left},
        {"dateModified", localized("N221", "Date Modified"), 181, 80, Align::Left},
        {"dateCreated", localized("N222", "Date Created"), 181, 80, Align::Left},
        {"size", localized("N223", "Size"), 100, 65, Align::Right},
        {"kind", localized("N224", "Kind"), 115, 80, Align::Left},
        {"version", localized("N226", "Version"), 75, 50, Align::Left},
        {"comments", localized("N227", "Comments"), 300, 60, Align::Left},
        {"label", localized("N225", "Label"), 100, 40, Align::Left},
        {"dateLastOpened", "Last Opened", 181, 80, Align::Left},
    };
    setColumnsShown({"name", "dateModified", "size", "kind"});
    // -rowHeightForIconAndText
    rowHeight = (iconSize == 16 ? std::min(16, textSize) : iconSize) + 4;
    m_header = add<ListHeader>(this);
    m_scroll = add<ScrollView>();
    m_scroll->lineStep = pitch();
    m_body = m_scroll->document()->add<ListBody>(this);
    m_hscroller = add<HScroller>(this);
}

double ListView::contentWidth() const
{
    double w = 0;
    for (const Column &c : m_columns)
        w += c.width + ColumnSpacing;
    return w;
}

void ListView::scrollHorizontally(double x)
{
    const double visible = m_scroll->document()->frame().w;
    m_hoffset = std::clamp(x, 0.0, std::max(0.0, contentWidth() - visible));
    m_header->setNeedsDisplay();
    m_body->setNeedsDisplay();
    m_hscroller->setNeedsDisplay();
}

void ListView::setFrame(const Rect &r)
{
    View::setFrame(r);
    layout();
}

void ListView::layout()
{
    // the columns wider than the view: NSScrollView's horizontal scroller under the rows
    const double rowsHeight = m_rows.size() * pitch();
    bool horizontal = contentWidth() > frame().w;
    for (int pass = 0; pass < 2; ++pass) {
        const double h = std::max(0.0, frame().h - HeaderHeight - (horizontal ? CoreUI::ScrollerWidth : 0));
        const double w = frame().w - (rowsHeight > h ? CoreUI::ScrollerWidth : 0);
        horizontal = contentWidth() > w;
    }
    const double h = std::max(0.0, frame().h - HeaderHeight - (horizontal ? CoreUI::ScrollerWidth : 0));
    m_header->setFrame({0, 0, frame().w, HeaderHeight});
    m_scroll->setFrame({0, HeaderHeight, frame().w, h});
    m_scroll->setDocumentHeight(rowsHeight);
    m_body->setFrame(m_scroll->document()->bounds());
    m_hscroller->setHidden(!horizontal);
    m_hscroller->setFrame({0, HeaderHeight + h, frame().w, CoreUI::ScrollerWidth});
    scrollHorizontally(horizontal ? m_hoffset : 0);
    m_header->setNeedsDisplay();
    m_body->setNeedsDisplay();
}

void ListView::focus()
{
    if (window())
        window()->makeFirstResponder(m_body);
}

void ListView::setItems(std::vector<Item> items)
{
    m_rows.clear();
    for (Item &it : items)
        m_rows.push_back({std::move(it), 0, false});
    m_selection.clear();
    m_anchor = -1;
    sort();
    m_scroll->scrollTo(0);
}

size_t ListView::itemCount() const
{
    return size_t(std::count_if(m_rows.begin(), m_rows.end(), [](const Row &r) { return r.level == 0; }));
}

std::vector<Item> ListView::selectedItems() const
{
    std::vector<Item> out;
    for (int i : m_selection)
        out.push_back(m_rows[size_t(i)].item);
    return out;
}

void ListView::setSort(const std::string &column, bool ascending)
{
    if (std::none_of(m_columns.begin(), m_columns.end(), [&](const Column &c) { return c.id == column; }))
        return;
    if (column == m_sortColumn && ascending == m_ascending)
        return;
    m_sortColumn = column;
    m_ascending = ascending;
    sort();
}

void ListView::setColumnsShown(const std::vector<std::string> &ids)
{
    // the nib's order; Name always
    std::vector<Column> shown;
    for (const Column &c : m_allColumns)
        if (c.id == "name" || std::find(ids.begin(), ids.end(), c.id) != ids.end()) {
            // a column keeps its width while hidden
            auto old = std::find_if(m_columns.begin(), m_columns.end(), [&](const Column &o) { return o.id == c.id; });
            shown.push_back(old != m_columns.end() ? *old : c);
        }
    m_columns = shown;
    if (std::none_of(m_columns.begin(), m_columns.end(), [&](const Column &c) { return c.id == m_sortColumn; }))
        m_sortColumn = "name";
    if (m_header)
        layout();
}

std::vector<std::string> ListView::columnsShown() const
{
    std::vector<std::string> out;
    for (const Column &c : m_columns)
        out.push_back(c.id);
    return out;
}

void ListView::setOptions(int text, int icons, bool relative, bool previews)
{
    // -rowHeightForIconAndText, -configureIndentation follow
    textSize = std::clamp(text, 10, 16);
    iconSize = icons >= 32 ? 32 : 16;
    relativeDates = relative;
    showIconPreview = previews;
    rowHeight = (iconSize == 16 ? std::min(16, textSize) : iconSize) + 4;
    m_scroll->lineStep = pitch();
    layout();
}

std::vector<Item> ListView::rowItems() const
{
    std::vector<Item> out;
    for (const Row &r : m_rows)
        out.push_back(r.item);
    return out;
}

void ListView::selectPath(const std::string &path)
{
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].item.path == path) {
            m_selection = {int(i)};
            m_anchor = int(i);
            reveal(int(i));
            m_body->setNeedsDisplay();
            return;
        }
}

void ListView::sortRows(std::vector<Row> &rows) const
{
    auto less = [this](const Row &a, const Row &b) {
        int c = 0;
        if (m_sortColumn == "dateModified") {
            const long long ta = modificationTime(a.item), tb = modificationTime(b.item);
            c = ta < tb ? -1 : ta > tb ? 1 : 0;
        } else if (m_sortColumn == "size") {
            const long long sa = a.item.folder ? -1 : sizeOnDisk(a.item), sb = b.item.folder ? -1 : sizeOnDisk(b.item);
            c = sa < sb ? -1 : sa > sb ? 1 : 0;
        } else if (m_sortColumn == "kind") {
            const std::string ka = kind(a.item), kb = kind(b.item);
            c = finderLess(ka, kb) ? -1 : finderLess(kb, ka) ? 1 : 0;
        }
        if (c == 0)   // then by name, as the Finder does
            c = finderLess(a.item.name, b.item.name) ? -1 : finderLess(b.item.name, a.item.name) ? 1 : 0;
        return m_ascending ? c < 0 : c > 0;
    };
    std::stable_sort(rows.begin(), rows.end(), less);
}

void ListView::sort()
{
    // each level sorted within its folder: the top level, then each open folder's children after it
    std::vector<Item> selected = selectedItems();
    std::function<std::vector<Row>(size_t &, int)> take = [&](size_t &i, int level) {
        std::vector<Row> group;
        while (i < m_rows.size() && m_rows[i].level == level) {
            Row r = m_rows[i++];
            std::vector<Row> children = r.expanded ? take(i, level + 1) : std::vector<Row>{};
            group.push_back(r);
            group.insert(group.end(), children.begin(), children.end());
        }
        return group;
    };
    // sort the parents with their subtrees attached
    std::function<std::vector<Row>(const std::vector<Row> &, int)> sorted = [&](const std::vector<Row> &rows, int level) {
        std::vector<std::vector<Row>> blocks;
        for (const Row &r : rows) {
            if (r.level == level)
                blocks.push_back({r});
            else if (!blocks.empty())
                blocks.back().push_back(r);
        }
        std::vector<Row> heads;
        for (const auto &b : blocks)
            heads.push_back(b.front());
        sortRows(heads);
        std::vector<Row> out;
        for (const Row &h : heads) {
            for (const auto &b : blocks)
                if (b.front().item.path == h.item.path) {
                    out.push_back(b.front());
                    std::vector<Row> rest(b.begin() + 1, b.end());
                    std::vector<Row> inner = sorted(rest, level + 1);
                    out.insert(out.end(), inner.begin(), inner.end());
                    break;
                }
        }
        return out;
    };
    m_rows = sorted(m_rows, 0);
    m_selection.clear();
    for (size_t i = 0; i < m_rows.size(); ++i)
        for (const Item &s : selected)
            if (s.path == m_rows[i].item.path)
                m_selection.insert(int(i));
    m_anchor = m_selection.empty() ? -1 : *m_selection.begin();
    layout();
}

void ListView::toggle(int row)
{
    Row &r = m_rows[size_t(row)];
    if (!r.item.folder)
        return;
    std::vector<Item> selected = selectedItems();
    if (r.expanded) {
        size_t end = size_t(row) + 1;
        while (end < m_rows.size() && m_rows[end].level > r.level)
            ++end;
        m_rows.erase(m_rows.begin() + row + 1, m_rows.begin() + long(end));
        r.expanded = false;
    } else {
        std::vector<Row> children;
        for (Item &it : list(r.item.path))
            children.push_back({std::move(it), r.level + 1, false});
        sortRows(children);
        r.expanded = true;
        m_rows.insert(m_rows.begin() + row + 1, children.begin(), children.end());
    }
    m_selection.clear();
    for (size_t i = 0; i < m_rows.size(); ++i)
        for (const Item &s : selected)
            if (s.path == m_rows[i].item.path)
                m_selection.insert(int(i));
    m_anchor = m_selection.empty() ? -1 : *m_selection.begin();
    layout();
    if (onSelectionChanged)
        onSelectionChanged();
}

void ListView::select(int row, bool extend)
{
    if (!extend)
        m_selection.clear();
    if (row >= 0) {
        if (extend && m_selection.count(row))
            m_selection.erase(row);
        else
            m_selection.insert(row);
        m_anchor = row;
    }
    m_body->setNeedsDisplay();
    if (onSelectionChanged)
        onSelectionChanged();
}

void ListView::selectAll()
{
    for (int i = 0; i < int(m_rows.size()); ++i)
        m_selection.insert(i);
    m_body->setNeedsDisplay();
    if (onSelectionChanged)
        onSelectionChanged();
}

void ListView::reveal(int row)
{
    if (row >= 0)
        m_scroll->scrollToVisible({0, row * pitch(), frame().w, pitch()});
}

} // namespace Finder
