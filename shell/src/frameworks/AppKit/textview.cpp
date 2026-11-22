// SPDX-License-Identifier: LGPL-2.1-or-later
#include "textview.h"

#include "controls.h"
#include "coreui.h"
#include "panels.h"
#include "rulerview.h"

#include "foundation.h"
#include "text.h"

#include <glib.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>



namespace AK {

namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

size_t prevChar(const std::string &s, size_t p)
{
    if (p == 0)
        return 0;
    --p;
    while (p > 0 && continuation(s[p]))
        --p;
    return p;
}

size_t nextChar(const std::string &s, size_t p)
{
    if (p >= s.size())
        return s.size();
    ++p;
    while (p < s.size() && continuation(s[p]))
        ++p;
    return p;
}

bool wordChar(const std::string &s, size_t p)
{
    if (p >= s.size())
        return false;
    const unsigned char c = static_cast<unsigned char>(s[p]);
    if (c >= 0x80)
        return s.compare(p, 3, AttachmentCharacter) != 0 && g_unichar_isalnum(g_utf8_get_char(s.c_str() + p));
    return std::isalnum(c) || c == '\'' || c == '_';
}

// The clipboard between notes: what Copy put there, with its styles, while the system's still holds its text
RichText s_clip;
std::string s_clipPlain;

void copyToSystem(const std::string &text)
{
    const char *cmd = std::getenv("WAYLAND_DISPLAY") ? "wl-copy 2>/dev/null" : "xclip -selection clipboard 2>/dev/null";
    if (FILE *p = popen(cmd, "w")) {
        fwrite(text.data(), 1, text.size(), p);
        pclose(p);
    }
}

std::string pasteFromSystem()
{
    const char *cmd = std::getenv("WAYLAND_DISPLAY") ? "wl-paste -n 2>/dev/null" : "xclip -selection clipboard -o 2>/dev/null";
    std::string out;
    if (FILE *p = popen(cmd, "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0 && out.size() < (16u << 20))
            out.append(buf, n);
        pclose(p);
    }
    return out;
}

PangoFontDescription *describe(const Style &s)
{
    PangoFontDescription *d = pango_font_description_new();
    pango_font_description_set_family(d, (s.family + ", Helvetica, Lucida Grande, sans-serif").c_str());
    PangoWeight weight = s.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL;
    if (s.face == "Light" || s.face == "Thin" || s.face == "UltraLight")
        weight = s.bold ? PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_LIGHT;
    else if (s.face == "Wide" || s.face == "Black" || s.face == "Heavy")
        weight = PANGO_WEIGHT_HEAVY;
    else if (s.face == "Medium")
        weight = PANGO_WEIGHT_MEDIUM;
    pango_font_description_set_weight(d, weight);
    pango_font_description_set_style(d, s.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(d, s.size * PANGO_SCALE);
    return d;
}

double rise(const Style &s) { return s.baseline + (s.script > 0 ? s.size * 0.33 : s.script < 0 ? -s.size * 0.2 : 0); }

// An attachment's picture: its file's bytes, decoded through a file of the cache
Image attachmentImage(Attachment &a)
{
    if (!a.image.isNull() || a.data.empty())
        return a.image;
    const std::string dir = cacheDir() + "/stickies";
    makePath(dir);
    std::string ext = a.name.substr(a.name.find_last_of('.') == std::string::npos ? a.name.size() : a.name.find_last_of('.'));
    const std::string file = dir + "/" + hashName(a.data) + ext;
    if (!isFile(file)) {
        std::ofstream f(file, std::ios::binary);
        f.write(a.data.data(), std::streamsize(a.data.size()));
    }
    a.image = Image::load(file);
    // NSTextAttachmentCell's for a file that is no picture: its icon (NSWorkspace's, 32 × 32), the
    // generic document's
    if (a.image.isNull()) {
        a.image = Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericDocumentIcon.icns"),
                              {32, 32});
        if (!a.image.isNull() && (a.image.width() != 32 || a.image.height() != 32))
            a.image = a.image.scaled(32, 32);
        a.width = a.height = 32;
    }
    if (!a.image.isNull() && (a.width <= 0 || a.height <= 0)) {
        a.width = a.image.width();
        a.height = a.image.height();
    }
    return a.image;
}

} // namespace

TextView::TextView()
{
    m_text.styles.push_back(Style());
}

TextView::~TextView()
{
    if (m_blink)
        App::shared().removeTimer(m_blink);
    for (Paragraph &p : m_paragraphs)
        if (p.layout)
            g_object_unref(p.layout);
}

void TextView::setText(RichText t)
{
    m_text = std::move(t);
    if (m_text.styles.empty())
        m_text.styles.push_back(m_typing);
    for (Attachment &a : m_text.attachments)
        attachmentImage(a);
    m_caret = m_anchor = 0;
    m_scroll = 0;
    m_undo.clear();
    m_redo.clear();
    m_typing = styleAtSelection();
    invalidateLayout();
}

void TextView::setFrame(const Rect &r)
{
    const bool wider = r.w != frame().w;
    View::setFrame(r);
    if (wider && !m_pages)
        invalidateLayout();
    m_scroll = std::clamp(m_scroll, 0.0, maxScrollOffset());
    m_scrollX = std::clamp(m_scrollX, 0.0, maxScrollOffsetX());
}

// ---- pages (TextEdit's MultiplePageView, 0x10000b08b...)

void TextView::setPages(std::optional<PageGeometry> pages)
{
    m_pages = pages;
    m_scrollX = 0;
    invalidateLayout();
    m_scroll = std::clamp(m_scroll, 0.0, maxScrollOffset());
}

double TextView::textWidth() const
{
    // documentSizeInPage (0x10000b7b3): the paper less the margins, the padding back on each side
    if (m_pages)
        return std::max(1.0, m_pages->paperWidth - m_pages->left - m_pages->right);
    return std::max(1.0, frame().w - 2 * Padding);
}

// documentRectForPageNumber: (0x10000b6f5): the page's origin, the left margin less the padding
double TextView::textOriginX() const { return m_pages ? m_pages->left - Padding : 0; }

double TextView::contentWidth() const { return m_pages ? m_pages->paperWidth : frame().w; }

size_t TextView::numberOfPages() const
{
    layout();
    return m_pages ? m_pageCount : 1;
}

Rect TextView::pageRect(size_t page) const
{
    // pageRectForPageNumber: (0x10000b614): one under the other, the separator between
    if (!m_pages)
        return bounds();
    return {0, double(page) * (m_pages->paperHeight + PageSeparatorHeight), m_pages->paperWidth, m_pages->paperHeight};
}

void TextView::invalidateLayout()
{
    m_layoutValid = false;
    setNeedsDisplay();
}

// ---- layout: a pango layout for each paragraph

void TextView::layout() const
{
    const double width = textWidth();
    if (m_layoutValid && width == m_layoutWidth)
        return;
    for (Paragraph &p : m_paragraphs)
        if (p.layout)
            g_object_unref(p.layout);
    m_paragraphs.clear();
    m_layoutWidth = width;
    m_layoutValid = true;
    const std::string &s = m_text.text;
    double y = 0;
    size_t start = 0;
    // with pages: how far down the lines have moved, from the first page's top margin
    double shift = m_pages ? m_pages->top : 0, lastTop = 0;
    for (;;) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos)
            end = s.size();
        Paragraph p;
        p.start = start;
        p.end = end;
        p.layout = pango_layout_new(pangoContext());
        const std::string part = s.substr(start, end - start);
        pango_layout_set_text(p.layout, part.c_str(), int(part.size()));
        pango_layout_set_wrap(p.layout, PANGO_WRAP_WORD_CHAR);
        // its style: the first character's (an empty paragraph's: the one typing would use)
        const Style &first = start < s.size() ? m_text.styleAt(start) : (start > 0 ? m_text.styleAt(start - 1) : m_typing);
        const Style &para = part.empty() && start == s.size() ? m_typing : first;
        const ParagraphStyle &ps = para.para;
        // NSTypesetter's line fragments: from the head indent (the first line's from its own),
        // to the tail indent (> 0 from the start, else from the end); here the layout from the
        // nearer of the two starts, the first line indented from it (pango's indent; a negative one
        // hangs the others)
        p.x = std::max(0.0, std::min(ps.firstLineHeadIndent, ps.headIndent));
        const double right = ps.tailIndent > 0 ? ps.tailIndent : width + ps.tailIndent;
        pango_layout_set_width(p.layout, int(std::max(1.0, right - p.x) * PANGO_SCALE));
        pango_layout_set_indent(p.layout, int((ps.firstLineHeadIndent - ps.headIndent) * PANGO_SCALE));
        // the tab stops (the paragraph's, else the text's), then every default interval after the
        // last as far as the line goes; from the layout's start
        std::vector<TabStop> stops = ps.tabStops;
        if (!ps.hasTabs) {
            stops.clear();
            for (double t : tabStops)
                stops.push_back({t, TabStop::Left});
        }
        const double interval = ps.defaultTabInterval > 0 ? ps.defaultTabInterval : defaultTabInterval;
        if (interval > 0) {
            double x = stops.empty() ? 0 : stops.back().location;
            while (x < right + interval && stops.size() < 512) {
                x = (std::floor(x / interval + 1e-9) + 1) * interval;
                stops.push_back({x, TabStop::Left});
            }
        }
        std::vector<TabStop> kept;
        for (const TabStop &t : stops)
            if (t.location > p.x)
                kept.push_back(t);
        PangoTabArray *tabs = pango_tab_array_new(int(kept.size()), FALSE);
        for (size_t i = 0; i < kept.size(); ++i) {
            const PangoTabAlign a = kept[i].kind == TabStop::Center ? PANGO_TAB_CENTER : kept[i].kind == TabStop::Right ? PANGO_TAB_RIGHT
                                    : kept[i].kind == TabStop::Decimal                                        ? PANGO_TAB_DECIMAL
                                                                                                               : PANGO_TAB_LEFT;
            pango_tab_array_set_tab(tabs, int(i), a, int((kept[i].location - p.x) * PANGO_SCALE));
        }
        pango_layout_set_tabs(p.layout, tabs);
        pango_tab_array_free(tabs);
        // the lines' height: lineHeightMultiple times their own, lineSpacing after each
        if (ps.lineHeightMultiple > 0 && ps.minimumLineHeight <= 0 && ps.maximumLineHeight <= 0)
            pango_layout_set_line_spacing(p.layout, float(ps.lineHeightMultiple));
        if (ps.lineSpacing > 0)
            pango_layout_set_spacing(p.layout, int(ps.lineSpacing * PANGO_SCALE));
        PangoFontDescription *pd = describe(para);
        pango_layout_set_font_description(p.layout, pd);
        pango_font_description_free(pd);
        switch (para.align) {
        case Alignment::Center: pango_layout_set_alignment(p.layout, PANGO_ALIGN_CENTER); break;
        case Alignment::Right: pango_layout_set_alignment(p.layout, PANGO_ALIGN_RIGHT); break;
        case Alignment::Justified: pango_layout_set_justify(p.layout, TRUE); break;
        default: break;
        }
        PangoAttrList *attrs = pango_attr_list_new();
        size_t attachment = m_text.attachmentIndex(start);
        for (size_t i = start; i < end;) {
            const uint16_t run = m_text.runs[i];
            size_t j = i;
            while (j < end && m_text.runs[j] == run && s.compare(j, 3, AttachmentCharacter) != 0)
                ++j;
            if (j == i) {
                // a picture: its size as the run's
                const Attachment *a = attachment < m_text.attachments.size() ? &m_text.attachments[attachment] : nullptr;
                ++attachment;
                const double w = a && a->width > 0 ? a->width : 16, h = a && a->height > 0 ? a->height : 16;
                PangoRectangle r = {0, int(-h * PANGO_SCALE), int(w * PANGO_SCALE), int(h * PANGO_SCALE)};
                PangoAttribute *shape = pango_attr_shape_new(&r, &r);
                shape->start_index = guint(i - start);
                shape->end_index = guint(i - start + 3);
                pango_attr_list_insert(attrs, shape);
                j = i + 3;
            }
            const Style &st = m_text.styles[run];
            PangoFontDescription *d = describe(st);
            PangoAttribute *font = pango_attr_font_desc_new(d);
            pango_font_description_free(d);
            font->start_index = guint(i - start);
            font->end_index = guint(j - start);
            pango_attr_list_insert(attrs, font);
            if (const double r = rise(st); r != 0) {
                PangoAttribute *ra = pango_attr_rise_new(int(r * PANGO_SCALE));
                ra->start_index = guint(i - start);
                ra->end_index = guint(j - start);
                pango_attr_list_insert(attrs, ra);
            }
            i = j;
        }
        pango_layout_set_attributes(p.layout, attrs);
        pango_attr_list_unref(attrs);
        // the minimum and maximum line heights (NSParagraphStyle's): the multiple of the first line's
        // own height kept within them, as the factor of every line (pango has no bounds of its own)
        double factor = ps.lineHeightMultiple;
        if (ps.minimumLineHeight > 0 || ps.maximumLineHeight > 0) {
            PangoRectangle natural;
            pango_layout_line_get_extents(pango_layout_get_line_readonly(p.layout, 0), nullptr, &natural);
            const double h = natural.height / double(PANGO_SCALE);
            if (h > 0) {
                double target = h * (ps.lineHeightMultiple > 0 ? ps.lineHeightMultiple : 1);
                if (ps.maximumLineHeight > 0)
                    target = std::min(target, ps.maximumLineHeight);
                target = std::max(target, ps.minimumLineHeight);
                factor = target / h;
                pango_layout_set_line_spacing(p.layout, float(factor));
            }
        }
        PangoRectangle logical;
        pango_layout_get_extents(p.layout, nullptr, &logical);
        // paragraphSpacingBefore above it, paragraphSpacing below; the first line's multiple too
        // (pango's line spacing leaves it: the extra above it, the glyphs at the bottom as Cocoa's)
        y += ps.paragraphSpacingBefore;
        if (factor > 1) {
            PangoRectangle first;
            pango_layout_line_get_extents(pango_layout_get_line_readonly(p.layout, 0), nullptr, &first);
            y += (factor - 1) * first.height / double(PANGO_SCALE);
        }
        p.y = y;
        p.height = logical.height / double(PANGO_SCALE) + ps.lineSpacing;
        y += p.height + ps.paragraphSpacing;
        if (m_pages) {
            // NSLayoutManager going on in the next page's container: a line that doesn't fit what
            // is left of its page starts the next one (alone on its page: it stays, cut)
            const PageGeometry &g = *m_pages;
            const double stride = g.paperHeight + PageSeparatorHeight, usable = g.paperHeight - g.top - g.bottom;
            PangoLayoutIter *it = pango_layout_get_iter(p.layout);
            do {
                int y0 = 0, y1 = 0;
                pango_layout_iter_get_line_yrange(it, &y0, &y1);
                double at = p.y + y0 / double(PANGO_SCALE) + shift;
                const double h = (y1 - y0) / double(PANGO_SCALE);
                const double page = std::floor(at / stride), pageTop = page * stride + g.top;
                if (at < pageTop - 0.01) {
                    shift += pageTop - at;
                    at = pageTop;
                } else if (at + h > pageTop + usable + 0.01 && at > pageTop + 0.01) {
                    shift += pageTop + stride - at;
                    at = pageTop + stride;
                }
                p.shifts.push_back(shift);
                lastTop = at;
            } while (pango_layout_iter_next_line(it));
            pango_layout_iter_free(it);
        }
        m_paragraphs.push_back(p);
        if (end >= s.size())
            break;
        start = end + 1;
    }
    if (m_pages)
        m_pageCount = size_t(std::max(0.0, std::floor(lastTop / (m_pages->paperHeight + PageSeparatorHeight)))) + 1;
}

