// SPDX-License-Identifier: LGPL-2.1-or-later
#include "sidebar.h"

#include "foundation.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Preview {

namespace {

const char *const Bundle = "/Applications/Preview.app";
// -[PVWindowController setupThumbnailView] (0x10000f1f4): IKImageBrowserBackgroundColorKey calibrated
// 0.9 0.92 0.96 (over the nib's clip view, 0.844 0.875 0.903); the layout manager's margin 10 across,
// 15 down, cell margin 20 x 20; cells shadowed and titled (setCellsStyleMask: 5)
const Color ThumbnailBackground{0.9, 0.92, 0.96, 1};
constexpr double MarginX = 10, MarginY = 15, CellMargin = 20;
// the title space under each thumbnail: its height clamped to 20 at least (+[IKImageBrowserView
// textHeightForAttributes:clamp:], ImageKit 0x2fed2); the title's own, Lucida Grande 12's line
constexpr double LabelHeight = 20, TitleHeight = 15;
const char *const ImageKitResources = "/System/Library/Frameworks/Quartz.framework/Versions/A/Frameworks/ImageKit.framework/Versions/A/Resources/";
// PVDocument.nib's PVOutlineView (object 133), the table of contents and the search's results: no
// header, rows 17 high and 2 apart (NSRowHeight, NSIntercellSpacing 3 x 2), Lucida Grande 13, on
// NSCalibratedRGB 0.906 0.918 0.961; its "relevance" column 64 wide
const Color OutlineBackground{0.9058823586, 0.9176470637, 0.9607843161, 1};
constexpr double OutlineRowHeight = 17 + 2, RelevanceWidth = 64;

Image resource(const std::string &name)
{
    return Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/" + name));
}

void roundedRect(cairo_t *cr, const Rect &r, double radius)
{
    const double x = r.x, y = r.y, w = r.w, h = r.h, rr = std::min({radius, w / 2, h / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rr, y + rr, rr, -M_PI / 2, 0);
    cairo_arc(cr, x + w - rr, y + h - rr, rr, 0, M_PI / 2);
    cairo_arc(cr, x + rr, y + h - rr, rr, M_PI / 2, M_PI);
    cairo_arc(cr, x + rr, y + rr, rr, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// Preview's "%@" strings with their values, in order
std::string withValues(const std::string &f, const std::vector<std::string> &values)
{
    std::string out;
    size_t v = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f.compare(i, 2, "%@") == 0) {
            out += v < values.size() ? values[v++] : std::string();
            ++i;
        } else {
            out += f[i];
        }
    }
    return out;
}

} // namespace

// ---- the thumbnails

int ThumbnailView::columns(double width) const
{
    if (!contactSheet)
        return 1;
    return std::max(1, int((width - 2 * MarginX) / (thumbnailSize + CellMargin)));
}

double ThumbnailView::heightFor(double width) const
{
    const int cols = columns(width);
    const int rows = (m_doc->pageCount() + cols - 1) / cols;
    return 2 * MarginY - CellMargin + rows * (thumbnailSize + LabelHeight + CellMargin);
}

// a cell with half the cell margin around it: the first thumbnail MarginY down
Rect ThumbnailView::cellRect(int page, double width) const
{
    const int cols = columns(width);
    const double cellW = contactSheet ? (width - 2 * MarginX) / cols : width;
    const double cellH = thumbnailSize + LabelHeight + CellMargin;
    return {std::floor((page % cols) * cellW + (contactSheet ? MarginX : 0)), MarginY - CellMargin / 2 + (page / cols) * cellH, std::floor(cellW),
            cellH};
}

const Image &ThumbnailView::thumbnail(int page)
{
    if (m_cacheSize != thumbnailSize) {
        m_cache.clear();
        m_cacheSize = thumbnailSize;
    }
    auto it = m_cache.find(page);
    if (it == m_cache.end()) {
        const Size s = m_doc->pageSize(page);
        const double scale = thumbnailSize / std::max(s.w, s.h);
        it = m_cache.emplace(page, m_doc->render(page, scale)).first;
    }
    return it->second;
}

// IKImageBrowserView's shadow under a thumbnail (-[IKCGRenderer drawRectShadow:withAlpha:], ImageKit
// 0x4afd9; IKShadowTool's pictures): 3 at the sides, 5 under, the corners 10 wide, the top 9 of each
// side fading in; only for a picture at least 14 x 12 (-[IKImageBrowserCell drawShadow], 0x2ceee)
void ThumbnailView::drawShadow(cairo_t *cr, const Rect &r)
{
    static const char *const Names[] = {"TopLeft", "Left", "BottomLeft", "Bottom", "BottomRight", "Right", "TopRight"};
    static std::vector<Image> parts;
    if (parts.empty())
        for (const char *n : Names)
            parts.push_back(Image::load(Assets::path(std::string(ImageKitResources) + "pbShadow" + n + ".tiff")));
    if (r.w < 14 || r.h < 12 || parts[0].isNull())
        return;
    auto w = [&](int i) { return double(parts[size_t(i)].width()); };
    auto h = [&](int i) { return double(parts[size_t(i)].height()); };
    parts[0].draw(cr, Rect{r.x - w(0), r.y, w(0), h(0)});
    parts[1].draw(cr, Rect{r.x - w(1), r.y + h(0), w(1), r.h - h(0)});
    parts[2].draw(cr, Rect{r.x - w(1), r.bottom(), w(2), h(2)});
    parts[6].draw(cr, Rect{r.right(), r.y, w(6), h(6)});
    parts[5].draw(cr, Rect{r.right(), r.y + h(6), w(5), r.h - h(6)});
    const double brX = r.right() + w(5) - w(4);
    parts[4].draw(cr, Rect{brX, r.bottom(), w(4), h(4)});
    const double bX = r.x - w(1) + w(2);
    parts[3].draw(cr, Rect{bX, r.bottom(), brX - bX, h(3)});
}

void ThumbnailView::draw(cairo_t *cr)
{
    fill(cr, bounds(), ThumbnailBackground);
    // only the cells in the clip (the scroll view's)
    double cx1, cy1, cx2, cy2;
    cairo_clip_extents(cr, &cx1, &cy1, &cx2, &cy2);
    const bool focus = windowIsKey() && isFirstResponder();
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        const Rect cell = cellRect(i, frame().w);
        if (cell.bottom() < cy1 || cell.y > cy2)
            continue;
        const Image &img = thumbnail(i);
        const double tw = img.width(), th = img.height();
        // the image centred in its square (imageAlignment 0), the title space under it
        const Rect box{cell.x + std::floor((cell.w - thumbnailSize) / 2), cell.y + CellMargin / 2, thumbnailSize, thumbnailSize};
        const Rect pic{box.x + std::floor((box.w - tw) / 2), box.y + std::floor((box.h - th) / 2), tw, th};
        // -[IKImageBrowserCell selectionFrame] (0x2d593): the picture grown by 4, made square on its
        // longer side; -[IKImageBrowserCell drawSelection] (0x2be0c): black at 0.2, rounded min(side/2, 8)
        Rect sel = pic.inset(-4, -4);
        if (sel.h >= sel.w) {
            sel.x -= std::floor((sel.h - sel.w) / 2);
            sel.w = sel.h;
        } else {
            sel.y -= std::floor((sel.w - sel.h) / 2);
            sel.h = sel.w;
        }
        if (i == selected) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.2);
            roundedRect(cr, sel, std::min(sel.w / 2, 8.0));
            cairo_fill(cr);
        }
        drawShadow(cr, pic);
        img.draw(cr, Point{pic.x, pic.y});
        // the title (-[IKImageBrowserCell titleFrame], 0x2c877): Lucida Grande 12 centred, 1 under the
        // selection's square; selected, on a rounded rect 6 wider each side and 1 less high
        // (__titleRoundedRectForRect, drawSelectionOnTitle 0x2f1b4): alternateSelectedControlColor with
        // white text when the browser has the focus, grey 0.8 and black text without
        // (_systemColorsDidChange 0x105313, unfocusedSelectionColor 0x10524c, _setDefaultTextAttributes
        // 0x104617)
        const std::string label = m_doc->pageLabel(i);
        TextStyle t;
        t.font = Font::system(12);
        t.align = Align::Center;
        t.color = Color::gray(0);
        const double lw = std::ceil(textWidth(label, t.font));
        const Rect lr{cell.x + std::floor((cell.w - lw) / 2), sel.bottom() + 1, lw, TitleHeight};
        if (i == selected) {
            const Rect badge{lr.x - 6, lr.y, lr.w + 12, lr.h - 1};
            if (focus)
                Color::rgb(56, 117, 215).set(cr);
            else
                Color::gray(204).set(cr);
            roundedRect(cr, badge, std::ceil(badge.h / 2));
            cairo_fill(cr);
            if (focus)
                t.color = Color::gray(255);
        }
        drawText(cr, label, Rect{lr.x - 6, lr.y, lr.w + 12, lr.h - 1}, t);
    }
}

