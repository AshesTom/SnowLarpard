// SPDX-License-Identifier: LGPL-2.1-or-later
#include "textview.h"

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

using namespace AK;

namespace Stickies {

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
    if (wider)
        invalidateLayout();
    m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, contentHeight() - r.h));
}

void TextView::invalidateLayout()
{
    m_layoutValid = false;
    setNeedsDisplay();
}

// ---- layout: a pango layout for each paragraph

void TextView::layout() const
{
    const double width = std::max(1.0, frame().w - 2 * Padding);
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
    PangoTabArray *tabs = pango_tab_array_new(12, TRUE);
    for (int i = 0; i < 12; ++i)
        pango_tab_array_set_tab(tabs, i, PANGO_TAB_LEFT, 28 * (i + 1));   // \tx560 ... (28 points apart)
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
        pango_layout_set_width(p.layout, int(width * PANGO_SCALE));
        pango_layout_set_wrap(p.layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_tabs(p.layout, tabs);
        // its style: the first character's (an empty paragraph's: the one typing would use)
        const Style &first = start < s.size() ? m_text.styleAt(start) : (start > 0 ? m_text.styleAt(start - 1) : m_typing);
        const Style &para = part.empty() && start == s.size() ? m_typing : first;
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
        PangoRectangle logical;
        pango_layout_get_extents(p.layout, nullptr, &logical);
        p.y = y;
        p.height = logical.height / double(PANGO_SCALE);
        y += p.height;
        m_paragraphs.push_back(p);
        if (end >= s.size())
            break;
        start = end + 1;
    }
    pango_tab_array_free(tabs);
}

double TextView::contentHeight() const
{
    layout();
    return m_paragraphs.empty() ? 0 : m_paragraphs.back().y + m_paragraphs.back().height;
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
    PangoRectangle strong;
    pango_layout_get_cursor_pos(p.layout, int(std::min(index, p.end) - p.start), &strong, nullptr);
    return {Padding + strong.x / double(PANGO_SCALE), p.y + strong.y / double(PANGO_SCALE), 1, strong.height / double(PANGO_SCALE)};
}