double TextView::contentHeight() const
{
    layout();
    if (m_pages) {
        // updateFrame (0x10000b3a3): the pages and the separators between them
        const double n = double(m_pageCount);
        return n * m_pages->paperHeight + (n - 1) * PageSeparatorHeight;
    }
    return m_paragraphs.empty() ? 0 : m_paragraphs.back().bottom();
}

size_t TextView::paragraphOf(size_t index) const
{
    layout();
    for (size_t i = 0; i < m_paragraphs.size(); ++i)
        if (index <= m_paragraphs[i].end)
            return i;
    return m_paragraphs.size() - 1;
}

Rect TextView::caretRect(size_t index) const
{
    const Paragraph &p = m_paragraphs[paragraphOf(index)];
    const int at = int(std::min(index, p.end) - p.start);
    PangoRectangle strong;
    pango_layout_get_cursor_pos(p.layout, at, &strong, nullptr);
    int line = 0;
    pango_layout_index_to_line_x(p.layout, at, FALSE, &line, nullptr);
    return {textOriginX() + Padding + p.x + strong.x / double(PANGO_SCALE), p.y + p.shift(line) + strong.y / double(PANGO_SCALE), 1,
            strong.height / double(PANGO_SCALE)};
}

size_t TextView::indexAt(Point pt, bool above) const
{
    layout();
    const double y = pt.y + m_scroll, x = pt.x + m_scrollX - textOriginX() - Padding;
    if (!m_pages && y < 0)
        return 0;
    // the line: the first whose bottom is below the point (between pages, above: the one before)
    const Paragraph *p = nullptr, *prev = nullptr;
    int lineY = 0, prevY = 0;
    for (const Paragraph &q : m_paragraphs) {
        if (q.bottom() <= y) {
            prev = &q;
            prevY = int((q.height - 0.5) * PANGO_SCALE);
            continue;
        }
        PangoLayoutIter *it = pango_layout_get_iter(q.layout);
        int line = 0;
        do {
            int y0 = 0, y1 = 0;
            pango_layout_iter_get_line_yrange(it, &y0, &y1);
            const double shift = q.y + q.shift(line);
            if (y < shift + y1 / double(PANGO_SCALE)) {
                if (above && prev && y < shift + y0 / double(PANGO_SCALE)) {
                    p = prev;
                    lineY = prevY;
                } else {
                    p = &q;
                    lineY = (y0 + y1) / 2;
                }
                break;
            }
            prev = &q;
            prevY = (y0 + y1) / 2;
            ++line;
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        if (p)
            break;
    }
    if (!p)
        return m_text.size();
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(p->layout, int((x - p->x) * PANGO_SCALE), lineY, &index, &trailing);
    size_t at = p->start + size_t(index);
    for (int i = 0; i < trailing && at < p->end; ++i)
        at = nextChar(m_text.text, at);
    return std::min(at, p->end);
}

size_t TextView::lineStart(size_t index) const
{
    const Paragraph &p = m_paragraphs[paragraphOf(index)];
    int line = 0;
    pango_layout_index_to_line_x(p.layout, int(std::min(index, p.end) - p.start), FALSE, &line, nullptr);
    PangoLayoutLine *l = pango_layout_get_line_readonly(p.layout, line);
    return l ? p.start + size_t(l->start_index) : p.start;
}

size_t TextView::lineEnd(size_t index) const
{
    const Paragraph &p = m_paragraphs[paragraphOf(index)];
    int line = 0;
    pango_layout_index_to_line_x(p.layout, int(std::min(index, p.end) - p.start), FALSE, &line, nullptr);
    PangoLayoutLine *l = pango_layout_get_line_readonly(p.layout, line);
    if (!l || line == pango_layout_get_line_count(p.layout) - 1)
        return p.end;
    // before the space the line wrapped at
    size_t end = p.start + size_t(l->start_index + l->length);
    while (end > p.start + size_t(l->start_index) && m_text.text[end - 1] == ' ')
        --end;
    return end;
}

size_t TextView::wordStart(size_t i) const
{
    const std::string &s = m_text.text;
    while (i > 0 && !wordChar(s, prevChar(s, i)))
        i = prevChar(s, i);
    while (i > 0 && wordChar(s, prevChar(s, i)))
        i = prevChar(s, i);
    return i;
}

size_t TextView::wordEnd(size_t i) const
{
    const std::string &s = m_text.text;
    while (i < s.size() && !wordChar(s, i))
        i = nextChar(s, i);
    while (i < s.size() && wordChar(s, i))
        i = nextChar(s, i);
    return i;
}

// ---- drawing

void TextView::draw(cairo_t *cr)
{
    layout();
    // ScalingScrollView's scale factor: the clip view's bounds scaled, the text laid out the same
    cairo_save(cr);
    cairo_scale(cr, m_zoom, m_zoom);
    const Rect seen{0, 0, viewWidth(), viewHeight()};
    const double top = m_scroll, bottom = m_scroll + viewHeight();
    const double ox = textOriginX() + Padding;
    const double rightEdge = m_pages ? ox + textWidth() + Padding : viewWidth();
    if (m_pages) {
        // -[MultiplePageView drawRect:] (0x10000ba2e), on the screen: the clip view's controlColor
        // (DocumentWindow.nib) between and around the pages, each page in marginColor, its text
        // container (centerScanRect:, NSInsetRect -1) framed a point wide in lineColor
        fill(cr, seen, Color::gray(232));
        cairo_save(cr);
        cairo_translate(cr, -m_scrollX, -m_scroll);
        const PageGeometry &g = *m_pages;
        const double stride = g.paperHeight + PageSeparatorHeight;
        const size_t first = size_t(std::max(0.0, std::floor(top / stride)));
        for (size_t k = first; k < m_pageCount && double(k) * stride <= bottom; ++k) {
            const Rect page = pageRect(k);
            fill(cr, page, background);
            const Rect d{std::round(textOriginX()) - 1, std::round(page.y + g.top) - 1, std::round(textWidth() + 2 * Padding) + 2,
                         std::round(g.paperHeight - g.top - g.bottom) + 2};
            const Color line = Color::gray(170);   // lightGrayColor: white 2/3
            fill(cr, Rect{d.x, d.y, d.w, 1}, line);
            fill(cr, Rect{d.x, d.y + d.h - 1, d.w, 1}, line);
            fill(cr, Rect{d.x, d.y + 1, 1, d.h - 2}, line);
            fill(cr, Rect{d.x + d.w - 1, d.y + 1, 1, d.h - 2}, line);
        }
        cairo_restore(cr);
    } else {
        fill(cr, seen, background);
    }
    cairo_save(cr);
    cairo_translate(cr, -m_scrollX, -m_scroll);
    const size_t a = selectionStart(), b = selectionEnd();
    const bool key = windowIsKey() && isFirstResponder();
    const Color selection = key ? Color{181 / 255.0, 213 / 255.0, 1, 1} : Color::gray(212);
    for (const Paragraph &p : m_paragraphs) {
        if (p.top() > bottom || p.bottom() < top)
            continue;
        PangoLayoutIter *it = pango_layout_get_iter(p.layout);
        // the selection's background, line by line
        if (a != b && b >= p.start && a <= p.end + 1) {
            int lineNo = 0;
            do {
                const double ly = p.y + p.shift(lineNo++);
                PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
                int y0 = 0, y1 = 0;
                pango_layout_iter_get_line_yrange(it, &y0, &y1);
                const size_t ls = p.start + size_t(line->start_index), le = ls + size_t(line->length);
                const size_t from = std::max(a, ls), to = std::min(b, le);
                const bool last = !pango_layout_iter_at_last_line(it) ? false : true;
                if (from <= to) {
                    int *ranges = nullptr, n = 0;
                    pango_layout_line_get_x_ranges(line, int(from - p.start), int(to - p.start), &ranges, &n);
                    for (int r = 0; r < n; ++r)
                        fill(cr, Rect{ox + p.x + ranges[2 * r] / double(PANGO_SCALE), ly + y0 / double(PANGO_SCALE),
                                      (ranges[2 * r + 1] - ranges[2 * r]) / double(PANGO_SCALE), (y1 - y0) / double(PANGO_SCALE)},
                             selection);
                    g_free(ranges);
                    // the paragraph's end selected: to the right edge
                    if (last && b > p.end) {
                        PangoRectangle ext;
                        pango_layout_line_get_extents(line, nullptr, &ext);
                        int lx = 0;
                        pango_layout_line_index_to_x(line, int(p.end - p.start), FALSE, &lx);
                        const double x = ox + p.x + std::max(lx, ext.x + ext.width) / double(PANGO_SCALE);
                        fill(cr, Rect{x, ly + y0 / double(PANGO_SCALE), std::max(0.0, rightEdge - x), (y1 - y0) / double(PANGO_SCALE)}, selection);
                    }
                }
            } while (pango_layout_iter_next_line(it));
            pango_layout_iter_free(it);
            it = pango_layout_get_iter(p.layout);
        }
        pango_layout_iter_free(it);
    }
    drawGlyphs(cr, top, bottom);
    // the insertion point (NSInsertionColor: black)
    if (key && m_caretOn && a == b) {
        Rect r = caretRect(m_caret);
        fill(cr, Rect{std::round(r.x), std::round(r.y), 1, std::round(r.h)}, Color::gray(0));
    }
    cairo_restore(cr);
    cairo_restore(cr);
}

std::vector<double> TextView::lineBottoms() const
{
    layout();
    std::vector<double> out;
    for (const Paragraph &p : m_paragraphs) {
        PangoLayoutIter *it = pango_layout_get_iter(p.layout);
        int line = 0;
        do {
            int y0 = 0, y1 = 0;
            pango_layout_iter_get_line_yrange(it, &y0, &y1);
            out.push_back(p.y + p.shift(line++) + y1 / double(PANGO_SCALE));
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
    }
    return out;
}

// the glyphs, run by run, in their colours, and the pictures, of the paragraphs between top and bottom
void TextView::drawGlyphs(cairo_t *cr, double top, double bottom)
{
    layout();
    const double ox = textOriginX() + Padding;
    for (const Paragraph &p : m_paragraphs) {
        if (p.top() > bottom || p.bottom() < top)
            continue;
        PangoLayoutIter *it = pango_layout_get_iter(p.layout);
        do {
            PangoLayoutRun *run = pango_layout_iter_get_run_readonly(it);
            if (!run)
                continue;
            PangoRectangle logical;
            pango_layout_iter_get_run_extents(it, nullptr, &logical);
            int line = 0;
            pango_layout_index_to_line_x(p.layout, run->item->offset, FALSE, &line, nullptr);
            const double baseline = p.y + p.shift(line) + pango_layout_iter_get_baseline(it) / double(PANGO_SCALE);
            const double x = ox + p.x + logical.x / double(PANGO_SCALE);
            const size_t at = p.start + size_t(run->item->offset);
            const Style &st = m_text.styleAt(at);
            if (m_text.text.compare(at, 3, AttachmentCharacter) == 0) {
                const size_t n = m_text.attachmentIndex(at);
                if (n < m_text.attachments.size()) {
                    Attachment &att = m_text.attachments[n];
                    const Image img = attachmentImage(att);
                    const double w = logical.width / double(PANGO_SCALE), h = att.height > 0 ? att.height : 16;
                    if (!img.isNull())
                        img.draw(cr, Rect{x, baseline - h - rise(st), w, h});
                }
                continue;
            }
            // a link in NSTextView's linkTextAttributes: blue, underlined
            const bool linked = !st.link.empty();
            const Color c = linked ? Color{0, 0, 1, 1} : st.coloured ? st.colour : Color::gray(0);
            const double y = baseline - rise(st);
            // NSShadow first: black at its opacity, offset (y up), its blur spread as faint copies
            if (st.shadow.on && st.shadow.alpha > 0) {
                const double r = std::max(0.0, st.shadow.blur) / 2;
                const int n = r < 0.5 ? 1 : 4;
                double total = 0;
                for (int i = -n + 1; i < n; ++i)
                    for (int k = -n + 1; k < n; ++k)
                        total += 1;
                for (int i = -n + 1; i < n; ++i)
                    for (int k = -n + 1; k < n; ++k) {
                        const double ox = n > 1 ? r * i / (n - 1) : 0, oy = n > 1 ? r * k / (n - 1) : 0;
                        cairo_new_path(cr);
                        cairo_move_to(cr, x + st.shadow.dx + ox, y - st.shadow.dy + oy);
                        pango_cairo_glyph_string_path(cr, run->item->analysis.font, run->glyphs);
                        cairo_set_source_rgba(cr, 0, 0, 0, n > 1 ? std::min(1.0, st.shadow.alpha * 2.5 / total) : st.shadow.alpha);
                        cairo_fill(cr);
                    }
            }
            cairo_new_path(cr);
            cairo_move_to(cr, x, y);
            pango_cairo_glyph_string_path(cr, run->item->analysis.font, run->glyphs);
            c.set(cr);
            fillTextPath(cr, st.size);
            if (st.underline || st.strike || linked) {
                PangoFontMetrics *m = pango_font_get_metrics(run->item->analysis.font, nullptr);
                const double w = logical.width / double(PANGO_SCALE);
                if (st.underline || linked) {
                    const double t = std::max(1.0, pango_font_metrics_get_underline_thickness(m) / double(PANGO_SCALE));
                    const double pos = pango_font_metrics_get_underline_position(m) / double(PANGO_SCALE);
                    fill(cr, Rect{x, std::round(y - pos), w, std::round(t)}, c);
                    if (st.underline == 2)   // NSUnderlineStyleDouble: a second line under it
                        fill(cr, Rect{x, std::round(y - pos) + 2 * std::round(t), w, std::round(t)}, c);
                }
                if (st.strike) {
                    const double t = std::max(1.0, pango_font_metrics_get_strikethrough_thickness(m) / double(PANGO_SCALE));
                    const double pos = pango_font_metrics_get_strikethrough_position(m) / double(PANGO_SCALE);
                    if (st.strike == 2) {
                        fill(cr, Rect{x, std::round(y - pos) - std::round(t), w, std::round(t)}, c);
                        fill(cr, Rect{x, std::round(y - pos) + std::round(t), w, std::round(t)}, c);
                    } else {
                        fill(cr, Rect{x, std::round(y - pos), w, std::round(t)}, c);
                    }
                }
                pango_font_metrics_unref(m);
            }
        } while (pango_layout_iter_next_run(it));
        pango_layout_iter_free(it);
    }
}

// ---- the selection

void TextView::select(size_t from, size_t to)
{
    from = std::min(from, m_text.size());
    to = std::min(to, m_text.size());
    m_anchor = from;
    m_caret = to;
    m_typing = styleAtSelection();
    m_goalX = -1;
    restartBlink();
    setNeedsDisplay();
}

Style TextView::styleAtSelection() const
{
    if (m_text.text.empty())
        return m_text.styles.empty() ? m_typing : m_text.styles[0];
    const size_t at = selectionStart();
    if (hasSelection() || at == 0)
        return m_text.styleAt(at);
    // after the character before the insertion point, but its paragraph's alignment
    Style s = m_text.styleAt(at - 1);
    if (m_text.text[at - 1] == '\n' && at < m_text.size())
        s.align = m_text.styleAt(at).align;
    return s;
}

void TextView::moveCaret(size_t to, bool extend)
{
    m_caret = std::min(to, m_text.size());
    if (!extend)
        m_anchor = m_caret;
    m_typing = styleAtSelection();
    m_typingGroup = false;
    scrollToSelection();
    restartBlink();
    setNeedsDisplay();
}

void TextView::scrollToSelection()
{
    layout();
    const Rect r = caretRect(m_caret);
    const double h = viewHeight();
    if (r.y < m_scroll)
        m_scroll = r.y;
    else if (r.y + r.h > m_scroll + h)
        m_scroll = r.y + r.h - h;
    m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, contentHeight() - h));
    // across: the caret in sight (pages wider than the view)
    if (m_pages) {
        if (r.x - Padding < m_scrollX)
            m_scrollX = r.x - Padding;
        else if (r.x + 1 + Padding > m_scrollX + viewWidth())
            m_scrollX = r.x + 1 + Padding - viewWidth();
        m_scrollX = std::clamp(m_scrollX, 0.0, maxScrollOffsetX());
    }
    setNeedsDisplay();
}