void ThumbnailView::select(int page)
{
    if (page == selected)
        return;
    selected = page;
    setNeedsDisplay();
    reveal();
}

void ThumbnailView::reveal()
{
    auto *sv = dynamic_cast<ScrollView *>(superview() ? superview()->superview() : nullptr);
    if (sv && selected >= 0 && sv->frame().h > 0)
        sv->scrollToVisible(cellRect(selected, frame().w));
}

void ThumbnailView::mouseDown(Event &e)
{
    for (int i = 0; i < m_doc->pageCount(); ++i)
        if (cellRect(i, frame().w).contains(e.location)) {
            select(i);
            if (onSelect)
                onSelect(i);
            return;
        }
}

void ThumbnailView::keyDown(Event &e)
{
    const int cols = columns(frame().w);
    int to = selected;
    switch (e.keysym) {
    case XKB_KEY_Up: to -= cols; break;
    case XKB_KEY_Down: to += cols; break;
    case XKB_KEY_Left: to -= 1; break;
    case XKB_KEY_Right: to += 1; break;
    case XKB_KEY_Home: to = 0; break;
    case XKB_KEY_End: to = m_doc->pageCount() - 1; break;
    default: View::keyDown(e); return;
    }
    to = std::clamp(to, 0, m_doc->pageCount() - 1);
    select(to);
    if (onSelect)
        onSelect(to);
}

