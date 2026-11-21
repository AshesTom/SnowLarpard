// SPDX-License-Identifier: LGPL-2.1-or-later
#include "pdfview.h"

#include <ctime>

#include "coreui.h"
#include "controls.h"
#include "text.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace AK;

namespace Preview {

static AK::Color annotationColor(int type);

namespace {

// PDFKit's continuous display (-[PDFView(PDFViewInternal) generatePageViewBounds], 0x12ddd, with
// displaysPageBreaks): in page points, each page 4 in from the sides (narrower ones centred on the
// widest), the first at the top, 12 between pages, 8 under the last; the whole scaled with the page.
// A picture fills its row.
constexpr double BreakX = 4, BreakGap = 12, BreakBottom = 8;
// -[PDFView(PDFViewInternal) createShadowMaskParts] (0xf9d6): the shadow's lower left corner, 8 x 8
// premultiplied black (_gShadowLowerLeftData at 0xacbc0), its first row against the page's bottom
// edge, its last four columns under the page; the other six parts are its mirror, its last column
// (under the page, stretched), its first row (beside it, stretched) and the upper corners (below).
// In pixels: not scaled with the page.
constexpr unsigned char ShadowCorner[8][8] = {
    {8, 25, 51, 87, 138, 184, 219, 247}, {8, 23, 46, 79, 125, 163, 196, 219}, {5, 20, 41, 69, 107, 138, 163, 184},
    {5, 15, 31, 54, 82, 107, 125, 138},  {3, 10, 20, 36, 54, 69, 79, 87},     {3, 5, 13, 20, 31, 41, 46, 51},
    {3, 3, 5, 10, 15, 20, 23, 25},       {0, 3, 3, 3, 5, 5, 8, 8}};
// the upper left part (_gShadowUpperLeftData at 0xace60): the corner's first four columns turned over,
// but for one value (10 where the corner has 20)
constexpr unsigned char ShadowUpper[8][4] = {{0, 3, 3, 3},   {3, 3, 5, 10},   {3, 5, 13, 20}, {3, 10, 20, 36},
                                             {5, 15, 31, 54}, {5, 10, 41, 69}, {8, 23, 46, 79}, {8, 25, 51, 87}};
// Preview's zoom steps (PVViewZoomFactorNext / Previous: the table at 0x1001320e0)
constexpr double Steps[] = {0.01, 0.02, 0.03, 0.04, 0.06, 0.08, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.75, 1, 1.5, 2,
                            3,    4,    6,    8,    10,   15,   20,  30,   40,  60,  80};
constexpr double MaxScale = 80;   // PVViewZoomFactorMax
// PDFView's matte (-[PDFView(PDFViewInternal) commonInit], 0x8966): device RGB 0.5 0.5 0.5, which
// Preview keeps (-[PVWindowController setupPDFView], 0x100054d7f, only sets the
// PVGeneralWindowBackgroundColor preference's); the paper white (pageColor)
const Color Background = Color::gray(128);
const double SBW = CoreUI::ScrollerWidth;

double rounded(double v) { return std::round(v * 1000) / 1000; }

double widest(const Document &doc)
{
    double w = 0;
    for (int i = 0; i < doc.pageCount(); ++i)
        w = std::max(w, doc.pageSize(i).w);
    return w;
}

// one part of the shadow: w x h pixels, alpha(row, column)
template <typename F> cairo_surface_t *shadowPart(int w, int h, F alpha)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char *data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            reinterpret_cast<uint32_t *>(data + y * stride)[x] = uint32_t(alpha(y, x)) << 24;
    cairo_surface_mark_dirty(s);
    return s;
}

void paintPart(cairo_t *cr, cairo_surface_t *part, double x, double y, double w, double h)
{
    if (w <= 0 || h <= 0)
        return;
    cairo_pattern_t *p = cairo_pattern_create_for_surface(part);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, -x, -y);
    cairo_pattern_set_matrix(p, &m);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

// -[PDFView(PDFViewInternal) drawShadowedRect:blur:] (0x1600e), in a top-down view: the corners 8 x 8
// at the bottom (4 out, 4 under the page), the bottom edge 8 high between them, the sides 4 wide up to
// 8 from the top, and 4 x 8 at the top of each side fading out. Nothing above the page.
void drawShadow(cairo_t *cr, const Rect &r)
{
    static cairo_surface_t *ll = shadowPart(8, 8, [](int y, int x) { return ShadowCorner[y][x]; });
    static cairo_surface_t *lr = shadowPart(8, 8, [](int y, int x) { return ShadowCorner[y][7 - x]; });
    static cairo_surface_t *lc = shadowPart(1, 8, [](int y, int) { return ShadowCorner[y][7]; });
    static cairo_surface_t *cl = shadowPart(4, 1, [](int, int x) { return ShadowCorner[0][x]; });
    static cairo_surface_t *cright = shadowPart(4, 1, [](int, int x) { return ShadowCorner[0][3 - x]; });
    static cairo_surface_t *ul = shadowPart(4, 8, [](int y, int x) { return ShadowUpper[y][x]; });
    static cairo_surface_t *ur = shadowPart(4, 8, [](int y, int x) { return ShadowUpper[y][3 - x]; });
    const double x = r.x, y = r.y, w = r.w, h = r.h;
    paintPart(cr, ll, x - 4, y + h, 8, 8);
    paintPart(cr, lr, x + w - 4, y + h, 8, 8);
    paintPart(cr, lc, x + 4, y + h, w - 8, 8);
    paintPart(cr, cl, x - 4, y + 8, 4, h - 8);
    paintPart(cr, cright, x + w, y + 8, 4, h - 8);
    paintPart(cr, ul, x - 4, y, 4, 8);
    paintPart(cr, ur, x + w, y, 4, 8);
}

void copyText(const std::string &text)
{
    // the session's clipboard: wl-copy on Wayland, xclip on X11
    const char *cmd = std::getenv("WAYLAND_DISPLAY") ? "wl-copy 2>/dev/null" : "xclip -selection clipboard 2>/dev/null";
    if (FILE *p = popen(cmd, "w")) {
        fwrite(text.data(), 1, text.size(), p);
        pclose(p);
    }
}

void openURI(const std::string &uri)
{
    std::string quoted = "'";
    for (char c : uri)
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    std::system(("xdg-open " + quoted + " >/dev/null 2>&1 &").c_str());
}

} // namespace

