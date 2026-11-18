// SPDX-License-Identifier: LGPL-2.1-or-later
// Text as AppKit sets it on 10.6: Lucida Grande (the DVD's), sizes in points (one point a pixel),
// unhinted outlines with fractional advances, dilated as Quartz's font smoothing dilates them, and its
// LCD smoothing on a window's pixels (grey where they can't be read).
#pragma once

#include "graphics.h"

#include <string>

typedef struct _PangoContext PangoContext;
typedef struct _PangoLayout PangoLayout;

namespace AK {

struct Font {
    double size = 13;   // [NSFont systemFontSize]
    bool bold = false;
    std::string family;   // empty: Lucida Grande
    static Font system(double size = 13) { return {size, false, {}}; }
    static Font boldSystem(double size = 13) { return {size, true, {}}; }
};

enum class Align { Left, Center, Right };

struct TextStyle {
    Font font;
    Color color = Color::gray(0);
    Align align = Align::Left;
    bool engraved = false;   // a white copy one point under (the title bar's, the toolbar's labels)
    double engravedAlpha = 0.5;
    bool elide = true;       // "…" at the end when it doesn't fit
};

double textWidth(const std::string &text, const Font &font);
double lineHeight(const Font &font);
// The text in r on one line, centred vertically on its line box as NSCell does
void drawText(cairo_t *cr, const std::string &text, const Rect &r, const TextStyle &style);
// The text over several lines, wrapped at words, from the top of r
void drawWrappedText(cairo_t *cr, const std::string &text, const Rect &r, const TextStyle &style);
double wrappedHeight(const std::string &text, double width, const Font &font);
// The baseline's distance from the line's top (for a field's text and its caret)
double ascent(const Font &font);

// For text laid out by the caller (NSLayoutManager's own: several fonts and colours): AquaKit's pango
// context (Quartz's font options), a layout drawn at the current point in the source colour as
// drawText draws, and the current path (glyphs the caller added: pango_cairo_glyph_string_path) filled
// the same way, with Quartz's dilation for a font of `size` and its LCD smoothing; the path is cleared.
PangoContext *pangoContext();
void showTextLayout(cairo_t *cr, PangoLayout *layout, double size);
void fillTextPath(cairo_t *cr, double size);

// CTFontManagerRegisterFontsForURL for the DVD's fonts
namespace FontManager {
// Lucida Grande and Helvetica Neue, for the text
void registerSystemFonts();
// Every font of /System/Library/Fonts and /Library/Fonts (a Stickies note's Helvetica, Times, Marker
// Felt...), for the programs whose documents name them
void registerAllFonts();
} // namespace FontManager

} // namespace AK