// ---- the bottom bar

// _utilityBottomView: BottomTB_toolbarButtonBackground across, the PVGradientToolbarSegmentedCell at
// the right (129 wide: four 32 point segments on SidebarButtonBackground, SidebarButtonEdge between),
// the small thumbnail slider at 23 in (contact sheet mode only)
class BottomBar : public View {
public:
    static constexpr double ControlWidth = 129, SegmentWidth = 32;
    BottomBar()
    {
        m_background = resource("BottomTB_toolbarButtonBackground.png");
        m_button = resource("SidebarButtonBackground.tiff");
        m_pressed = resource("SidebarButtonBackground_Pressed.tiff");
        m_edge = resource("SidebarButtonEdge.tiff");
        for (const char *n : {"BottomSidebar_ContactSheet.png", "BottomSidebar_Thumbnails.png", "BottomSidebar_TableOfContents.png",
                              "BottomSidebar_Annotations.png"})
            m_icons.push_back(resource(n));
        slider = add<Slider>();
        slider->small = true;
        slider->setFrame({23, 4, 100, 15});
        slider->setHidden(true);
    }
    Slider *slider;
    int selected = 1;
    std::vector<bool> enabled{true, true, true, false};
    std::function<void(int)> onSelect;

    Rect segment(int i) const
    {
        const double x0 = frame().w - 20 - ControlWidth;
        return {x0 + i * (SegmentWidth + 1), 0, SegmentWidth, BottomBarHeight};
    }
    void draw(cairo_t *cr) override
    {
        m_background.draw(cr, bounds());
        for (int i = 0; i < 4; ++i) {
            const Rect r = segment(i);
            (i == selected || i == m_down ? m_pressed : m_button).draw(cr, r);
            m_edge.draw(cr, Rect{r.x - 1, 0, 1, BottomBarHeight});
            const Image &icon = m_icons[size_t(i)];
            icon.draw(cr, Rect{r.x + std::floor((r.w - icon.width()) / 2), std::floor((BottomBarHeight - icon.height()) / 2), double(icon.width()),
                               double(icon.height())},
                      enabled[size_t(i)] ? 1 : 0.35);
        }
        m_edge.draw(cr, Rect{segment(3).right(), 0, 1, BottomBarHeight});
    }
    void mouseDown(Event &e) override
    {
        for (int i = 0; i < 4; ++i)
            if (segment(i).contains(e.location) && enabled[size_t(i)])
                m_down = i;
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        const int down = m_down;
        m_down = -1;
        setNeedsDisplay();
        if (down >= 0 && segment(down).contains(e.location) && onSelect)
            onSelect(down);
    }

private:
    Image m_background, m_button, m_pressed, m_edge;
    std::vector<Image> m_icons;
    int m_down = -1;
};

