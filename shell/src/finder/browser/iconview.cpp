// SPDX-License-Identifier: LGPL-2.1-or-later
#include "iconview.h"
#include "labels.h"

#include "controls.h"
#include "foundation.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <strings.h>

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Finder {

namespace {

// The Finder's label constants (their defaults: FXDesktopLayoutGridCharCount, FXLayoutMinCharCount,
// FXLabelHorizontalPad, FXLabelVerticalPad, FXMaximumLabelLines, FXTightLabelLines below a grid
// spacing of 26)
constexpr int GridCharCount = 17, MinCharCount = 4, LabelHPad = 1, LabelVPad = 2, LabelLines = 2, TightLabelLines = 1;

double even(double v)
{
    v = std::floor(v);
    return (int(v) & 1) ? v + 1 : v;
}

void roundedRect(cairo_t *cr, const Rect &r, double radius)
{
    radius = std::min({radius, r.w / 2, r.h / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.right() - radius, r.y + radius, radius, -M_PI / 2, 0);
    cairo_arc(cr, r.right() - radius, r.bottom() - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, r.x + radius, r.bottom() - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// the text cut in the middle with "…" to fit (the label's last line)
std::string middleTruncated(const std::string &s, double width, const Font &font)
{
    if (textWidth(s, font) <= width)
        return s;
    // by UTF-8 characters
    std::vector<size_t> starts;
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            starts.push_back(i);
    size_t keep = starts.size();
    while (keep > 1) {
        --keep;
        const size_t head = (keep + 1) / 2, tail = keep - head;
        const std::string t = s.substr(0, starts[head]) + "…" + s.substr(starts[starts.size() - tail]);
        if (textWidth(t, font) <= width)
            return t;
    }
    return "…";
}

} // namespace

IconView::IconView() = default;

IconView::Metrics IconView::metrics() const
{
    // TDesktopViewController's -setTextSize: then -calculateGridSize, -updateIconMetrics
    Metrics m;
    const double cw = std::ceil(textWidth("0", font));
    m.lineHeight = std::ceil(lineHeight(font));
    m.pad = iconSize / 16;
    m.box = iconSize + 2 * m.pad;
    const double widest = GridCharCount * cw + 18;
    if (gridSpacing < 100) {
        const double least = std::max(MinCharCount * cw + 18, m.box);
        m.labelWidth = std::floor(least + gridSpacing * ((widest - least) / 100.0));
    } else {
        m.labelWidth = std::floor(widest);
    }
    // the label's lines: one beside a small icon, else two (one when the grid is tight); the item's
    // info a line more
    m.lines = (labelOnBottom || iconSize > 16) ? (gridSpacing < 26 ? TightLabelLines : LabelLines) : 1;
    m.labelHeight = std::floor(m.lines * m.lineHeight + (showItemInfo ? m.lineHeight : 0) + 2);
    if (labelOnBottom) {
        m.grid = {even(m.labelWidth + LabelHPad), even(m.box + m.labelHeight + LabelVPad + 4)};
    } else {
        // the label at the icon's right: the cell as wide as both, as high as the taller and some
        double h = std::max(m.labelHeight, m.box);
        if (iconSize <= 16)
            h += m.box <= LabelVPad + m.labelHeight ? LabelVPad * 2 : LabelVPad / 2;
        else
            h += LabelVPad * 3;
        m.grid = {even(m.labelWidth + LabelHPad + m.box + 2), even(h)};
    }
    return m;
}

int IconView::columns() const
{
    // TLayoutGrid's -calculateGridDimensions: the frame's width in whole cells
    return std::max(1, int(frame().w / metrics().grid.w));
}

std::vector<Point> IconView::gridSpots(size_t count) const
{
    // the grid's spots in reading order (as place() lays them out)
    const Metrics m = metrics();
    const int cols = std::max(1, int(frame().w / m.grid.w));
    std::vector<Point> out;
    for (size_t cell = 0; cell < count; ++cell) {
        const double left = (int(cell) % cols) * m.grid.w, top = (int(cell) / cols) * m.grid.h;
        out.push_back(labelOnBottom ? Point{std::round(left + m.grid.w / 2 + 4), std::round(top + 4 + m.box / 2)}
                                    : Point{std::round(left + 4 + m.box / 2), std::round(top + m.grid.h / 2 + 4)});
    }
    return out;
}

std::vector<Point> IconView::place(double width) const
{
    // the items with a place (.DS_Store's Iloc: their anchor) stay there; the others take the grid's
    // first free spots in reading order (TLayoutGrid's -nextOpenLocation:). A spot is TLayoutGrid's
    // -anchorAtGridLocation: the cell's centre, 4 to the right; the icon box's centre 4 under the
    // cell's top
    const Metrics m = metrics();
    const int cols = std::max(1, int(width / m.grid.w));
    // labels at the bottom: the cell's centre across, the icon box 4 under the cell's top; at the right:
    // the icon box 4 in from the cell's left, the cell's middle down (anchoredIconMaxBounds)
    auto spot = [&](int cell) {
        const double left = (cell % cols) * m.grid.w, top = (cell / cols) * m.grid.h;
        if (labelOnBottom)
            return Point{std::round(left + m.grid.w / 2 + 4), std::round(top + 4 + m.box / 2)};
        return Point{std::round(left + 4 + m.box / 2), std::round(top + m.grid.h / 2 + 4)};
    };
    auto cellOf = [&](Point a) {
        const Point first = spot(0);
        const int col = int(std::lround((a.x - first.x) / m.grid.w)), row = int(std::lround((a.y - first.y) / m.grid.h));
        return std::pair<int, int>{std::clamp(col, 0, cols - 1), std::max(row, 0)};
    };
    std::vector<Point> out(m_items.size());
    std::vector<bool> placed(m_items.size(), false);
    std::set<int> taken;
    if (arrangeBy != "none" && arrangeBy != "grid") {
        // kept arranged: the grid in the order chosen, the places ignored
        std::vector<size_t> order(m_items.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return arrangedBefore(m_items[a], m_items[b]); });
        for (size_t k = 0; k < order.size(); ++k)
            out[order[k]] = spot(int(k));
        return out;
    }
    for (size_t i = 0; i < m_items.size(); ++i) {
        auto it = m_positions.find(m_items[i].name);
        if (it == m_positions.end())
            continue;
        const auto [col, row] = cellOf(it->second);
        // snapped to the grid: the nearest spot
        out[i] = arrangeBy == "grid" ? spot(row * cols + col) : it->second;
        placed[i] = true;
        taken.insert(row * cols + col);
    }
    int cell = 0;
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (placed[i])
            continue;
        while (taken.count(cell))
            ++cell;
        out[i] = spot(cell++);
    }
    return out;
}

double IconView::contentHeight(double width) const
{
    const Metrics m = metrics();
    double bottom = 0;
    for (const Point &a : place(width))
        bottom = std::max(bottom, (labelOnBottom ? a.y - m.box / 2 : a.y - m.grid.h / 2) + m.grid.h);
    return bottom + 4;
}

void IconView::setPositions(std::map<std::string, Point> positions)
{
    m_positions = std::move(positions);
    m_anchors.clear();
    setNeedsDisplay();
}

void IconView::setFrame(const Rect &r)
{
    const bool resized = r.w != frame().w;
    View::setFrame(r);
    if (resized)
        m_anchors.clear();
}

Point IconView::anchor(int index) const
{
    if (m_anchors.size() != m_items.size())
        m_anchors = place(frame().w);
    Point a = m_anchors[size_t(index)];
    if (m_moving && m_selection.count(index)) {
        a.x += m_moveBy.x;
        a.y += m_moveBy.y;
    }
    return a;
}

Rect IconView::iconRect(int index) const
{
    const Point a = anchor(index);
    return {a.x - iconSize / 2.0, a.y - iconSize / 2.0, double(iconSize), double(iconSize)};
}

Rect IconView::labelRect(int index) const
{
    // anchoredMaxLabelBounds: 2 under the icon box, or 2 right of it, its middle the icon's
    const Metrics m = metrics();
    const Point a = anchor(index);
    if (labelOnBottom)
        return {a.x - m.labelWidth / 2, a.y - m.box / 2 + m.box + 2, m.labelWidth, m.labelHeight};
    return {a.x - m.box / 2 + m.box + 2, std::round(a.y - m.labelHeight / 2), m.labelWidth, m.labelHeight};
}

std::string IconView::itemInfo(const Item &item) const
{
    // "Show item info": a folder's item count, a picture's size in pixels
    static std::map<std::string, std::string> cache;
    auto it = cache.find(item.path);
    if (it != cache.end())
        return it->second;
    std::string info;
    const std::string core = "/System/Library/CoreServices/Finder.app";
    if (item.folder) {
        const size_t n = list(item.path).size();
        std::string f = n == 1 ? Assets::tr(core, "LocalizableCore", "SB_COUNT_V2", "1 item")
                        : n == 0 ? Assets::tr(core, "LocalizableCore", "SB_COUNT_V1", "0 items")
                                 : Assets::tr(core, "LocalizableCore", "SB_COUNT_V3", "^0 items");
        const size_t at = f.find("^0");
        info = at == std::string::npos ? f : f.replace(at, 2, std::to_string(n));
    } else {
        const Size s = Image::sizeOf(item.path);
        if (!s.empty())
            info = std::to_string(int(s.w)) + " × " + std::to_string(int(s.h));
    }
    if (cache.size() > 2000)
        cache.clear();
    return cache[item.path] = info;
}

bool IconView::arrangedBefore(const Item &a, const Item &b) const
{
    // Arrange By: names A to Z, dates and sizes the newest and largest first, kinds A to Z
    if (arrangeBy == "dateModified" || arrangeBy == "dateCreated") {
        const long long ta = modificationTime(a), tb = modificationTime(b);
        if (ta != tb)
            return ta > tb;
    } else if (arrangeBy == "size") {
        const long long sa = a.folder ? -1 : sizeOnDisk(a), sb = b.folder ? -1 : sizeOnDisk(b);
        if (sa != sb)
            return sa > sb;
    } else if (arrangeBy == "kind") {
        const std::string ka = kind(a), kb = kind(b);
        if (ka != kb)
            return finderLess(ka, kb);
    }
    return finderLess(a.name, b.name);
}

std::vector<std::string> IconView::labelLines(const std::string &name, double width, int lines) const
{
    // the first line takes the words that fit (a word too long for it is cut where it stops fitting);
    // the second the rest, cut in the middle when it doesn't fit; one line: the name cut in the middle
    if (textWidth(name, font) <= width)
        return {name};
    if (lines < 2)
        return {middleTruncated(name, width, font)};
    size_t best = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != ' ' && name[i] != '-' && name[i] != '_' && name[i] != '.')
            continue;
        const size_t end = name[i] == ' ' ? i : i + 1;
        if (textWidth(name.substr(0, end), font) <= width)
            best = i + 1;
        else
            break;
    }
    if (best == 0) {
        for (size_t i = 1; i <= name.size(); ++i) {
            if (i < name.size() && (static_cast<unsigned char>(name[i]) & 0xC0) == 0x80)
                continue;
            if (textWidth(name.substr(0, i), font) > width)
                break;
            best = i;
        }
        best = std::max<size_t>(best, 1);
    }
    std::string first = name.substr(0, best), rest = name.substr(best);
    while (!first.empty() && first.back() == ' ')
        first.pop_back();
    return {first, middleTruncated(rest, width, font)};
}