PDFView::PDFView(const Document *doc) : m_doc(doc) { relayout(); }

double PDFView::rowWidth(const Document &doc, double scale)
{
    // -[PDFView(PDFViewInternal) viewSizeForPage:] (0x137aa): the widest page and its breaks, floored,
    // then scaled and rounded (pageViewSizeForPage:, 0xd931)
    if (!doc.isPDF())
        return std::ceil(widest(doc) * scale);
    return std::round(scale * std::floor(widest(doc) + 2 * BreakX));
}

double PDFView::rowHeight(const Document &doc, int page, double scale)
{
    // -[PDFView(PDFViewInternal) pageViewHeight:] (0xdb2f): the page and its 8 point break, scaled
    if (!doc.isPDF())
        return std::ceil(doc.pageSize(page).h * scale);
    return std::round(scale * (doc.pageSize(page).h + BreakBottom));
}

void PDFView::relayout()
{
    m_tops.clear();
    m_pages.clear();
    const bool pdf = m_doc->isPDF();
    const double maxW = widest(*m_doc);
    double y = 0;   // in page points
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        const Size s = m_doc->pageSize(i);
        m_tops.push_back(y * m_scale);
        if (pdf) {
            m_pages.push_back({(BreakX + (maxW - s.w) / 2) * m_scale, y * m_scale, s.w * m_scale, s.h * m_scale});
            y += s.h + (i + 1 < m_doc->pageCount() ? BreakGap : BreakBottom);
        } else {
            m_pages.push_back({(maxW - s.w) / 2 * m_scale, y * m_scale, s.w * m_scale, s.h * m_scale});
            y += s.h;
        }
    }
    m_tops.push_back(pdf ? std::round(m_scale * std::floor(y)) : std::ceil(y * m_scale));
}

double PDFView::documentHeight() const { return m_tops.empty() ? 0 : m_tops.back(); }

PDFView::Layout PDFView::layout() const
{
    Layout l;
    l.width = rowWidth(*m_doc, m_scale);
    l.height = documentHeight();
    double w = frame().w, h = frame().h;
    // a scroller takes room that may make the other needed
    for (int pass = 0; pass < 2; ++pass) {
        l.vertical = l.height > h + 0.5;
        l.horizontal = l.width > (l.vertical ? frame().w - SBW : frame().w) + 0.5;
        w = frame().w - (l.vertical ? SBW : 0);
        h = frame().h - (l.horizontal ? SBW : 0);
    }
    l.content = {0, 0, std::max(0.0, w), std::max(0.0, h)};
    return l;
}