// ---- the search's results

// _outlineView once -[PVWindowController displaySearchResults:isDone:] (0x1000d83a0) has filtered it:
// the document's group row, then one row per page with a match (pagesSeen), labelled with the table
// of contents' entry the page is in, "%@ (Page %@)" when several pages share it, or "Page %@"
// without contents; the relevance column (addRelevanceColumn) at the right, its bar against the best
// page's (setMaxValue: maxRelevanceScore). PDFKit's relevance is Search Kit's: here the page's matches.
class ResultsView : public OutlineView {
public:
    std::vector<double> relevance;   // by row; < 0: none
    void draw(cairo_t *cr) override
    {
        OutlineView::draw(cr);
        double best = 0;
        for (double r : relevance)
            best = std::max(best, r);
        const std::vector<int> vis = visibleRows();
        for (size_t n = 0; n < vis.size(); ++n) {
            const size_t row = size_t(vis[n]);
            if (row >= relevance.size() || relevance[row] <= 0 || best <= 0)
                continue;
            // -[NSLevelIndicatorCell _drawRelevancyWithFrame:inView:] (AppKit 0x424d17): the cell's
            // whole height, its width times value / max, filled with the theme's relevancy pattern
            // (_NSGetThemePattern 0x95, AppKit's theme art, not read: thin upright strokes 2 apart here)
            const bool white = vis[n] == selected() && isFirstResponder() && windowIsKey();
            const double cellW = RelevanceWidth - 3, x0 = frame().w - RelevanceWidth;   // NSIntercellSpacing 3 x 2
            const double w = std::floor(cellW * std::min(1.0, relevance[row] / best));
            const double y = double(n) * rowHeight + 1;
            cairo_set_source_rgb(cr, white ? 1 : 0.45, white ? 1 : 0.45, white ? 1 : 0.45);
            for (double x = 0; x < w; x += 2)
                cairo_rectangle(cr, x0 + x, y, 1, rowHeight - 2);
            cairo_fill(cr);
        }
    }
};

// ---- the sidebar

