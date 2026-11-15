// SPDX-License-Identifier: LGPL-2.1-or-later
#include "text.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace AK {

namespace {

PangoContext *context()
{
    static PangoContext *ctx = [] {
        PangoFontMap *map = pango_cairo_font_map_get_default();
        PangoContext *c = pango_font_map_create_context(map);
        cairo_font_options_t *o = cairo_font_options_create();
        // Quartz: no hinting, grey antialiasing, fractional advances
        cairo_font_options_set_antialias(o, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(o, CAIRO_HINT_STYLE_NONE);
        cairo_font_options_set_hint_metrics(o, CAIRO_HINT_METRICS_OFF);
        pango_cairo_context_set_font_options(c, o);
        cairo_font_options_destroy(o);
        pango_context_set_round_glyph_positions(c, FALSE);
        return c;
    }();
    return ctx;
}

PangoLayout *layout(const std::string &text, const Font &font)
{
    PangoLayout *l = pango_layout_new(context());
    PangoFontDescription *d = pango_font_description_from_string(
        font.family.empty() ? "Lucida Grande, Helvetica Neue, sans-serif" : (font.family + ", Lucida Grande, sans-serif").c_str());
    pango_font_description_set_absolute_size(d, font.size * PANGO_SCALE);
    pango_font_description_set_weight(d, font.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);
    pango_layout_set_text(l, text.c_str(), int(text.size()));
    pango_layout_set_single_paragraph_mode(l, TRUE);
    return l;
}

// The layout's glyphs at the current point, as Quartz draws them with font smoothing: unhinted
// outlines, dilated (CGFontCreateGlyphBitmap32, with the medium smoothing style's
// CGFontGetDilationParametersForStyle: 0.0121 and 0.015125 of the size, 0.3 pixel at most). The
// dilation is path_dilator_iterate's: every segment of the outline is moved, across by the first
// outwards (to the right when it goes up, to the left when it goes down) and, when it runs along the
// top of a stroke, up by twice the second; those along the bottom stay.
void showLayout(cairo_t *cr, PangoLayout *l, double size)
{
    const double dx = std::min(0.3, 0.0121 * size), dy = std::min(0.3, 0.015125 * size);
    struct Point {
        double x, y;
    };
    cairo_save(cr);
    cairo_set_tolerance(cr, 0.02);
    pango_cairo_layout_path(cr, l);
    cairo_path_t *flat = cairo_copy_path_flat(cr);
    cairo_new_path(cr);
    std::vector<std::vector<Point>> contours;
    for (int i = 0; i < flat->num_data; i += flat->data[i].header.length) {
        const cairo_path_data_t *d = &flat->data[i];
        if (d->header.type == CAIRO_PATH_MOVE_TO)
            contours.push_back({{d[1].point.x, d[1].point.y}});
        else if (d->header.type == CAIRO_PATH_LINE_TO && !contours.empty())
            contours.back().push_back({d[1].point.x, d[1].point.y});
    }
    cairo_path_destroy(flat);
    // the way the outer contours turn: as Quartz's own orientation pass leaves them
    double area = 0;
    for (std::vector<Point> &c : contours) {
        if (c.size() > 1 && c.front().x == c.back().x && c.front().y == c.back().y)
            c.pop_back();
        for (size_t i = 0; i < c.size(); ++i) {
            const Point &a = c[i], &b = c[(i + 1) % c.size()];
            area += a.x * b.y - b.x * a.y;
        }
    }
    for (std::vector<Point> &c : contours) {
        if (area > 0)
            std::reverse(c.begin(), c.end());
        bool first = true;
        for (size_t i = 0; i < c.size(); ++i) {
            const Point &a = c[i], &b = c[(i + 1) % c.size()];
            const double across = b.x - a.x, down = b.y - a.y;
            if (across == 0 && down == 0)
                continue;
            const double ox = down < 0 ? dx : down > 0 ? -dx : 0, oy = across > 0 ? 0 : across < 0 ? -2 * dy : -dy;
            if (first)
                cairo_move_to(cr, a.x + ox, a.y + oy);
            else
                cairo_line_to(cr, a.x + ox, a.y + oy);
            cairo_line_to(cr, b.x + ox, b.y + oy);
            first = false;
        }
        if (!first)
            cairo_close_path(cr);
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_fill(cr);
    cairo_restore(cr);
}

} // namespace

double textWidth(const std::string &text, const Font &font)
{
    PangoLayout *l = layout(text, font);
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    g_object_unref(l);
    return logical.width / double(PANGO_SCALE);
}

double lineHeight(const Font &font)
{
    PangoLayout *l = layout("Xg", font);
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    g_object_unref(l);
    return logical.height / double(PANGO_SCALE);
}

void drawWrappedText(cairo_t *cr, const std::string &text, const Rect &r, const TextStyle &st)
{
    if (text.empty())
        return;
    PangoLayout *l = layout(text, st.font);
    pango_layout_set_single_paragraph_mode(l, FALSE);
    pango_layout_set_width(l, int(r.w * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(l, st.align == Align::Center ? PANGO_ALIGN_CENTER : st.align == Align::Right ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    if (st.engraved) {
        cairo_set_source_rgba(cr, 1, 1, 1, st.engravedAlpha);
        cairo_move_to(cr, r.x, r.y + 1);
        showLayout(cr, l, st.font.size);
    }
    st.color.set(cr);
    cairo_move_to(cr, r.x, r.y);
    showLayout(cr, l, st.font.size);
    cairo_restore(cr);
    g_object_unref(l);
}

double wrappedHeight(const std::string &text, double width, const Font &font)
{
    PangoLayout *l = layout(text, font);
    pango_layout_set_single_paragraph_mode(l, FALSE);
    pango_layout_set_width(l, int(width * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    g_object_unref(l);
    return logical.height / double(PANGO_SCALE);
}

double ascent(const Font &font)
{
    PangoLayout *l = layout("X", font);
    const double a = pango_layout_get_baseline(l) / double(PANGO_SCALE);
    g_object_unref(l);
    return a;
}

void drawText(cairo_t *cr, const std::string &text, const Rect &r, const TextStyle &st)
{
    if (text.empty())
        return;
    PangoLayout *l = layout(text, st.font);
    if (st.elide) {
        pango_layout_set_width(l, int(r.w * PANGO_SCALE));
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(l, st.align == Align::Center ? PANGO_ALIGN_CENTER : st.align == Align::Right ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    }
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    const double w = logical.width / double(PANGO_SCALE), h = logical.height / double(PANGO_SCALE);
    double x = r.x;
    if (!st.elide && st.align == Align::Center)
        x = r.x + (r.w - w) / 2;
    else if (!st.elide && st.align == Align::Right)
        x = r.right() - w;
    // the baseline on a whole pixel, as Quartz puts it
    const double y = std::round(r.y + (r.h - h) / 2 + pango_layout_get_baseline(l) / double(PANGO_SCALE)) -
                     pango_layout_get_baseline(l) / double(PANGO_SCALE);
    cairo_save(cr);
    if (st.engraved) {
        cairo_set_source_rgba(cr, 1, 1, 1, st.engravedAlpha);
        cairo_move_to(cr, x, y + 1);
        showLayout(cr, l, st.font.size);
    }
    st.color.set(cr);
    cairo_move_to(cr, x, y);
    showLayout(cr, l, st.font.size);
    cairo_restore(cr);
    g_object_unref(l);
}

} // namespace AK
