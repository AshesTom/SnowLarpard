// SPDX-License-Identifier: LGPL-2.1-or-later
// Text as AppKit sets it on 10.6: Lucida Grande (the DVD's), sizes in points (one point a pixel),
// unhinted outlines with fractional advances, dilated as Quartz's font smoothing dilates them, grey
// antialiasing.
#pragma once

#include "graphics.h"

#include <string>

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

} // namespace AK