double TextView::maxScrollOffset() const { return std::max(0.0, contentHeight() - viewHeight()); }

double TextView::maxScrollOffsetX() const { return std::max(0.0, contentWidth() - viewWidth()); }

void TextView::setZoom(double zoom)
{
    m_zoom = std::clamp(zoom, 0.01, 100.0);
    m_scroll = std::clamp(m_scroll, 0.0, maxScrollOffset());
    m_scrollX = std::clamp(m_scrollX, 0.0, maxScrollOffsetX());
    setNeedsDisplay();
}

void TextView::scrollToX(double x)
{
    m_scrollX = std::clamp(x, 0.0, maxScrollOffsetX());
    setNeedsDisplay();
}

void TextView::scrollTo(double y)
{
    m_scroll = std::clamp(y, 0.0, maxScrollOffset());
    setNeedsDisplay();
}

void TextView::setTabs(std::vector<double> stops, double interval)
{
    tabStops = std::move(stops);
    defaultTabInterval = interval;
    invalidateLayout();
}

void TextView::scrollWheel(Event &e)
{
    m_scroll = std::clamp(m_scroll + e.deltaY * 3 * 13, 0.0, std::max(0.0, contentHeight() - viewHeight()));
    setNeedsDisplay();
}

// ---- editing

void TextView::snapshot(bool typing)
{
    if (typing && m_typingGroup)
        return;
    m_undo.push_back({m_text, m_caret, m_anchor});
    if (m_undo.size() > 100)
        m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_typingGroup = typing;
}

