// SPDX-License-Identifier: LGPL-2.1-or-later
#include "text.h"

#include "foundation.h"

#include "quartztext.h"

#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>   // after fontconfig.h
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

// The layout's glyphs at the current point, as Quartz draws them with font smoothing
// (common/quartztext.h): the unhinted outlines, dilated, then on an image whose pixels can be read
// (a window's) the LCD coverage mixed into the pixels under the text; elsewhere (a group, a clip
// that isn't rectangles, smoothing turned off) the dilated outlines filled in grey.
void showLayout(cairo_t *cr, PangoLayout *l, double size)
{
    cairo_save(cr);
    cairo_set_tolerance(cr, 0.02);
    pango_cairo_layout_path(cr, l);
    fillTextPath(cr, size);
    cairo_restore(cr);
}

} // namespace

PangoContext *pangoContext() { return context(); }

void showTextLayout(cairo_t *cr, PangoLayout *l, double size) { showLayout(cr, l, size); }

void fillTextPath(cairo_t *cr, double size)
{
    cairo_surface_t *target = cairo_get_group_target(cr);
    double offsetX = 0, offsetY = 0;
    cairo_surface_get_device_offset(target, &offsetX, &offsetY);
    cairo_save(cr);
    cairo_set_tolerance(cr, 0.02);
    cairo_path_t *flat = cairo_copy_path_flat(cr);
    cairo_new_path(cr);
    // in the target's own pixels
    QuartzText::Contours contours;
    for (int i = 0; i < flat->num_data; i += flat->data[i].header.length) {
        const cairo_path_data_t *d = &flat->data[i];
        if (d->header.type != CAIRO_PATH_MOVE_TO && d->header.type != CAIRO_PATH_LINE_TO)
            continue;
        double x = d[1].point.x, y = d[1].point.y;
        cairo_user_to_device(cr, &x, &y);
        if (d->header.type == CAIRO_PATH_MOVE_TO)
            contours.emplace_back();
        if (!contours.empty())
            contours.back().push_back({x + offsetX, y + offsetY});
    }
    cairo_path_destroy(flat);
    double scale = 1, unused = 0;
    cairo_user_to_device_distance(cr, &scale, &unused);
    contours = QuartzText::dilatedForSize(std::move(contours), size * std::fabs(scale));

    bool drawn = false;
    double r = 0, g = 0, b = 0, a = 1;
    const cairo_format_t format =
        cairo_surface_get_type(target) == CAIRO_SURFACE_TYPE_IMAGE ? cairo_image_surface_get_format(target) : CAIRO_FORMAT_INVALID;
    if (QuartzText::smoothing() && (format == CAIRO_FORMAT_RGB24 || format == CAIRO_FORMAT_ARGB32) &&
        cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a) == CAIRO_STATUS_SUCCESS) {
        cairo_rectangle_list_t *clip = cairo_copy_clip_rectangle_list(cr);
        if (clip->status == CAIRO_STATUS_SUCCESS) {
            const QuartzText::Mask mask = QuartzText::lcdMask(contours);
            cairo_surface_flush(target);
            for (int i = 0; i < clip->num_rectangles && mask.w > 0; ++i) {
                double x0 = clip->rectangles[i].x, y0 = clip->rectangles[i].y;
                double x1 = x0 + clip->rectangles[i].width, y1 = y0 + clip->rectangles[i].height;
                cairo_user_to_device(cr, &x0, &y0);
                cairo_user_to_device(cr, &x1, &y1);
                QuartzText::blend(reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(target)),
                                  cairo_image_surface_get_stride(target), cairo_image_surface_get_width(target),
                                  cairo_image_surface_get_height(target), format == CAIRO_FORMAT_ARGB32,
                                  int(std::lround(x0 + offsetX)), int(std::lround(y0 + offsetY)), int(std::lround(x1 + offsetX)),
                                  int(std::lround(y1 + offsetY)), mask, r, g, b, a);
            }
            if (mask.w > 0)
                cairo_surface_mark_dirty_rectangle(target, mask.x, mask.y, mask.w, mask.h);
            drawn = true;
        }
        cairo_rectangle_list_destroy(clip);
    }
    if (!drawn) {
        cairo_identity_matrix(cr);
        for (const std::vector<QuartzText::Point> &c : contours) {
            for (size_t i = 0; i < c.size(); ++i)
                (i ? cairo_line_to : cairo_move_to)(cr, c[i].x - offsetX, c[i].y - offsetY);
            cairo_close_path(cr);
        }
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        cairo_fill(cr);
    }
    cairo_restore(cr);
    cairo_new_path(cr);
}

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

// ---- CTFontManager: the DVD's fonts made the programs' own (fontconfig's application set)

namespace FontManager {

void registerSystemFonts()
{
    for (const char *font : {"/System/Library/Fonts/LucidaGrande.ttc", "/System/Library/Fonts/HelveticaNeue.ttc"})
        if (Assets::exists(font))
            FcConfigAppFontAddFile(FcConfigGetCurrent(), reinterpret_cast<const FcChar8 *>(Assets::path(font).c_str()));
}

void registerAllFonts()
{
    // Each face as fontconfig reads it, its weight taken from its style's name when the font has no
    // OS/2 table to say it (Helvetica.dfont's Bold reads as a regular weight, and would stand for it)
    FcConfig *config = FcConfigGetCurrent();
    for (const char *dir : {"/System/Library/Fonts", "/Library/Fonts"}) {
        for (const std::string &f : files(Assets::path(dir))) {
            const auto *file = reinterpret_cast<const FcChar8 *>(f.c_str());
            FcFontSet *set = FcConfigGetFonts(config, FcSetApplication);
            if (!set) {
                FcConfigAppFontAddFile(config, file);
                continue;
            }
            int count = 1;
            for (int i = 0; i < count; ++i) {
                FcPattern *face = FcFreeTypeQuery(file, i, nullptr, &count);
                if (!face)
                    break;
                FcChar8 *style = nullptr;
                int weight = FC_WEIGHT_REGULAR;
                if (FcPatternGetString(face, FC_STYLE, 0, &style) == FcResultMatch &&
                    FcPatternGetInteger(face, FC_WEIGHT, 0, &weight) == FcResultMatch) {
                    const std::string st = reinterpret_cast<const char *>(style);
                    int want = weight;
                    if (st.find("Black") != std::string::npos || st.find("Heavy") != std::string::npos)
                        want = FC_WEIGHT_BLACK;
                    else if (st.find("Bold") != std::string::npos)
                        want = FC_WEIGHT_BOLD;
                    else if (st.find("Light") != std::string::npos)
                        want = FC_WEIGHT_LIGHT;
                    if (want != weight && weight == FC_WEIGHT_REGULAR) {
                        FcPatternDel(face, FC_WEIGHT);
                        FcPatternAddInteger(face, FC_WEIGHT, want);
                    }
                }
                if (!FcFontSetAdd(set, face))
                    FcPatternDestroy(face);
            }
        }
    }
}

} // namespace FontManager

} // namespace AK