Rect PDFView::pageRect(int page) const
{
    // -[PDFView(PDFViewInternal) drawPagePre:] (0xf2a7): the page's rect in the document view, its
    // origin rounded up and its far edges down to whole pixels
    const Rect &r = m_pages[size_t(page)];
    if (!m_doc->isPDF())
        return {std::floor(r.x), std::floor(r.y), std::ceil(r.w), std::ceil(r.h)};
    const double x = std::ceil(r.x), y = std::ceil(r.y);
    return {x, y, std::floor(r.x + r.w) - x, std::floor(r.y + r.h) - y};
}

int PDFView::pageAt(Point p, bool nearest) const
{
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        if (p.y < m_tops[size_t(i) + 1] || i == m_doc->pageCount() - 1) {
            if (nearest || pageRect(i).contains(p))
                return i;
            return -1;
        }
    }
    return -1;
}

void PDFView::setFrame(const Rect &r)
{
    View::setFrame(r);
    if (m_autoFit) {
        const bool fit = m_autoFit;
        zoomToFit();
        m_autoFit = fit;
    }
    scrollTo(m_x, m_y);
}

void PDFView::scrollTo(double x, double y)
{
    // -[PDFView(PDFViewInternal) positionInternalViews:] (0x11963): the pages' view in the middle of
    // what is seen when smaller, round((visible - pages) / 2) each way
    const Layout l = layout();
    m_originX = l.width < l.content.w ? std::round((l.content.w - l.width) / 2) : 0;
    m_originY = l.height < l.content.h ? std::round((l.content.h - l.height) / 2) : 0;
    m_x = std::clamp(x, 0.0, std::max(0.0, l.width - l.content.w));
    m_y = std::clamp(y, 0.0, std::max(0.0, l.height - l.content.h));
    updateCurrent();
    setNeedsDisplay();
}

void PDFView::updateCurrent()
{
    // PDFView's currentPage: the one under the middle of what is seen (the first at the top)
    const Layout l = layout();
    int page = m_y <= 0 ? 0 : pageAt({0, m_y + l.content.h / 2 - m_originY}, true);
    if (m_y >= l.height - l.content.h - 0.5 && l.height > l.content.h)
        page = m_doc->pageCount() - 1;
    page = std::clamp(page, 0, std::max(0, m_doc->pageCount() - 1));
    if (page != m_current) {
        m_current = page;
        if (onPageChanged)
            onPageChanged(page);
    }
}

void PDFView::setScale(double s)
{
    s = std::clamp(rounded(s), 0.01, MaxScale);
    m_autoFit = false;
    if (s == m_scale)
        return;
    // the point in the middle stays there
    const Layout before = layout();
    const Point centre = {m_x + before.content.w / 2 - m_originX, m_y + before.content.h / 2 - m_originY};
    const int page = std::clamp(pageAt(centre, true), 0, std::max(0, m_doc->pageCount() - 1));
    const double inPage = (centre.y - m_tops[size_t(page)]) / m_scale;
    const double across = before.width > 0 ? centre.x / before.width : 0.5;
    m_scale = s;
    relayout();
    const Layout after = layout();
    scrollTo(across * after.width - after.content.w / 2, m_tops[size_t(page)] + inPage * s - after.content.h / 2);
    m_cache.clear();
    if (onScaleChanged)
        onScaleChanged(s);
}

void PDFView::zoomIn()
{
    // PVViewZoomFactorNext: the next step above (1 when within half a percent of it)
    double s = rounded(m_scale);
    if (s > 0.995 && s < 1.005)
        s = 1;
    for (double step : Steps)
        if (s < step) {
            setScale(step);
            return;
        }
}

void PDFView::zoomOut()
{
    double s = rounded(m_scale);
    if (s > 0.995 && s < 1.005)
        s = 1;
    for (int i = int(std::size(Steps)) - 1; i >= 0; --i)
        if (s > Steps[i]) {
            setScale(Steps[i]);
            return;
        }
}

void PDFView::zoomToFit()
{
    if (m_doc->pageCount() == 0 || frame().w <= 0)
        return;
    const Size page = m_doc->pageSize(m_current);
    double s;
    if (m_doc->isPDF()) {
        // -[PVPDFView zoomToFit] (0x10006269f) turns PDFKit's autoScales on; in continuous display
        // -[PDFView(PDFViewInternal) autoScaleFactorForPage:] (0xb4a2) fits the width less the
        // scroller's, a single page too (functionalDisplayMode stays continuous): the height isn't fitted
        s = (frame().w - SBW) / std::floor(widest(*m_doc) + 2 * BreakX);
    } else {
        // a picture: all of it, never enlarged
        s = std::min({1.0, frame().w / page.w, frame().h / page.h});
    }
    setScale(std::max(0.01, s));
    m_autoFit = true;
}