void IconView::setItems(std::vector<Item> items)
{
    m_items = std::move(items);
    m_anchors.clear();
    m_selection.clear();
    m_anchor = -1;
    setNeedsDisplay();
    if (onSelectionChanged)
        onSelectionChanged();
}

void IconView::selectAll()
{
    for (int i = 0; i < int(m_items.size()); ++i)
        m_selection.insert(i);
    setNeedsDisplay();
    if (onSelectionChanged)
        onSelectionChanged();
}

void IconView::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(255));
    const Metrics m = metrics();
    const bool key = windowIsKey() && isFirstResponder();
    for (int i = 0; i < int(m_items.size()); ++i) {
        const Rect lr = labelRect(i);
        if (lr.bottom() < 0 || lr.y - m.box > frame().h)
            continue;
        const bool selected = m_selection.count(i) > 0;
        const Rect ir = iconRect(i);
        if (selected) {
            // the icon's selection: its box (anchoredSelectionRectBounds, the icon grown by pad), a
            // translucent dark rounded square
            const Rect sr = ir.inset(-m.pad, -m.pad);
            roundedRect(cr, sr, 5);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.2);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        const Image img = icon(m_items[size_t(i)], iconSize, showIconPreview);
        if (!img.isNull()) {
            const Size s = scaledToFit(img.size(), {double(iconSize), double(iconSize)});
            // a preview sits on the icon's bottom, centred across
            img.draw(cr, Rect{std::round(ir.x + (ir.w - s.w) / 2), std::round(ir.bottom() - s.h), s.w, s.h});
        }
        const std::vector<std::string> lines = labelLines(m_items[size_t(i)].name, m.labelWidth - 6, m.lines);
        TextStyle t;
        t.font = font;
        t.align = labelOnBottom ? Align::Center : Align::Left;
        t.elide = false;
        const int label = Labels::get(m_items[size_t(i)].path);
        for (size_t n = 0; n < lines.size(); ++n) {
            const Rect line{lr.x, lr.y + 1 + n * m.lineHeight, lr.w, m.lineHeight};
            if (!selected && label) {
                // a labelled item: its band in the label's colour (TDesktopIcon -drawSingleLineLabelBackground:...labelIndex:)
                const double w = std::ceil(textWidth(lines[n], font)) + 10;
                const Rect band{labelOnBottom ? std::round(line.x + (line.w - w) / 2) : line.x - 5, line.y, w, m.lineHeight};
                const Color c = Labels::color(label);
                cairo_pattern_t *g = cairo_pattern_create_linear(0, band.y, 0, band.bottom());
                cairo_pattern_add_color_stop_rgb(g, 0, std::min(1.0, c.r + 0.15), std::min(1.0, c.g + 0.15), std::min(1.0, c.b + 0.15));
                cairo_pattern_add_color_stop_rgb(g, 1, c.r, c.g, c.b);
                roundedRect(cr, band, m.lineHeight / 2);
                cairo_set_source(cr, g);
                cairo_fill(cr);
                cairo_pattern_destroy(g);
            }
            if (selected) {
                // the label's selection: a rounded band hugging the text, the selected text colour on it
                const double w = std::ceil(textWidth(lines[n], font)) + 10;
                const Rect band{labelOnBottom ? std::round(line.x + (line.w - w) / 2) : line.x - 5, line.y, w, m.lineHeight};
                roundedRect(cr, band, m.lineHeight / 2);
                (key ? Color::rgb(56, 117, 215) : Color::gray(212)).set(cr);
                cairo_fill(cr);
            }
            t.color = selected && key ? Color::gray(255) : Color::gray(0);
            drawText(cr, lines[n], line, t);
        }
        if (showItemInfo) {
            const std::string info = itemInfo(m_items[size_t(i)]);
            if (!info.empty()) {
                TextStyle it = t;
                it.color = Color::rgb(36, 92, 198);   // the info's blue
                drawText(cr, info, Rect{lr.x, lr.y + 1 + lines.size() * m.lineHeight, lr.w, m.lineHeight}, it);
            }
        }
    }
    if (m_marquee) {
        // the selection rectangle
        const Rect r{std::min(m_marqueeStart.x, m_marqueeEnd.x), std::min(m_marqueeStart.y, m_marqueeEnd.y),
                     std::abs(m_marqueeEnd.x - m_marqueeStart.x), std::abs(m_marqueeEnd.y - m_marqueeStart.y)};
        fill(cr, r, Color::gray(0, 0.15));
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, std::max(0.0, r.w - 1), std::max(0.0, r.h - 1));
        cairo_stroke(cr);
    }
}