Sidebar::Sidebar(const Document *doc) : m_doc(doc)
{
    m_thumbScroll = add<ScrollView>();
    m_thumbs = m_thumbScroll->document()->add<ThumbnailView>(doc);
    m_thumbs->onSelect = [this](int page) {
        if (onPage)
            onPage(page);
    };
    m_outline = doc->outline();
    m_outlineScroll = add<ScrollView>();
    m_outlineView = m_outlineScroll->document()->add<OutlineView>();
    m_outlineView->background = OutlineBackground;
    m_outlineView->rowHeight = OutlineRowHeight;
    // the outline's roots: the document's PVContainerOutlineRoot, a group row
    // (outlineView:isGroupItem:, 0x100056293), its entries under it; a label's line breaks become
    // spaces (outlineView:objectValueForTableColumn:byItem:, 0x100056d1c)
    for (Document::OutlineItem &item : m_outline)
        for (char &c : item.title)
            if (c == '\n' || c == '\r')
                c = ' ';
    if (!m_outline.empty()) {
        OutlineRow root;
        root.title = fileName(doc->path());
        root.groupRow = true;
        root.group = true;
        root.selectable = false;
        root.tag = -1;
        m_outlineView->rows.push_back(root);
    }
    std::vector<int> parents{0};
    for (const Document::OutlineItem &item : m_outline) {
        OutlineRow row;
        row.title = item.title;
        row.level = item.level + 1;
        row.tag = item.page;
        parents.resize(size_t(item.level + 1));
        row.parent = parents.back();
        parents.push_back(int(m_outlineView->rows.size()));
        m_outlineView->rows.push_back(row);
    }
    for (size_t i = 0; i < m_outlineView->rows.size(); ++i)
        if (i + 1 < m_outlineView->rows.size() && m_outlineView->rows[i + 1].level > m_outlineView->rows[i].level)
            m_outlineView->rows[i].group = true;
    m_outlineView->onSelect = [this](int row) {
        if (row >= 0 && onPage && m_outlineView->rows[size_t(row)].tag >= 0)
            onPage(m_outlineView->rows[size_t(row)].tag);
    };
    m_resultsScroll = add<ScrollView>();
    m_results = m_resultsScroll->document()->add<ResultsView>();
    m_results->background = OutlineBackground;
    m_results->rowHeight = OutlineRowHeight;
    m_results->trailing = RelevanceWidth;
    m_results->onSelect = [this](int row) {
        if (onMatch && row >= 0 && size_t(row) < m_resultMatch.size() && m_resultMatch[size_t(row)] >= 0)
            onMatch(m_resultMatch[size_t(row)]);
    };
    m_bar = add<BottomBar>();
    m_bar->enabled[2] = hasOutline();
    m_bar->onSelect = [this](int i) { setMode(Mode(i)); };
    m_bar->slider->minValue = 64;
    m_bar->slider->maxValue = 512;
    m_bar->slider->setDoubleValue(160);
    m_bar->slider->onChange = [this](double) { sizeThumbnails(); };
    setMode(Thumbnails);
}

void Sidebar::setMode(Mode m)
{
    if (m != SearchResults)
        m_before = m;
    m_mode = m;
    m_thumbs->contactSheet = m == ContactSheet;
    m_bar->selected = m == SearchResults ? -1 : int(m);
    m_bar->slider->setHidden(m != ContactSheet);
    layout();
    if (onModeChanged)
        onModeChanged(m);
}

void Sidebar::setPage(int page)
{
    m_thumbs->select(page);
    // the contents' entry the page is in
    int best = -1;
    for (size_t i = 0; i < m_outlineView->rows.size(); ++i)
        if (m_outlineView->rows[i].tag >= 0 && m_outlineView->rows[i].tag <= page)
            best = int(i);
    if (best >= 0 && best != m_outlineView->selected())
        m_outlineView->select(best);
}