void PDFView::goToPage(int page, double top)
{
    if (page < 0 || page >= m_doc->pageCount())
        return;
    scrollTo(m_x, m_tops[size_t(page)] + (top > 0 ? top * m_scale : 0));
}

void PDFView::setTool(Tool t)
{
    m_tool = t;
    m_marquee = {};
    setNeedsDisplay();
}

void PDFView::setMatches(std::vector<Document::Match> matches)
{
    m_matches = std::move(matches);
    m_match = -1;
    setNeedsDisplay();
}

void PDFView::showMatch(int index)
{
    if (index < 0 || index >= int(m_matches.size()))
        return;
    m_match = index;
    const Document::Match &m = m_matches[size_t(index)];
    // the match in the middle of the view when it isn't seen
    const Layout l = layout();
    const Rect page = pageRect(m.page);
    const double y = page.y + m.rect.y * m_scale, x = page.x + m.rect.x * m_scale;
    double sx = m_x, sy = m_y;
    if (y < m_y || y + m.rect.h * m_scale > m_y + l.content.h)
        sy = y - l.content.h / 2;
    if (x < m_x || x + m.rect.w * m_scale > m_x + l.content.w)
        sx = x - l.content.w / 2;
    scrollTo(sx, sy);
    // and selected, as PDFKit selects what it finds
    m_selPage = m.page;
    m_selection = m.rect;
    setNeedsDisplay();
}

std::string PDFView::selectedText() const
{
    if (m_selPage < 0 || m_selection.empty())
        return {};
    return m_doc->selectedText(m_selPage, m_selection);
}

void PDFView::copy() const
{
    const std::string t = selectedText();
    if (!t.empty())
        copyText(t);
}

const Image &PDFView::rendered(int page)
{
    if (m_cacheScale != m_scale) {
        m_cache.clear();
        m_cacheScale = m_scale;
    }
    auto it = m_cache.find(page);
    if (it == m_cache.end()) {
        // the pages far from those seen go
        for (auto c = m_cache.begin(); c != m_cache.end();)
            c = std::abs(c->first - page) > 6 ? m_cache.erase(c) : std::next(c);
        it = m_cache.emplace(page, m_doc->render(page, m_scale)).first;
    }
    return it->second;
}

