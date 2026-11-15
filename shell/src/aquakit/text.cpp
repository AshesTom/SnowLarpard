// SPDX-License-Identifier: LGPL-2.1-or-later
#include "text.h"

#include <pango/pangocairo.h>

#include <cmath>

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
        pango_cairo_show_layout(cr, l);
    }
    st.color.set(cr);
    cairo_move_to(cr, r.x, r.y);
    pango_cairo_show_layout(cr, l);
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
        pango_cairo_show_layout(cr, l);
    }
    st.color.set(cr);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    cairo_restore(cr);
    g_object_unref(l);
}

} // namespace AK