void TextView::changed()
{
    m_text.compact();
    invalidateLayout();
    m_goalX = -1;
    scrollToSelection();
    restartBlink();
    if (onChange)
        onChange();
}

void TextView::insertText(const std::string &s)
{
    if (!editable)
        return;
    snapshot(true);
    Style typing = m_typing;
    typing.link.clear();   // NSTextView's typing attributes never carry the link
    const size_t a = selectionStart();
    m_text.erase(a, selectionEnd());
    m_text.insert(a, s, typing);
    m_caret = m_anchor = a + s.size();
    m_typing = typing;
    changed();
}

void TextView::replaceSelection(const RichText &with)
{
    if (!editable)
        return;
    snapshot(false);
    const size_t a = selectionStart();
    m_text.erase(a, selectionEnd());
    m_text.insert(a, with);
    for (Attachment &att : m_text.attachments)
        attachmentImage(att);
    m_caret = m_anchor = a + with.text.size();
    m_typing = styleAtSelection();
    changed();
}

void TextView::deleteSelection()
{
    if (!editable || !hasSelection())
        return;
    snapshot(false);
    const size_t a = selectionStart();
    const Style typing = m_text.styleAt(a);
    m_text.erase(a, selectionEnd());
    m_caret = m_anchor = a;
    m_typing = typing;
    changed();
}