void PDFView::draw(cairo_t *cr)
{
    const Layout l = layout();
    fill(cr, l.content, m_doc->isPDF() ? Background : Color::gray(255));
    cairo_save(cr);
    cairo_rectangle(cr, l.content.x, l.content.y, l.content.w, l.content.h);
    cairo_clip(cr);
    cairo_translate(cr, m_originX - m_x, m_originY - m_y);
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        if (m_tops[size_t(i) + 1] + 8 < m_y - m_originY || m_tops[size_t(i)] > m_y - m_originY + l.content.h)
            continue;
        const Rect r = pageRect(i);
        if (m_doc->isPDF()) {
            // drawPagePre: the shadow (displaysPageBreaks), then the paper (pageColor)
            drawShadow(cr, r);
            fill(cr, r, Color::gray(255));
        }
        const double pixels = r.w * r.h;
        if (pixels <= 6000.0 * 6000.0) {
            rendered(i).draw(cr, Point{r.x, r.y});
        } else {
            // too large to keep: only what is seen, drawn each time
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_clip(cr);
            cairo_translate(cr, r.x, r.y);
            m_doc->draw(cr, i, m_scale);
            cairo_restore(cr);
        }
        cairo_save(cr);
        cairo_translate(cr, r.x, r.y);
        // the search's matches: yellow under the text, the one shown selected
        for (size_t m = 0; m < m_matches.size(); ++m) {
            const Document::Match &match = m_matches[m];
            if (match.page != i || int(m) == m_match)
                continue;
            cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
            cairo_set_source_rgb(cr, 1, 0.95, 0.35);
            cairo_rectangle(cr, match.rect.x * m_scale, match.rect.y * m_scale, match.rect.w * m_scale, match.rect.h * m_scale);
            cairo_fill(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        }
        if (m_selPage == i && !m_selection.empty())
            m_doc->drawSelection(cr, i, m_scale, m_selection);
        // the annotation being drawn, in its colour and width
        if (m_drag == Creating && m_createPage == i) {
            const Color c = annotationColor(m_adding);
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_set_line_width(cr, 3 * m_scale);
            const Point a{m_createFrom.x * m_scale, m_createFrom.y * m_scale}, b{m_createTo.x * m_scale, m_createTo.y * m_scale};
            if (m_adding == 1) {
                cairo_move_to(cr, a.x, a.y);
                cairo_line_to(cr, b.x, b.y);
            } else if (m_adding == 2) {
                const double w = std::abs(b.x - a.x), h = std::abs(b.y - a.y);
                if (w > 1 && h > 1) {
                    cairo_save(cr);
                    cairo_translate(cr, std::min(a.x, b.x) + w / 2, std::min(a.y, b.y) + h / 2);
                    cairo_scale(cr, w / 2, h / 2);
                    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
                    cairo_restore(cr);
                }
            } else {
                cairo_rectangle(cr, std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y));
            }
            cairo_stroke(cr);
        }
        // the selected annotation: framed, its corners' handles (-drawSelectionBorderInView:)
        if (m_selectedAnnot >= 0 && size_t(m_selectedAnnot) < m_doc->annotations().size() &&
            m_doc->annotations()[size_t(m_selectedAnnot)].page == i) {
            const Rect a = m_doc->annotations()[size_t(m_selectedAnnot)].rect;
            const Rect r2{std::floor(a.x * m_scale) + 0.5, std::floor(a.y * m_scale) + 0.5, std::floor(a.w * m_scale), std::floor(a.h * m_scale)};
            cairo_set_line_width(cr, 1);
            cairo_set_source_rgba(cr, 0.35, 0.35, 0.35, 0.8);
            cairo_rectangle(cr, r2.x, r2.y, r2.w, r2.h);
            cairo_stroke(cr);
            for (const Point &h : {Point{r2.x, r2.y}, Point{r2.right(), r2.y}, Point{r2.x, r2.bottom()}, Point{r2.right(), r2.bottom()}}) {
                cairo_rectangle(cr, h.x - 3, h.y - 3, 6, 6);
                cairo_set_source_rgb(cr, 1, 1, 1);
                cairo_fill_preserve(cr);
                cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
                cairo_stroke(cr);
            }
        }
        cairo_restore(cr);
    }
    if (!m_marquee.empty()) {
        // the Select tool's rectangle: dashed, as the marching ants stand still
        cairo_set_line_width(cr, 1);
        const double dash[] = {4, 4};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_rectangle(cr, std::floor(m_marquee.x) + 0.5, std::floor(m_marquee.y) + 0.5, std::floor(m_marquee.w), std::floor(m_marquee.h));
        cairo_stroke(cr);
        cairo_set_dash(cr, dash, 2, 4);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, std::floor(m_marquee.x) + 0.5, std::floor(m_marquee.y) + 0.5, std::floor(m_marquee.w), std::floor(m_marquee.h));
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    drawScrollers(cr, l);
}

void PDFView::drawScrollers(cairo_t *cr, const Layout &l)
{
    const CoreUI::State s = controlState(*this);
    if (l.vertical) {
        const double range = l.height - l.content.h;
        CoreUI::scroller(cr, Rect{l.content.w, 0, SBW, l.content.h}, range > 0 ? m_y / range : 0, l.content.h / l.height, s,
                         m_drag == Arrow && m_vertArrow ? (m_arrowDir < 0 ? 1 : 2) : 0);
    }
    if (l.horizontal) {
        const double range = l.width - l.content.w;
        CoreUI::scroller(cr, Rect{0, l.content.h, l.content.w, SBW}, range > 0 ? m_x / range : 0, l.content.w / l.width, s,
                         m_drag == Arrow && !m_vertArrow ? (m_arrowDir < 0 ? 1 : 2) : 0);
    }
    if (l.vertical && l.horizontal)
        fill(cr, Rect{l.content.w, l.content.h, SBW, SBW}, Color::gray(237));
}

// PVAnnotation's defaults (persistentColorForType:, persistentLineWidthForType:): the shapes, the text
// and strike through PVAnnotationBanner's red (tag 101: 0.969, 0.290, 0.243), the note its yellow
// (103: 0.941, 0.863, 0.169), the highlight the highlighting yellow (0.984, 0.929, 0.451), the
// underline black; lines 3 wide
static Color annotationColor(int type)
{
    switch (type) {
    case 4: return {0.941, 0.863, 0.169, 1};
    case 7: return {0.984, 0.929, 0.451, 1};
    case 9: return {0, 0, 0, 1};
    default: return {0.969, 0.290, 0.243, 1};
    }
}

void PDFView::setAddingType(int type)
{
    m_adding = type;
    m_selectedAnnot = -1;
    setNeedsDisplay();
}

void PDFView::annotationsChanged(int page)
{
    m_cache.erase(page);
    setNeedsDisplay();
    if (onAnnotationsChanged)
        onAnnotationsChanged();
}