int IconView::itemAt(Point p) const
{
    const Metrics m = metrics();
    for (int i = 0; i < int(m_items.size()); ++i) {
        if (iconRect(i).inset(-m.pad, -m.pad).contains(p))
            return i;
        // the label's text, not its whole place
        const Rect lr = labelRect(i);
        const std::vector<std::string> lines = labelLines(m_items[size_t(i)].name, m.labelWidth - 6, m.lines);
        for (size_t n = 0; n < lines.size(); ++n) {
            const double w = std::ceil(textWidth(lines[n], font)) + 10;
            const double x = labelOnBottom ? lr.x + (lr.w - w) / 2 : lr.x - 5;
            if (Rect{x, lr.y + 1 + n * m.lineHeight, w, m.lineHeight}.contains(p))
                return i;
        }
    }
    return -1;
}

void IconView::select(int index, bool extend)
{
    if (!extend)
        m_selection.clear();
    if (index >= 0) {
        if (extend && m_selection.count(index))
            m_selection.erase(index);
        else
            m_selection.insert(index);
        m_anchor = index;
    }
    setNeedsDisplay();
    if (onSelectionChanged)
        onSelectionChanged();
}

void IconView::revealItem(int index)
{
    if (auto *sv = dynamic_cast<ScrollView *>(superview() ? superview()->superview() : nullptr)) {
        const Rect ir = iconRect(index), lr = labelRect(index);
        sv->scrollToVisible({ir.x, ir.y - 8, ir.w, lr.bottom() - ir.y + 12});
    }
}