void TextView::cut()
{
    if (!editable || !hasSelection())
        return;
    copy();
    deleteSelection();
}

void TextView::copy()
{
    if (!hasSelection())
        return;
    s_clip = m_text.slice(selectionStart(), selectionEnd());
    std::string plain;
    for (size_t i = 0; i < s_clip.text.size(); i += 1) {
        if (s_clip.text.compare(i, 3, AttachmentCharacter) == 0) {
            i += 2;
            continue;
        }
        plain += s_clip.text[i];
    }
    s_clipPlain = plain;
    copyToSystem(plain);
}

void TextView::paste(bool matchStyle)
{
    const std::string system = pasteFromSystem();
    if (!matchStyle && !s_clip.text.empty() && system == s_clipPlain) {
        replaceSelection(s_clip);
        return;
    }
    if (system.empty())
        return;
    RichText plain = fromPlainText(system, m_typing);
    replaceSelection(plain);
}

void TextView::undo()
{
    if (!editable || m_undo.empty())
        return;
    m_redo.push_back({m_text, m_caret, m_anchor});
    State s = std::move(m_undo.back());
    m_undo.pop_back();
    m_text = std::move(s.text);
    m_caret = s.caret;
    m_anchor = s.anchor;
    m_typingGroup = false;
    m_typing = styleAtSelection();
    changed();
}

void TextView::redo()
{
    if (!editable || m_redo.empty())
        return;
    m_undo.push_back({m_text, m_caret, m_anchor});
    State s = std::move(m_redo.back());
    m_redo.pop_back();
    m_text = std::move(s.text);
    m_caret = s.caret;
    m_anchor = s.anchor;
    m_typingGroup = false;
    m_typing = styleAtSelection();
    changed();
}

void TextView::changeStyle(const std::function<void(Style &)> &change)
{
    if (!editable)
        return;
    if (!hasSelection()) {
        change(m_typing);
        if (m_text.text.empty() && !m_text.styles.empty()) {
            // an empty note: its paragraph shows the new style already
            invalidateLayout();
        }
        return;
    }
    snapshot(false);
    m_text.setStyle(selectionStart(), selectionEnd(), change);
    m_typing = styleAtSelection();
    changed();
}

// ---- lists