int PDFView::addAnnotation(int type, int page, Rect rect, Point from, Point to)
{
    Document::Annotation a;
    a.type = type;
    a.page = page;
    a.rect = rect;
    a.start = from;
    a.end = to;
    a.color = annotationColor(type);
    a.lineWidth = 3;
    if (type == 4) {
        // -defaultTextContents: the date, NSDateFormatterShortStyle with the time
        char buf[64];
        const time_t now = time(nullptr);
        std::strftime(buf, sizeof buf, "%m/%d/%y %l:%M %p", std::localtime(&now));
        a.contents = buf;
    }
    auto *doc = const_cast<Document *>(m_doc);
    const int index = doc->addAnnotation(a);
    if (index >= 0) {
        m_selectedAnnot = index;
        annotationsChanged(page);
        if (onAnnotationAdded)
            onAnnotationAdded(index);
    }
    return index;
}

bool PDFView::addOverSelection(int type)
{
    if (m_selPage < 0 || m_selection.empty())
        return false;
    const std::vector<Rect> lines = m_doc->selectionRects(m_selPage, m_selection);
    if (lines.empty())
        return false;
    Rect b = lines[0];
    for (const Rect &l : lines) {
        const double r = std::max(b.right(), l.right()), bot = std::max(b.bottom(), l.bottom());
        b.x = std::min(b.x, l.x), b.y = std::min(b.y, l.y);
        b.w = r - b.x, b.h = bot - b.y;
    }
    const int page = m_selPage;
    if (type >= 7) {
        addAnnotation(type, page, m_selection);
    } else if (type == 4) {
        // a note of 24 × 24 at the selection's middle
        addAnnotation(type, page, {std::round(b.x + b.w / 2) - 12, std::round(b.y + b.h / 2) - 12, 24, 24});
    } else {
        // the shapes around it, out by the line's width; the oval out to hold the rectangle
        double grow = 3;
        Rect r{b.x - grow, b.y - grow, b.w + 2 * grow, b.h + 2 * grow};
        if (type == 2) {
            const double w2 = r.h / 2, d = w2 + std::hypot(w2, r.w), e = std::sqrt(d * d - r.w * r.w);
            const double dx = ((w2 < r.w ? d : e) - r.w) / 2, dy = ((w2 < r.w ? e : d) - r.h) / 2;
            r = {r.x - dx, r.y - dy, r.w + 2 * dx, r.h + 2 * dy};
        }
        if (type == 1)
            addAnnotation(type, page, r, {r.x, r.y + r.h / 2}, {r.right(), r.y + r.h / 2});
        else
            addAnnotation(type, page, r);
    }
    m_selection = {};
    setNeedsDisplay();
    return true;
}

void PDFView::addFreeText(int page, Point at, const std::string &text)
{
    if (text.empty())
        return;
    const double w = std::ceil(textWidth(text, Font{36, false, "Helvetica"})) + 12, h = 46;
    Document::Annotation a;
    a.type = 5;
    a.page = page;
    a.rect = {std::round(at.x - w / 2), std::round(at.y - h / 2), w, h};
    a.color = annotationColor(5);
    a.contents = text;
    const int index = const_cast<Document *>(m_doc)->addAnnotation(a);
    if (index >= 0) {
        m_selectedAnnot = index;
        annotationsChanged(page);
        if (onAnnotationAdded)
            onAnnotationAdded(index);
    }
}

Point PDFView::pageToView(int page, Point p) const
{
    const Rect pr = pageRect(page);
    return {pr.x + p.x * m_scale - m_x + m_originX, pr.y + p.y * m_scale - m_y + m_originY};
}

void PDFView::deleteSelectedAnnotation()
{
    if (m_selectedAnnot < 0 || size_t(m_selectedAnnot) >= m_doc->annotations().size())
        return;
    const int page = m_doc->annotations()[size_t(m_selectedAnnot)].page;
    const_cast<Document *>(m_doc)->removeAnnotation(m_selectedAnnot);
    m_selectedAnnot = -1;
    annotationsChanged(page);
}