void IconView::mouseDown(Event &e)
{
    if (window())
        window()->makeFirstResponder(this);
    const int i = itemAt(e.location);
    const bool extend = e.modifiers & (Command | Shift);
    m_pressAt = e.location;
    m_clickedName = -1;
    if (i >= 0) {
        if (e.clickCount == 2 && !extend) {
            if (onOpen)
                onOpen(m_items[size_t(i)]);
            return;
        }
        m_clickedName = !extend && e.clickCount == 1 && m_selection.size() == 1 && m_selection.count(i) && labelRect(i).contains(e.location) ? i : -1;
        if (extend || !m_selection.count(i))
            select(i, extend);
        // a drag moves the selected icons (not when the folder is kept arranged)
        if (m_selection.count(i) && (arrangeBy == "none" || arrangeBy == "grid")) {
            m_moving = true;
            m_moveFrom = e.location;
            m_moveBy = {0, 0};
        }
        return;
    }
    if (!extend)
        select(-1, false);
    m_marquee = true;
    m_marqueeStart = m_marqueeEnd = e.location;
    m_marqueeBase = m_selection;
}

void IconView::mouseDragged(Event &e)
{
    if (std::abs(e.location.x - m_pressAt.x) > 3 || std::abs(e.location.y - m_pressAt.y) > 3)
        m_clickedName = -1;
    if (m_moving) {
        m_moveBy = {e.location.x - m_moveFrom.x, e.location.y - m_moveFrom.y};
        setNeedsDisplay();
        return;
    }
    if (!m_marquee)
        return;
    m_marqueeEnd = e.location;
    const Rect r{std::min(m_marqueeStart.x, m_marqueeEnd.x), std::min(m_marqueeStart.y, m_marqueeEnd.y),
                 std::abs(m_marqueeEnd.x - m_marqueeStart.x), std::abs(m_marqueeEnd.y - m_marqueeStart.y)};
    auto meets = [](const Rect &a, const Rect &b) { return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom(); };
    const Metrics m = metrics();
    m_selection = m_marqueeBase;
    for (int i = 0; i < int(m_items.size()); ++i)
        if (meets(r, iconRect(i).inset(-m.pad, -m.pad)) || meets(r, labelRect(i)))
            m_selection.insert(i);
    setNeedsDisplay();
    if (onSelectionChanged)
        onSelectionChanged();
}