namespace {

size_t paraStartOf(const std::string &s, size_t at)
{
    const size_t nl = at == 0 ? std::string::npos : s.rfind('\n', at - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

size_t paraEndOf(const std::string &s, size_t at)
{
    const size_t nl = s.find('\n', at);
    return nl == std::string::npos ? s.size() : nl;
}

// _standardMarkerAttributesForAttributes:: the item's font and colour, its paragraph; no link,
// underline, strike nor shift
Style markerStyle(Style s)
{
    s.link.clear();
    s.underline = s.strike = false;
    s.script = 0;
    s.baseline = 0;
    return s;
}

} // namespace

size_t TextView::listMarkerEnd(size_t start) const
{
    const std::string &s = m_text.text;
    if (start >= s.size() || s[start] != '\t' || m_text.styleAt(start).para.textLists.empty())
        return start;
    const size_t end = s.find('\t', start + 1), nl = s.find('\n', start);
    return end == std::string::npos || (nl != std::string::npos && end > nl) ? start : end + 1;
}

std::optional<TextList> TextView::selectedList() const
{
    const size_t at = paraStartOf(m_text.text, selectionStart());
    const Style st = at < m_text.size() ? m_text.styleAt(at) : m_typing;
    if (st.para.textLists.empty())
        return std::nullopt;
    return st.para.textLists.front();
}

void TextView::reformList(size_t at)
{
    // -_reformListAtIndex:: the run of list paragraphs around `at`, each item numbered within its
    // list (an item further out ends the lists inside it), its marker the enclosing item's first
    // when the list prepends it
    const std::string &s = m_text.text;
    if (s.empty())
        return;
    auto listsAt = [&](size_t start) -> std::vector<TextList> {
        const Style st = start < m_text.size() ? m_text.styleAt(start) : (start > 0 ? m_text.styleAt(start - 1) : m_typing);
        return st.para.textLists;
    };
    size_t p = paraStartOf(s, std::min(at, s.size()));
    if (listsAt(p).empty())
        return;
    while (p > 0 && !listsAt(paraStartOf(s, p - 1)).empty())
        p = paraStartOf(s, p - 1);
    struct Level {
        TextList list;
        long item;
        std::string marker;   // the item's marker as shown (for the lists inside that prepend it)
    };
    std::vector<Level> levels;
    for (;;) {
        const std::vector<TextList> lists = listsAt(p);
        if (lists.empty())
            break;
        const size_t d = lists.size();
        // the levels open still: the same lists, outermost first
        size_t keep = 0;
        while (keep < levels.size() && keep < d && levels[keep].list == lists[keep])
            ++keep;
        if (keep < d - 1 || keep > d)
            keep = std::min(keep, d - 1);
        levels.resize(keep);
        if (levels.size() == d) {
            ++levels.back().item;
        } else {
            while (levels.size() < d)
                levels.push_back({lists[levels.size()], lists[levels.size()].start, {}});
        }
        Level &here = levels.back();
        here.marker = here.list.marker(here.item);
        if ((here.list.options & 1) && d >= 2)
            here.marker = levels[d - 2].marker + here.marker;
        const std::string marker = "\t" + here.marker + "\t";
        const size_t old = listMarkerEnd(p);
        if (m_text.text.compare(p, old - p, marker) != 0 || old == p) {
            const Style st = markerStyle(p < m_text.size() ? m_text.styleAt(p) : m_typing);
            const long delta = long(marker.size()) - long(old - p);
            m_text.erase(p, old);
            m_text.insert(p, marker, st);
            auto shift = [&](size_t &x) {
                if (x >= old)
                    x = size_t(long(x) + delta);
                else if (x > p)
                    x = p + marker.size();
            };
            shift(m_caret);
            shift(m_anchor);
        }
        const size_t end = paraEndOf(m_text.text, p);
        if (end >= m_text.size())
            break;
        p = end + 1;
    }
}

namespace {

// the indents and tab stops of an item `depth` lists in: the head indent (depth × 36), a tab 25
// points before it and one at it, the paragraph's stops further out kept
void indentForList(ParagraphStyle &ps, size_t depth)
{
    const double indent = 36.0 * double(depth);
    ps.firstLineHeadIndent = 0;
    ps.headIndent = indent;
    std::vector<TabStop> kept;
    if (ps.hasTabs)
        for (const TabStop &t : ps.tabStops)
            if (t.location > indent + 36 || (t.location > indent && t.location != indent + 11 && t.location != indent + 36))
                kept.push_back(t);
    if (depth > 0)
        kept.insert(kept.begin(), {{indent - 25, TabStop::Left}, {indent, TabStop::Left}});
    ps.tabStops = kept;
    ps.hasTabs = true;
}

} // namespace

void TextView::nestList(size_t start)
{
    // -_nestListAtIndex: (0x5a502d): one list further in; the list of an item next to it at that
    // depth in the same enclosing item (looked for before it, then after), else a new {hyphen} one
    const std::string &s = m_text.text;
    const std::vector<TextList> lists = start < m_text.size() ? m_text.styleAt(start).para.textLists : m_typing.para.textLists;
    if (lists.empty())
        return;
    const size_t d = lists.size();
    auto listsAt = [&](size_t p) { return p < m_text.size() ? m_text.styleAt(p).para.textLists : std::vector<TextList>(); };
    auto sameOuter = [&](const std::vector<TextList> &l) { return l.size() >= d && std::equal(lists.begin(), lists.end(), l.begin()); };
    std::optional<std::vector<TextList>> found;
    for (size_t p = start; p > 0 && !found;) {
        p = paraStartOf(s, p - 1);
        const auto l = listsAt(p);
        if (!sameOuter(l))
            break;
        if (l.size() == d + 1)
            found = l;
    }
    for (size_t p = paraEndOf(s, start) + 1; p < s.size() && !found; p = paraEndOf(s, p) + 1) {
        const auto l = listsAt(p);
        if (!sameOuter(l))
            break;
        if (l.size() == d + 1)
            found = l;
    }
    std::vector<TextList> nested = found ? *found : lists;
    if (!found)
        nested.push_back(TextList{"{hyphen}", 0, 1});
    const size_t end = std::min(m_text.size(), paraEndOf(s, start) + 1);
    auto apply = [&nested](Style &st) {
        st.para.textLists = nested;
        indentForList(st.para, nested.size());
    };
    if (start < end)
        m_text.setStyle(start, end, apply);
    apply(m_typing);
    reformList(start);
}

void TextView::unnestList(size_t start)
{
    // -_unnestListAtIndex:markerRange:: one list further out; out of the outermost the marker goes
    // and the paragraph is back at the margin; the items after it numbered again
    const std::vector<TextList> lists = start < m_text.size() ? m_text.styleAt(start).para.textLists : m_typing.para.textLists;
    if (lists.size() > 1) {
        std::vector<TextList> outer(lists.begin(), lists.end() - 1);
        const size_t end = std::min(m_text.size(), paraEndOf(m_text.text, start) + 1);
        auto apply = [&outer](Style &st) {
            st.para.textLists = outer;
            indentForList(st.para, outer.size());
        };
        if (start < end)
            m_text.setStyle(start, end, apply);
        apply(m_typing);
        reformList(start);
        return;
    }
    const size_t end = listMarkerEnd(start);
    const size_t para = paraEndOf(m_text.text, start);
    const long removed = long(end - start);
    m_text.erase(start, end);
    auto shift = [&](size_t &x) {
        if (x >= end)
            x = size_t(long(x) - removed);
        else if (x > start)
            x = start;
    };
    shift(m_caret);
    shift(m_anchor);
    const size_t to = std::min(m_text.size(), size_t(long(para) - removed) + 1);
    auto clear = [](Style &s) {
        s.para.textLists.clear();
        s.para.firstLineHeadIndent = s.para.headIndent = 0;
    };
    if (start < to)
        m_text.setStyle(start, to, clear);
    clear(m_typing);
    const size_t next = paraEndOf(m_text.text, start) + 1;
    if (next < m_text.size())
        reformList(next);
}

void TextView::setListMarkerFormat(const std::string &format, unsigned options, long start)
{
    if (!editable)
        return;
    snapshot(false);
    const bool caretOnly = !hasSelection();
    size_t p = paraStartOf(m_text.text, selectionStart());
    const size_t lastStart = paraStartOf(m_text.text, selectionEnd() > selectionStart() && selectionEnd() > 0 &&
                                                          m_text.text[selectionEnd() - 1] == '\n' ? selectionEnd() - 1 : selectionEnd());
    // where the caret is in its item's text, kept there
    const size_t caretOffset = caretOnly ? m_caret - listMarkerEnd(p) : 0;
    const size_t first = p;
    TextList list{format, options, start};
    size_t count = 0;
    for (size_t guard = 0; guard < 100000; ++guard) {
        const bool last = p >= lastStart;
        ++count;
        const size_t mEnd = listMarkerEnd(p);
        const Style base = mEnd < m_text.size() && m_text.text[mEnd] != '\n' ? m_text.styleAt(mEnd) : (p < m_text.size() ? m_text.styleAt(p) : m_typing);
        auto setPara = [&](Style &s) {
            if (format.empty()) {
                s.para.textLists.clear();
                return;
            }
            // the paragraph's own list (its innermost) replaced, the ones around it kept
            if (s.para.textLists.empty())
                s.para.textLists = {list};
            else
                s.para.textLists.back() = list;
            const double indent = 36.0 * double(s.para.textLists.size());   // (level + 1) × 36
            s.para.firstLineHeadIndent = 0;
            s.para.headIndent = indent;
            std::vector<TabStop> kept;
            if (s.para.hasTabs)
                for (const TabStop &t : s.para.tabStops)
                    if (t.location > indent)
                        kept.push_back(t);
            kept.insert(kept.begin(), {{indent - 25, TabStop::Left}, {indent, TabStop::Left}});
            s.para.tabStops = kept;
            s.para.hasTabs = true;
        };
        m_text.erase(p, mEnd);
        if (!format.empty())
            m_text.insert(p, "\t\t", markerStyle(base));   // the marker's place, numbered below
        const size_t end = paraEndOf(m_text.text, p);
        m_text.setStyle(p, std::min(m_text.size(), end + 1), setPara);
        if (p >= m_text.size() || m_text.text.empty())
            setPara(m_typing);
        if (last || end >= m_text.size())
            break;
        p = end + 1;
    }
    if (!format.empty())
        reformList(first);
    Style typing = m_typing;
    if (format.empty())
        typing.para.textLists.clear();
    if (caretOnly) {
        m_caret = m_anchor = std::min(m_text.size(), listMarkerEnd(first) + caretOffset);
    } else {
        // the paragraphs changed, selected
        size_t e = first;
        for (size_t k = 0; k < count; ++k) {
            e = paraEndOf(m_text.text, e);
            if (k + 1 < count && e < m_text.size())
                ++e;
        }
        m_anchor = first;
        m_caret = std::min(m_text.size(), e);
    }
    m_typing = m_text.text.empty() ? typing : styleAtSelection();
    changed();
}

void TextView::setAlignment(Alignment al)
{
    changeParagraphStyle([al](Style &s) { s.align = al; });
}

void TextView::changeParagraphStyle(const std::function<void(Style &)> &change, bool undoable)
{
    // the selected paragraphs whole (NSTextView's rangeForUserParagraphAttributeChange), and the
    // typing attributes
    if (!editable)
        return;
    layout();
    const size_t from = m_paragraphs[paragraphOf(selectionStart())].start;
    const size_t to = std::min(m_text.size(), m_paragraphs[paragraphOf(selectionEnd())].end + 1);
    change(m_typing);
    if (from >= to) {
        invalidateLayout();
        return;
    }
    if (undoable)
        snapshot(false);
    m_text.setStyle(from, to, change);
    if (undoable) {
        changed();
    } else {
        // a ruler marker being dragged: laid out again, the change undone with the drag's first
        m_text.compact();
        invalidateLayout();
        if (onChange)
            onChange();
    }
}

void TextView::beginParagraphStyleChange()
{
    if (editable)
        snapshot(false);
}

void TextView::transform(int kind)
{
    if (!editable)
        return;
    size_t from = selectionStart(), to = selectionEnd();
    if (from == to) {
        from = wordStart(m_caret);
        to = wordEnd(from);
    }
    if (from >= to)
        return;
    snapshot(false);
    RichText out;
    bool wordStarted = false;
    for (size_t i = from; i < to;) {
        const size_t n = nextChar(m_text.text, i);
        const std::string ch = m_text.text.substr(i, n - i);
        const gunichar u = g_utf8_get_char(ch.c_str());
        gunichar t = u;
        if (kind == 0)
            t = g_unichar_toupper(u);
        else if (kind == 1)
            t = g_unichar_tolower(u);
        else
            t = g_unichar_isalnum(u) ? (wordStarted ? g_unichar_tolower(u) : g_unichar_totitle(u)) : u;
        wordStarted = g_unichar_isalnum(u);
        char buf[8] = {};
        const int len = ch == AttachmentCharacter ? 0 : g_unichar_to_utf8(t, buf);
        out.insert(out.text.size(), len ? std::string(buf, size_t(len)) : ch, m_text.styleAt(i));
        i = n;
    }
    const RichText before = m_text.slice(from, to);
    out.attachments = before.attachments;
    m_text.erase(from, to);
    m_text.insert(from, out);
    m_anchor = from;
    m_caret = from + out.text.size();
    changed();
}

// ---- the mouse and the keyboard

void TextView::mouseDown(Event &e)
{
    if (window())
        window()->makeFirstResponder(this);
    const size_t at = indexAt(Point{e.location.x / m_zoom, e.location.y / m_zoom});
    m_clickCount = e.clickCount;
    // a click on a link follows it (-clickedOnLink:atIndex:: NSWorkspace opens the URL)
    if (e.clickCount == 1 && !(e.modifiers & (Shift | Command)) && at < m_text.size() && !m_text.styleAt(at).link.empty()) {
        Workspace::openFile(m_text.styleAt(at).link);
        return;
    }
    if (e.clickCount == 2) {
        size_t s = at, t = at;
        if (wordChar(m_text.text, at) || (at > 0 && wordChar(m_text.text, prevChar(m_text.text, at)))) {
            s = wordChar(m_text.text, at) ? wordStart(nextChar(m_text.text, at)) : wordStart(at);
            t = wordEnd(s);
        } else if (at < m_text.size()) {
            t = nextChar(m_text.text, at);
        }
        m_dragOrigin = s;
        m_dragOriginEnd = t;
        select(s, t);
    } else if (e.clickCount >= 3) {
        layout();
        const Paragraph &p = m_paragraphs[paragraphOf(at)];
        m_dragOrigin = p.start;
        m_dragOriginEnd = std::min(m_text.size(), p.end + 1);
        select(m_dragOrigin, m_dragOriginEnd);
    } else if (e.modifiers & Shift) {
        m_caret = at;
        m_dragOrigin = m_dragOriginEnd = m_anchor;
        m_typing = styleAtSelection();
    } else {
        m_dragOrigin = m_dragOriginEnd = at;
        moveCaret(at, false);
    }
    m_typingGroup = false;
    setNeedsDisplay();
}

void TextView::mouseDragged(Event &e)
{
    size_t at = indexAt(Point{e.location.x / m_zoom, e.location.y / m_zoom});
    if (m_clickCount == 2) {
        if (at < m_dragOrigin)
            select(m_dragOriginEnd, wordStart(at));
        else
            select(m_dragOrigin, std::max(m_dragOriginEnd, wordEnd(at)));
    } else if (m_clickCount >= 3) {
        layout();
        const Paragraph &p = m_paragraphs[paragraphOf(at)];
        if (at < m_dragOrigin)
            select(m_dragOriginEnd, p.start);
        else
            select(m_dragOrigin, std::min(m_text.size(), p.end + 1));
    } else {
        m_anchor = m_dragOrigin;
        m_caret = at;
        m_typing = styleAtSelection();
    }
    // past the top or the bottom: it scrolls
    if (e.location.y < 0 || e.location.y > frame().h)
        scrollToSelection();
    setNeedsDisplay();
}

void TextView::mouseUp(Event &) {}

void TextView::keyDown(Event &e)
{
    if (e.modifiers & (Command | Control)) {
        View::keyDown(e);   // the menus' key equivalents
        return;
    }
    layout();
    const bool shift = e.modifiers & Shift, option = e.modifiers & Option;
    const std::string &s = m_text.text;
    switch (e.keysym) {
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left:
        if (hasSelection() && !shift)
            moveCaret(selectionStart(), false);
        else
            moveCaret(option ? wordStart(m_caret) : prevChar(s, m_caret), shift);
        return;
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right:
        if (hasSelection() && !shift)
            moveCaret(selectionEnd(), false);
        else
            moveCaret(option ? wordEnd(m_caret) : nextChar(s, m_caret), shift);
        return;
    case XKB_KEY_Up:
    case XKB_KEY_Down:
    case XKB_KEY_KP_Up:
    case XKB_KEY_KP_Down: {
        const bool up = e.keysym == XKB_KEY_Up || e.keysym == XKB_KEY_KP_Up;
        const Rect r = caretRect(up && hasSelection() && !shift ? selectionStart() : !up && hasSelection() && !shift ? selectionEnd() : m_caret);
        if (m_goalX < 0)
            m_goalX = r.x;
        const double goal = m_goalX;
        size_t to;
        // from the first line up: the start; from the last down: the end
        if (up)
            to = r.y <= m_paragraphs.front().top() + 0.5 ? 0 : indexAt({goal - m_scrollX, r.y - 1 - m_scroll}, true);
        else
            to = r.y + r.h >= m_paragraphs.back().bottom() - 0.5 ? s.size() : indexAt({goal - m_scrollX, r.y + r.h + 1 - m_scroll});
        moveCaret(to, shift);
        m_goalX = goal;
        return;
    }
    case XKB_KEY_Home:
        moveCaret(0, shift);
        return;
    case XKB_KEY_End:
        moveCaret(s.size(), shift);
        return;
    case XKB_KEY_Page_Up:
        m_scroll = std::max(0.0, m_scroll - viewHeight());
        setNeedsDisplay();
        return;
    case XKB_KEY_Page_Down:
        m_scroll = std::clamp(m_scroll + viewHeight(), 0.0, std::max(0.0, contentHeight() - viewHeight()));
        setNeedsDisplay();
        return;
    case XKB_KEY_BackSpace:
        if (!editable)
            return;
        if (hasSelection()) {
            deleteSelection();
        } else if (const size_t ps = paraStartOf(s, m_caret); m_caret > ps && m_caret == listMarkerEnd(ps)) {
            // -deleteBackward: just after an item's marker: out of the list
            snapshot(false);
            unnestList(ps);
            m_typing = styleAtSelection();
            changed();
        } else if (m_caret > 0) {
            snapshot(true);
            const size_t p = option ? wordStart(m_caret) : prevChar(s, m_caret);
            const Style typing = m_text.styleAt(p);
            m_text.erase(p, m_caret);
            m_caret = m_anchor = p;
            m_typing = typing;
            changed();
        }
        return;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        if (!editable)
            return;
        if (hasSelection()) {
            deleteSelection();
        } else if (m_caret < s.size()) {
            snapshot(true);
            m_text.erase(m_caret, option ? wordEnd(m_caret) : nextChar(s, m_caret));
            changed();
        }
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter: {
        // -insertNewline: in a list (_checkInList:…): an empty item leaves the list, else the new
        // paragraph is the next item (-_reformListAtIndex:)
        const size_t ps = paraStartOf(s, m_caret);
        const size_t mEnd = editable && !hasSelection() ? listMarkerEnd(ps) : ps;
        if (mEnd > ps && m_caret >= mEnd) {
            if (paraEndOf(s, ps) == mEnd) {
                snapshot(false);
                unnestList(ps);
                changed();
                return;
            }
            insertText("\n");
            const size_t np = m_caret;
            Style st = m_text.styleAt(np > 0 ? np - 1 : 0);
            m_text.insert(np, "\t\t", markerStyle(st));
            if (np < m_text.size())
                m_text.setStyle(np, std::min(m_text.size(), paraEndOf(m_text.text, np) + 1), [&st](Style &x) { x.para = st.para; });
            m_caret = m_anchor = np + 2;
            reformList(np);
            m_typing = styleAtSelection();
            changed();
            return;
        }
        insertText("\n");
        return;
    }
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab: {
        // -insertTab: just after an item's marker nests it, -insertBacktab: (Shift-Tab) takes it out
        const size_t ps = paraStartOf(s, m_caret);
        const size_t mEnd = editable && !hasSelection() ? listMarkerEnd(ps) : ps;
        const bool back = shift || e.keysym == XKB_KEY_ISO_Left_Tab;
        if (mEnd > ps && m_caret == mEnd) {
            snapshot(false);
            if (back)
                unnestList(ps);
            else
                nestList(ps);
            m_typing = styleAtSelection();
            changed();
            return;
        }
        if (!back)
            insertText("\t");
        return;
    }
    case XKB_KEY_Escape:
        return;
    default:
        break;
    }
    if (!e.text.empty() && static_cast<unsigned char>(e.text[0]) >= 0x20 && e.text[0] != 0x7f)
        insertText(e.text);
}

void TextView::focusChanged()
{
    restartBlink();
    setNeedsDisplay();
}

void TextView::restartBlink()
{
    App &app = App::shared();
    if (m_blink)
        app.removeTimer(m_blink);
    m_blink = 0;
    m_caretOn = true;
    if (!isFirstResponder())
        return;
    m_blink = app.addTimer(0.5, true, [this] {
        m_caretOn = !m_caretOn;
        setNeedsDisplay();
    });
}

// ---- TextScrollView

TextScrollView::TextScrollView() { m_text = add<TextView>(); }

double TextScrollView::top() const { return isRulerVisible() ? TextRulerView::requiredThickness() : 0; }

bool TextScrollView::isRulerVisible() const { return m_ruler && !m_ruler->isHidden(); }

void TextScrollView::setRulerVisible(bool visible)
{
    if (visible && !m_ruler)
        m_ruler = add<TextRulerView>(m_text);
    if (m_ruler)
        m_ruler->setHidden(!visible);
    layoutSubviews();
    setNeedsDisplay();
}

void TextScrollView::setPages(std::optional<TextView::PageGeometry> pages)
{
    m_text->setPages(pages);
    layoutSubviews();
    setNeedsDisplay();
}

void TextScrollView::setFrame(const Rect &r)
{
    View::setFrame(r);
    layoutSubviews();
}

void TextScrollView::layoutSubviews()
{
    const Rect r = frame();
    const double t = top(), bottom = m_text->hasPages() ? CoreUI::ScrollerWidth : 0;
    if (m_ruler)
        m_ruler->setFrame({0, 0, r.w, TextRulerView::requiredThickness()});
    m_text->setFrame({0, t, std::max(0.0, r.w - CoreUI::ScrollerWidth), std::max(0.0, r.h - t - bottom)});
}

void TextScrollView::setScales(std::vector<std::pair<std::string, double>> scales, int selected)
{
    m_scales = std::move(scales);
    m_scale = selected;
    setNeedsDisplay();
}

double TextScrollView::scalePopupWidth() const
{
    if (m_scales.empty() || !m_text->hasPages())
        return 0;
    // -sizeToFit: the widest title and the arrows
    double w = 0;
    for (const auto &[title, v] : m_scales)
        w = std::max(w, textWidth(title, Font::system(10)));
    return std::ceil(w) + 22;
}

Rect TextScrollView::scalePopupRect() const
{
    // -tile: at the shortened horizontal scroller's end, a point taller
    const double sw = CoreUI::ScrollerWidth, pw = scalePopupWidth();
    return {frame().w - sw - pw, frame().h - sw - 1 + 1, pw, sw};
}

Rect TextScrollView::scrollerRect(bool horizontal) const
{
    const double t = top(), w = frame().w, h = frame().h, sw = CoreUI::ScrollerWidth;
    if (horizontal)
        return {0, h - sw, std::max(0.0, w - sw - scalePopupWidth()), sw};
    return {w - sw, t, sw, std::max(0.0, h - t - (m_text->hasPages() ? sw : 0))};
}

double TextScrollView::proportion(bool horizontal) const
{
    const double total = horizontal ? m_text->contentWidth() : m_text->contentHeight();
    const double shown = horizontal ? m_text->viewWidth() : m_text->viewHeight();
    return total > shown ? shown / total : 1;
}

double TextScrollView::value(bool horizontal) const
{
    const double range = horizontal ? m_text->maxScrollOffsetX() : m_text->maxScrollOffset();
    const double at = horizontal ? m_text->scrollOffsetX() : m_text->scrollOffset();
    return range > 0 ? at / range : 0;
}

void TextScrollView::draw(cairo_t *cr)
{
    // the ruler follows the selection's paragraph (NSTextView's updateRuler) and the page across
    if (isRulerVisible()) {
        m_ruler->originOffset = m_text->textOriginX() + TextView::Padding - m_text->scrollOffsetX();
        m_ruler->update();
    }
    // drawn after the text (its subview) has scrolled: the knob follows it
    CoreUI::scroller(cr, scrollerRect(), value(), proportion(), controlState(*this), m_horizontal ? 0 : m_arrow);
    if (m_text->hasPages()) {
        CoreUI::scroller(cr, scrollerRect(true), value(true), proportion(true), controlState(*this), m_horizontal ? m_arrow : 0);
        // the corner between the scrollers
        const double sw = CoreUI::ScrollerWidth;
        fill(cr, Rect{frame().w - sw, frame().h - sw, sw, sw}, Color::gray(255));
        if (scalePopupWidth() > 0) {
            // ScalingScrollView's popup: a shadowless square bezel, its title, the arrows; the black
            // line before it (-drawRect:)
            const Rect r = scalePopupRect();
            cairo_pattern_t *g = cairo_pattern_create_linear(0, r.y, 0, r.bottom());
            cairo_pattern_add_color_stop_rgb(g, 0, 0.98, 0.98, 0.98);
            cairo_pattern_add_color_stop_rgb(g, 1, 0.86, 0.86, 0.86);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
            fill(cr, Rect{r.x - 1, r.y, 1, r.h}, Color::gray(0));
            fill(cr, Rect{r.x, r.y, r.w, 1}, Color::gray(170));
            TextStyle t;
            t.font = Font::system(10);
            if (m_scale >= 0 && size_t(m_scale) < m_scales.size())
                drawText(cr, m_scales[size_t(m_scale)].first, Rect{r.x + 6, r.y + 1, r.w - 18, r.h - 1}, t);
            // the arrows, up and down
            cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
            const double ax = r.right() - 9, ay = r.y + r.h / 2;
            cairo_move_to(cr, ax - 3, ay - 1);
            cairo_line_to(cr, ax + 3, ay - 1);
            cairo_line_to(cr, ax, ay - 4.5);
            cairo_close_path(cr);
            cairo_move_to(cr, ax - 3, ay + 1);
            cairo_line_to(cr, ax + 3, ay + 1);
            cairo_line_to(cr, ax, ay + 4.5);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
}

void TextScrollView::mouseDown(Event &e)
{
    if (scalePopupWidth() > 0 && scalePopupRect().contains(e.location)) {
        // -scalePopUpAction:: the scale chosen, the text scaled
        std::vector<MenuItem> items;
        for (size_t k = 0; k < m_scales.size(); ++k)
            items.push_back(MenuItem(m_scales[k].first, false, true, int(k) == m_scale));
        const Rect r = scalePopupRect();
        Menu::popUp(window(), toWindow(Point{r.x, r.y - 4}), items, r.w, Font::system(10), [this](int i) {
            if (i < 0 || size_t(i) >= m_scales.size())
                return;
            m_scale = i;
            m_text->setZoom(m_scales[size_t(i)].second);
            if (onScale)
                onScale(m_scales[size_t(i)].second);
            setNeedsDisplay();
        });
        return;
    }
    for (const bool horizontal : {false, true}) {
        if (horizontal && !m_text->hasPages())
            continue;
        const Rect sr = scrollerRect(horizontal);
        if (!sr.contains(e.location))
            continue;
        if (proportion(horizontal) >= 1)
            return;
        m_horizontal = horizontal;
        const Point p{e.location.x - sr.x, e.location.y - sr.y};
        const CoreUI::ScrollerParts parts = CoreUI::scrollerParts({0, 0, sr.w, sr.h}, value(horizontal), proportion(horizontal));
        if (parts.knob.contains(p)) {
            m_dragFrom = horizontal ? p.x : p.y;
            m_dragOffset = horizontal ? m_text->scrollOffsetX() : m_text->scrollOffset();
            m_dragging = true;
        } else if (parts.up.contains(p) || parts.down.contains(p)) {
            m_arrow = parts.up.contains(p) ? 1 : 2;
            step();
            m_repeat = App::shared().addTimer(0.35, false, [this] { m_repeat = App::shared().addTimer(0.05, true, [this] { step(); }); });
        } else if (horizontal) {
            m_text->scrollToX(m_text->scrollOffsetX() + (p.x < parts.knob.x ? -1 : 1) * (m_text->frame().w - lineStep));
        } else {
            // a page towards the click
            m_text->scrollTo(m_text->scrollOffset() + (p.y < parts.knob.y ? -1 : 1) * (m_text->frame().h - lineStep));
        }
        setNeedsDisplay();
        return;
    }
}

void TextScrollView::mouseDragged(Event &e)
{
    if (!m_dragging)
        return;
    const Rect sr = scrollerRect(m_horizontal);
    const CoreUI::ScrollerParts parts = CoreUI::scrollerParts({0, 0, sr.w, sr.h}, value(m_horizontal), proportion(m_horizontal));
    if (m_horizontal) {
        const double travel = parts.track.w - parts.knob.w;
        if (travel > 0)
            m_text->scrollToX(m_dragOffset + (e.location.x - sr.x - m_dragFrom) * m_text->maxScrollOffsetX() / travel);
    } else {
        const double travel = parts.track.h - parts.knob.h;
        if (travel > 0)
            m_text->scrollTo(m_dragOffset + (e.location.y - sr.y - m_dragFrom) * m_text->maxScrollOffset() / travel);
    }
}

void TextScrollView::mouseUp(Event &)
{
    m_dragging = false;
    m_arrow = 0;
    if (m_repeat)
        App::shared().removeTimer(m_repeat);
    m_repeat = 0;
    setNeedsDisplay();
}

void TextScrollView::step()
{
    const double by = (m_arrow == 1 ? -1 : 1) * lineStep;
    if (m_horizontal)
        m_text->scrollToX(m_text->scrollOffsetX() + by);
    else
        m_text->scrollTo(m_text->scrollOffset() + by);
}

} // namespace AK