void PDFView::mouseDown(Event &e)
{
    const Layout l = layout();
    m_down = m_last = e.location;
    m_moved = false;
    m_drag = None;
    // the scrollers
    auto scroller = [&](bool vertical) {
        const Rect r = vertical ? Rect{l.content.w, 0, SBW, l.content.h} : Rect{0, l.content.h, l.content.w, SBW};
        if (!r.contains(e.location))
            return false;
        const double range = vertical ? l.height - l.content.h : l.width - l.content.w;
        const double pos = vertical ? m_y : m_x, seen = vertical ? l.content.h : l.content.w;
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(r, range > 0 ? pos / range : 0, seen / (vertical ? l.height : l.width));
        if (p.knob.contains(e.location)) {
            m_drag = vertical ? VKnob : HKnob;
            m_dragStart = pos;
        } else if (p.up.contains(e.location) || p.down.contains(e.location)) {
            m_drag = Arrow;
            m_vertArrow = vertical;
            m_arrowDir = p.up.contains(e.location) ? -1 : 1;
            auto step = [this] {
                if (m_vertArrow)
                    scrollTo(m_x, m_y + m_arrowDir * 19);
                else
                    scrollTo(m_x + m_arrowDir * 19, m_y);
            };
            step();
            m_repeat = App::shared().addTimer(0.35, false, [this, step] { m_repeat = App::shared().addTimer(0.05, true, step); });
        } else {
            // a page towards the click
            const double towards = (vertical ? e.location.y < p.knob.y : e.location.x < p.knob.x) ? -1 : 1;
            if (vertical)
                scrollTo(m_x, m_y + towards * (seen - 19));
            else
                scrollTo(m_x + towards * (seen - 19), m_y);
        }
        setNeedsDisplay();
        return true;
    };
    if ((l.vertical && scroller(true)) || (l.horizontal && scroller(false)))
        return;
    if (!l.content.contains(e.location))
        return;
    const Point d = toDocument(e.location);
    if (m_adding && m_doc->isPDF() && m_adding < 7) {
        const int page = pageAt(d, false);
        if (page < 0)
            return;
        const Rect pr = pageRect(page);
        const Point p{(d.x - pr.x) / m_scale, (d.y - pr.y) / m_scale};
        if (m_adding == 4) {
            addAnnotation(4, page, {std::round(p.x) - 12, std::round(p.y) - 12, 24, 24});
            return;
        }
        if (m_adding == 5) {
            if (onFreeText)
                onFreeText(page, p);
            return;
        }
        m_drag = Creating;
        m_createPage = page;
        m_createFrom = m_createTo = p;
        setNeedsDisplay();
        return;
    }
    if (m_tool == Move && !m_adding) {
        m_drag = Pan;
    } else if ((m_tool == Text || m_adding >= 7) && m_doc->isPDF()) {
        m_drag = Selecting;
        m_selPage = pageAt(d, true);
        const Rect pr = pageRect(m_selPage);
        m_selection = {(d.x - pr.x) / m_scale, (d.y - pr.y) / m_scale, 0, 0};
        m_match = -1;
    } else {
        m_drag = Marquee;
        m_marquee = {d.x, d.y, 0, 0};
    }
    setNeedsDisplay();
}

void PDFView::mouseDragged(Event &e)
{
    const Layout l = layout();
    if (std::abs(e.location.x - m_down.x) > 2 || std::abs(e.location.y - m_down.y) > 2)
        m_moved = true;
    switch (m_drag) {
    case Pan:
        scrollTo(m_x - (e.location.x - m_last.x), m_y - (e.location.y - m_last.y));
        break;
    case VKnob:
    case HKnob: {
        const bool v = m_drag == VKnob;
        const Rect r = v ? Rect{l.content.w, 0, SBW, l.content.h} : Rect{0, l.content.h, l.content.w, SBW};
        const double total = v ? l.height : l.width, seen = v ? l.content.h : l.content.w;
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(r, 0, seen / total);
        const double travel = v ? p.track.h - p.knob.h : p.track.w - p.knob.w;
        if (travel > 0) {
            const double delta = (v ? e.location.y - m_down.y : e.location.x - m_down.x) * (total - seen) / travel;
            if (v)
                scrollTo(m_x, m_dragStart + delta);
            else
                scrollTo(m_dragStart + delta, m_y);
        }
        break;
    }
    case Selecting: {
        const Point d = toDocument(e.location);
        const Rect pr = pageRect(m_selPage);
        const double x = (d.x - pr.x) / m_scale, y = (d.y - pr.y) / m_scale;
        const Point start{m_selection.x, m_selection.y};
        // the anchor stays where the mouse went down: the rectangle runs from it to the pointer
        m_selection = {start.x, start.y, x - start.x, y - start.y};
        // poppler takes the two points in any order: kept as a start and an extent
        setNeedsDisplay();
        break;
    }
    case Creating: {
        const Point d = toDocument(e.location);
        const Rect pr = pageRect(m_createPage);
        m_createTo = {(d.x - pr.x) / m_scale, (d.y - pr.y) / m_scale};
        setNeedsDisplay();
        break;
    }
    case Marquee: {
        const Point d = toDocument(e.location);
        const Point a = toDocument(m_down);
        m_marquee = {std::min(a.x, d.x), std::min(a.y, d.y), std::abs(d.x - a.x), std::abs(d.y - a.y)};
        setNeedsDisplay();
        break;
    }
    default:
        break;
    }
    m_last = e.location;
}