void IconView::mouseUp(Event &e)
{
    if (m_clickedName >= 0 && size_t(m_clickedName) < m_items.size() && onClickName) {
        const Item item = m_items[size_t(m_clickedName)];
        m_clickedName = -1;
        onClickName(item);
    }
    m_clickedName = -1;
    if (m_moving && std::abs(m_moveBy.x) + std::abs(m_moveBy.y) >= 3) {
        // dropped on a folder that isn't moving with it, or outside the view: the files move
        const int target = itemAt(e.location);
        const bool option = e.modifiers & Option;
        if (target >= 0 && !m_selection.count(target) && m_items[size_t(target)].folder && onDropOnFolder) {
            m_moving = false;
            m_moveBy = {0, 0};
            setNeedsDisplay();
            onDropOnFolder(m_items[size_t(target)], option);
            return;
        }
        if (!bounds().contains(e.location) && onDropOutside) {
            m_moving = false;
            m_moveBy = {0, 0};
            setNeedsDisplay();
            onDropOutside(e.screen, option);
            return;
        }
    }
    if (m_moving) {
        m_moving = false;
        if (m_anchors.size() != m_items.size())
            m_anchors = place(frame().w);
        if (std::abs(m_moveBy.x) + std::abs(m_moveBy.y) >= 3) {
            // the icons where they were dropped: their new places
            std::vector<std::pair<std::string, Point>> moved;
            for (int i : m_selection) {
                Point a = m_anchors[size_t(i)];
                a = {std::round(a.x + m_moveBy.x), std::round(std::max(a.y + m_moveBy.y, metrics().box / 2))};
                a.x = std::max(a.x, double(iconSize) / 2);
                m_positions[m_items[size_t(i)].name] = a;
                moved.emplace_back(m_items[size_t(i)].name, a);
            }
            m_anchors.clear();
            if (arrangeBy == "grid") {
                // their snapped places
                m_anchors = place(frame().w);
                for (auto &mv : moved)
                    for (size_t k = 0; k < m_items.size(); ++k)
                        if (m_items[k].name == mv.first)
                            m_positions[mv.first] = mv.second = m_anchors[k];
            }
            if (onMoved)
                onMoved(moved);
        }
        m_moveBy = {0, 0};
        setNeedsDisplay();
        return;
    }
    if (m_marquee) {
        m_marquee = false;
        setNeedsDisplay();
    }
}