size_t TextView::indexAt(Point pt) const
{
    layout();
    const double y = pt.y + m_scroll;
    if (y < 0)
        return 0;
    const Paragraph *p = &m_paragraphs.back();
    for (const Paragraph &q : m_paragraphs)
        if (y < q.y + q.height) {
            p = &q;
            break;
        }
    if (y >= p->y + p->height && p == &m_paragraphs.back())
        return m_text.size();
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(p->layout, int((pt.x - Padding) * PANGO_SCALE), int((y - p->y) * PANGO_SCALE), &index, &trailing);
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
    fill(cr, bounds(), background);
    cairo_save(cr);
    cairo_translate(cr, 0, -m_scroll);
    const size_t a = selectionStart(), b = selectionEnd();
    const bool key = windowIsKey() && isFirstResponder();
    const Color selection = key ? Color{181 / 255.0, 213 / 255.0, 1, 1} : Color::gray(212);
    const double top = m_scroll, bottom = m_scroll + frame().h;
    for (const Paragraph &p : m_paragraphs) {
        if (p.y > bottom || p.y + p.height < top)
            continue;
        PangoLayoutIter *it = pango_layout_get_iter(p.layout);
        // the selection's background, line by line
        if (a != b && b >= p.start && a <= p.end + 1) {
            do {
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
                        fill(cr, Rect{Padding + ranges[2 * r] / double(PANGO_SCALE), p.y + y0 / double(PANGO_SCALE),
                                      (ranges[2 * r + 1] - ranges[2 * r]) / double(PANGO_SCALE), (y1 - y0) / double(PANGO_SCALE)},
                             selection);
                    g_free(ranges);
                    // the paragraph's end selected: to the right edge
                    if (last && b > p.end) {
                        PangoRectangle ext;
                        pango_layout_line_get_extents(line, nullptr, &ext);
                        int lx = 0;
                        pango_layout_line_index_to_x(line, int(p.end - p.start), FALSE, &lx);
                        const double x = Padding + std::max(lx, ext.x + ext.width) / double(PANGO_SCALE);
                        fill(cr, Rect{x, p.y + y0 / double(PANGO_SCALE), std::max(0.0, frame().w - x), (y1 - y0) / double(PANGO_SCALE)}, selection);
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
}

std::vector<double> TextView::lineBottoms() const
{
    layout();
    std::vector<double> out;
    for (const Paragraph &p : m_paragraphs) {
        PangoLayoutIter *it = pango_layout_get_iter(p.layout);
        do {
            int y0 = 0, y1 = 0;
            pango_layout_iter_get_line_yrange(it, &y0, &y1);
            out.push_back(p.y + y1 / double(PANGO_SCALE));
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
    }
    return out;
}

// the glyphs, run by run, in their colours, and the pictures, of the paragraphs between top and bottom
void TextView::drawGlyphs(cairo_t *cr, double top, double bottom)
{
    layout();
    for (const Paragraph &p : m_paragraphs) {
        if (p.y > bottom || p.y + p.height < top)
            continue;
        PangoLayoutIter *it = pango_layout_get_iter(p.layout);
        do {
            PangoLayoutRun *run = pango_layout_iter_get_run_readonly(it);
            if (!run)
                continue;
            PangoRectangle logical;
            pango_layout_iter_get_run_extents(it, nullptr, &logical);
            const double baseline = p.y + pango_layout_iter_get_baseline(it) / double(PANGO_SCALE);
            const double x = Padding + logical.x / double(PANGO_SCALE);
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
            const Color c = st.coloured ? st.colour : Color::gray(0);
            const double y = baseline - rise(st);
            cairo_move_to(cr, x, y);
            pango_cairo_glyph_string_path(cr, run->item->analysis.font, run->glyphs);
            c.set(cr);
            fillTextPath(cr, st.size);
            if (st.underline || st.strike) {
                PangoFontMetrics *m = pango_font_get_metrics(run->item->analysis.font, nullptr);
                const double w = logical.width / double(PANGO_SCALE);
                if (st.underline) {
                    const double t = std::max(1.0, pango_font_metrics_get_underline_thickness(m) / double(PANGO_SCALE));
                    const double pos = pango_font_metrics_get_underline_position(m) / double(PANGO_SCALE);
                    fill(cr, Rect{x, std::round(y - pos), w, std::round(t)}, c);
                }
                if (st.strike) {
                    const double t = std::max(1.0, pango_font_metrics_get_strikethrough_thickness(m) / double(PANGO_SCALE));
                    const double pos = pango_font_metrics_get_strikethrough_position(m) / double(PANGO_SCALE);
                    fill(cr, Rect{x, std::round(y - pos), w, std::round(t)}, c);
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
    const double h = frame().h;
    if (r.y < m_scroll)
        m_scroll = r.y;
    else if (r.y + r.h > m_scroll + h)
        m_scroll = r.y + r.h - h;
    m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, contentHeight() - h));
    setNeedsDisplay();
}

void TextView::scrollWheel(Event &e)
{
    m_scroll = std::clamp(m_scroll + e.deltaY * 3 * 13, 0.0, std::max(0.0, contentHeight() - frame().h));
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
    snapshot(true);
    const Style typing = m_typing;
    const size_t a = selectionStart();
    m_text.erase(a, selectionEnd());
    m_text.insert(a, s, typing);
    m_caret = m_anchor = a + s.size();
    m_typing = typing;
    changed();
}

void TextView::replaceSelection(const RichText &with)
{
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
    if (!hasSelection())
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
    if (!hasSelection())
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
    if (m_undo.empty())
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
    if (m_redo.empty())
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

void TextView::setAlignment(Alignment al)
{
    layout();
    const size_t from = m_paragraphs[paragraphOf(selectionStart())].start;
    const size_t to = std::min(m_text.size(), m_paragraphs[paragraphOf(selectionEnd())].end + 1);
    m_typing.align = al;
    if (from >= to) {
        invalidateLayout();
        return;
    }
    snapshot(false);
    m_text.setStyle(from, to, [al](Style &s) { s.align = al; });
    changed();
}

void TextView::transform(int kind)
{
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
    const size_t at = indexAt(e.location);
    m_clickCount = e.clickCount;
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
    size_t at = indexAt(e.location);
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
        if (up)
            to = r.y <= 0.5 ? 0 : indexAt({goal, r.y - 1 - m_scroll});
        else
            to = r.y + r.h >= contentHeight() - 0.5 ? s.size() : indexAt({goal, r.y + r.h + 1 - m_scroll});
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
        m_scroll = std::max(0.0, m_scroll - frame().h);
        setNeedsDisplay();
        return;
    case XKB_KEY_Page_Down:
        m_scroll = std::clamp(m_scroll + frame().h, 0.0, std::max(0.0, contentHeight() - frame().h));
        setNeedsDisplay();
        return;
    case XKB_KEY_BackSpace:
        if (hasSelection()) {
            deleteSelection();
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
        if (hasSelection()) {
            deleteSelection();
        } else if (m_caret < s.size()) {
            snapshot(true);
            m_text.erase(m_caret, option ? wordEnd(m_caret) : nextChar(s, m_caret));
            changed();
        }
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        insertText("\n");
        return;
    case XKB_KEY_Tab:
        insertText("\t");
        return;
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

} // namespace Stickies