void PDFView::mouseUp(Event &e)
{
    if (m_repeat)
        App::shared().removeTimer(m_repeat);
    m_repeat = 0;
    const Drag drag = m_drag;
    m_drag = None;
    setNeedsDisplay();
    if (drag == Creating) {
        // -trackAnnotationResize:…isNewAnnotation:: the rectangle dragged; nothing for a click
        // (an empty annotation is taken back)
        const Point a = m_createFrom, b = m_createTo;
        if (m_moved) {
            if (m_adding == 1) {
                const Rect r{std::min(a.x, b.x) - 3, std::min(a.y, b.y) - 3, std::abs(b.x - a.x) + 6, std::abs(b.y - a.y) + 6};
                addAnnotation(1, m_createPage, r, a, b);
            } else if (m_adding == 2 || m_adding == 3) {
                addAnnotation(m_adding, m_createPage, {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)});
            }
        }
        m_createPage = -1;
        return;
    }
    if (drag == Selecting && m_adding >= 7 && m_moved) {
        // -toggleMarkupOfType:overSelection: on the mouse's going up
        addOverSelection(m_adding);
        return;
    }
    if (drag == Selecting && !m_moved)
        m_selection = {};
    if (m_moved || !(drag == Pan || drag == Selecting || drag == Marquee))
        return;
    // a click: an annotation under it selected, else a link under it followed
    const Point d = toDocument(e.location);
    const int page = pageAt(d, false);
    if (page < 0)
        return;
    const Rect pr = pageRect(page);
    const Point inPage{(d.x - pr.x) / m_scale, (d.y - pr.y) / m_scale};
    m_selectedAnnot = -1;
    const auto &annots = m_doc->annotations();
    for (size_t i = annots.size(); i-- > 0;) {
        if (annots[i].page == page && annots[i].rect.contains(inPage)) {
            m_selectedAnnot = int(i);
            setNeedsDisplay();
            return;
        }
    }
    for (const Document::Link &link : m_doc->links(page)) {
        if (!link.rect.contains(inPage))
            continue;
        if (!link.uri.empty())
            openURI(link.uri);
        else
            goToPage(link.page, link.top);
        return;
    }
    if (drag == Marquee)
        m_marquee = {};
}

void PDFView::scrollWheel(Event &e)
{
    if (e.modifiers & AK::Shift)
        scrollTo(m_x + e.deltaY * 40, m_y);
    else
        scrollTo(m_x, m_y + e.deltaY * 40);
}

void PDFView::keyDown(Event &e)
{
    const Layout l = layout();
    const bool command = e.modifiers & (AK::Command | AK::Control);
    if (command) {
        switch (e.keysym) {
        case XKB_KEY_plus:
        case XKB_KEY_equal:
        case XKB_KEY_KP_Add: zoomIn(); return;
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract: zoomOut(); return;
        case XKB_KEY_0: actualSize(); return;
        case XKB_KEY_c:
        case XKB_KEY_C: copy(); return;
        default: break;
        }
    }
    const bool fitsAcross = l.width <= l.content.w + 0.5;
    switch (e.keysym) {
    case XKB_KEY_Up: scrollTo(m_x, m_y - 19); break;
    case XKB_KEY_Down: scrollTo(m_x, m_y + 19); break;
    case XKB_KEY_Left:
        if (fitsAcross)
            previousPage();
        else
            scrollTo(m_x - 19, m_y);
        break;
    case XKB_KEY_Right:
        if (fitsAcross)
            nextPage();
        else
            scrollTo(m_x + 19, m_y);
        break;
    case XKB_KEY_Page_Up: scrollTo(m_x, m_y - (l.content.h - 19)); break;
    case XKB_KEY_space:
        scrollTo(m_x, m_y + ((e.modifiers & AK::Shift) ? -1 : 1) * (l.content.h - 19));
        break;
    case XKB_KEY_Page_Down: scrollTo(m_x, m_y + (l.content.h - 19)); break;
    case XKB_KEY_Home: scrollTo(m_x, 0); break;
    case XKB_KEY_End: scrollTo(m_x, l.height); break;
    case XKB_KEY_BackSpace:
    case XKB_KEY_Delete:
        deleteSelectedAnnotation();
        break;
    default: View::keyDown(e); break;
    }
}

} // namespace Preview