void IconView::keyDown(Event &e)
{
    const int n = int(m_items.size());
    if (e.modifiers & Command) {
        if (e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_o) {
            if (m_selection.size() == 1 && onOpen)
                onOpen(m_items[size_t(*m_selection.begin())]);
            return;
        }
        if (e.keysym == XKB_KEY_Up) {
            if (onBack)
                onBack();
            return;
        }
        if (e.keysym == XKB_KEY_a) {
            selectAll();
            return;
        }
        View::keyDown(e);
        return;
    }
    if (n == 0) {
        View::keyDown(e);
        return;
    }
    const int cols = columns();
    const int from = m_anchor >= 0 ? m_anchor : -1;
    int to = from;
    switch (e.keysym) {
    case XKB_KEY_Right: to = from < 0 ? 0 : std::min(n - 1, from + 1); break;
    case XKB_KEY_Left: to = from < 0 ? 0 : std::max(0, from - 1); break;
    case XKB_KEY_Down: to = from < 0 ? 0 : (from + cols < n ? from + cols : from); break;
    case XKB_KEY_Up: to = from < 0 ? 0 : (from - cols >= 0 ? from - cols : from); break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        View::keyDown(e);   // the window's: renaming, Quick Look
        return;
    default:
        if (!e.text.empty() && static_cast<unsigned char>(e.text[0]) >= 0x20) {
            // type-select: the first item whose name starts with what was typed
            for (int i = 0; i < n; ++i)
                if (strncasecmp(m_items[size_t(i)].name.c_str(), e.text.c_str(), e.text.size()) == 0) {
                    select(i, false);
                    revealItem(i);
                    return;
                }
            return;
        }
        View::keyDown(e);
        return;
    }
    if (to >= 0) {
        select(to, false);
        revealItem(to);
    }
}