void Sidebar::setResults(const std::vector<Document::Match> &matches, const std::string &)
{
    ResultsView &v = *m_results;
    v.rows.clear();
    v.relevance.clear();
    m_resultMatch.clear();
    OutlineRow root;
    root.title = fileName(m_doc->path());
    root.groupRow = true;
    root.group = true;
    root.selectable = false;
    v.rows.push_back(root);
    v.relevance.push_back(-1);
    m_resultMatch.push_back(-1);
    // the contents' entry a page is in: the last that starts on it or before (lowerBoundOfObject:)
    auto entry = [this](int page) {
        int best = -1;
        for (size_t i = 0; i < m_outline.size(); ++i)
            if (m_outline[i].page >= 0 && m_outline[i].page <= page && (best < 0 || m_outline[i].page >= m_outline[size_t(best)].page))
                best = int(i);
        return best;
    };
    // the pages in order, each once, with its first match and its count
    std::map<int, std::pair<int, int>> byPage;
    for (size_t i = 0; i < matches.size(); ++i) {
        auto [it, added] = byPage.try_emplace(matches[i].page, int(i), 0);
        ++it->second.second;
    }
    std::map<int, int> perEntry;
    std::vector<int> pages;
    for (const auto &[page, first] : byPage) {
        pages.push_back(page);
        ++perEntry[entry(page)];
        m_resultMatch.push_back(first.first);
        v.relevance.push_back(first.second);
    }
    const std::string pageFormat = Assets::tr(Bundle, "Localizable", "Page %@", "Page %@");
    const std::string sharedFormat = Assets::tr(Bundle, "Localizable", "%@ (Page %@)", "%@ (Page %@)");
    for (int page : pages) {
        OutlineRow row;
        row.level = 1;
        row.parent = 0;
        row.tag = page;
        const int e = entry(page);
        const std::string label = m_doc->pageLabel(page);
        if (e < 0)
            row.title = withValues(pageFormat, {label});
        else if (perEntry[e] > 1)
            row.title = withValues(sharedFormat, {m_outline[size_t(e)].title, label});
        else
            row.title = m_outline[size_t(e)].title;
        v.rows.push_back(row);
    }
    v.reload();
    // the first result selected (selectRowIndexes: 1, under the group row)
    if (v.rows.size() > 1)
        v.select(1);
    setMode(SearchResults);
}

void Sidebar::clearResults()
{
    if (m_mode == SearchResults)
        setMode(m_before);
}

void Sidebar::sizeThumbnails()
{
    const double w = m_thumbScroll->frame().w;
    if (m_mode == ContactSheet) {
        m_thumbs->thumbnailSize = std::round(m_bar->slider->doubleValue());
    } else {
        // PVWindowController: floor((sidebarWidth - 20 - (one column ? 15 : 0)) / columns - 20)
        m_thumbs->thumbnailSize = std::max(32.0, std::floor((frame().w - 20 - 15) / 1 - 20));
    }
    m_thumbScroll->setDocumentHeight(m_thumbs->heightFor(w > 0 ? w : frame().w));
    m_thumbs->setFrame(m_thumbScroll->document()->bounds());
    m_thumbs->reveal();
    m_thumbs->setNeedsDisplay();
}

void Sidebar::layout()
{
    const Rect list{0, 0, frame().w, std::max(0.0, frame().h - BottomBarHeight)};
    m_thumbScroll->setHidden(!(m_mode == Thumbnails || m_mode == ContactSheet));
    m_outlineScroll->setHidden(m_mode != Contents);
    m_resultsScroll->setHidden(m_mode != SearchResults);
    m_thumbScroll->setFrame(list);
    m_outlineScroll->setFrame(list);
    m_resultsScroll->setFrame(list);
    m_outlineView->reload();
    m_results->reload();
    sizeThumbnails();
    m_bar->setFrame({0, list.h, frame().w, BottomBarHeight});
    setNeedsDisplay();
}

void Sidebar::setFrame(const Rect &r)
{
    View::setFrame(r);
    layout();
}

void Sidebar::draw(cairo_t *cr) { fill(cr, bounds(), m_mode == Contents ? OutlineBackground : ThumbnailBackground); }

} // namespace Preview