void IconView::selectPaths(const std::vector<std::string> &paths)
{
    m_selection.clear();
    int first = -1;
    for (size_t i = 0; i < m_items.size(); ++i)
        if (std::find(paths.begin(), paths.end(), m_items[i].path) != paths.end()) {
            m_selection.insert(int(i));
            if (first < 0)
                first = int(i);
        }
    m_anchor = first;
    if (first >= 0)
        revealItem(first);
    setNeedsDisplay();
    if (onSelectionChanged)
        onSelectionChanged();
}

Rect IconView::nameRectInWindow(const std::string &path) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].path == path) {
            const Rect r = labelRect(int(i));
            const Metrics m = metrics();
            const Point o = toWindow({r.x, r.y});
            return {o.x, o.y, r.w, m.lineHeight + 4};
        }
    return {};
}

void IconView::rightMouseDown(Event &e)
{
    // the item under the pointer joins the selection (alone, unless it was selected already)
    const int i = itemAt(e.location);
    if (i >= 0 && !m_selection.count(i))
        select(i, false);
    else if (i < 0 && !m_selection.empty())
        select(-1, false);
    if (onContextMenu)
        onContextMenu(toWindow(e.location), i >= 0);
}

std::vector<std::pair<std::string, Point>> IconView::cleanUp(bool selectionOnly) const
{
    // TIconViewController -cmdCleanUp: each icon to the nearest free place of the grid
    const std::vector<Point> grid = gridSpots(m_items.size() * 2 + 64);
    std::vector<std::pair<std::string, Point>> out;
    std::vector<bool> taken(grid.size(), false);
    auto nearest = [&](Point p) {
        int best = -1;
        double bestD = 1e18;
        for (size_t g = 0; g < grid.size(); ++g) {
            if (taken[g])
                continue;
            const double dx = grid[g].x - p.x, dy = grid[g].y - p.y, d = dx * dx + dy * dy;
            if (d < bestD) {
                bestD = d;
                best = int(g);
            }
        }
        return best;
    };
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (selectionOnly && !m_selection.count(int(i)))
            continue;
        const int g = nearest(anchor(int(i)));
        if (g < 0)
            continue;
        taken[size_t(g)] = true;
        out.push_back({m_items[i].name, grid[size_t(g)]});
    }
    return out;
}

std::vector<std::pair<std::string, Point>> IconView::arrangedPlaces(const std::string &by) const
{
    std::vector<int> order(m_items.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    IconView sorter;
    sorter.arrangeBy = by;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return sorter.arrangedBefore(m_items[size_t(a)], m_items[size_t(b)]); });
    const std::vector<Point> grid = gridSpots(m_items.size());
    std::vector<std::pair<std::string, Point>> out;
    for (size_t k = 0; k < order.size() && k < grid.size(); ++k)
        out.push_back({m_items[size_t(order[k])].name, grid[k]});
    return out;
}

} // namespace Finder
