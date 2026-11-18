// SPDX-License-Identifier: LGPL-2.1-or-later
// AquaKit's drawing types, without Qt: points, rectangles and colours as Quartz has them (top left
// origin, as AppKit's flipped views), images as cairo surfaces in CoreUI's own pixel format (8 bits a
// channel, alpha premultiplied: cairo's ARGB32 is that), and the files the DVD and the user give
// (PNG, JPEG, TIFF, icns, PDF).
#pragma once

#include <cairo.h>

#include <cstdint>
#include <memory>
#include <string>

namespace AK {

struct Point {
    double x = 0, y = 0;
};

struct Size {
    double w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(Point p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    Rect inset(double dx, double dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect translated(double dx, double dy) const { return {x + dx, y + dy, w, h}; }
};

struct Color {
    double r = 0, g = 0, b = 0, a = 1;
    static Color gray(int v, double alpha = 1) { return {v / 255.0, v / 255.0, v / 255.0, alpha}; }
    static Color rgb(int r, int g, int b, double alpha = 1) { return {r / 255.0, g / 255.0, b / 255.0, alpha}; }
    // A colour in Generic RGB (CoreUI's _CUIColorSpaceGetRGB: its gradients' _kPairs, kColorTable; AppKit's
    // calibrated colours), matched to the screen as ColorSync does: lighter in its middle tones
    static Color generic(int r, int g, int b, double alpha = 1);
    static Color genericGray(int v, double alpha = 1) { return generic(v, v, v, alpha); }
    Color matched() const;   // this colour's values taken as Generic RGB's
    static Color parse(const std::string &hex);   // "#rrggbb"
    std::string name() const;                     // "#rrggbb"
    void set(cairo_t *cr) const { cairo_set_source_rgba(cr, r, g, b, a); }
};

void fill(cairo_t *cr, const Rect &r, const Color &c);

class Image {
public:
    Image() = default;
    explicit Image(cairo_surface_t *adopt);   // takes the reference
    Image(int width, int height);             // transparent
    bool isNull() const { return !m_surface; }
    int width() const;
    int height() const;
    Size size() const { return {double(width()), double(height())}; }
    cairo_surface_t *surface() const { return m_surface.get(); }

    // into dst, stretched (the way CGContextDrawImage does), with a source rectangle if given
    void draw(cairo_t *cr, const Rect &dst, double alpha = 1) const;
    void draw(cairo_t *cr, const Rect &src, const Rect &dst, double alpha = 1) const;
    void draw(cairo_t *cr, Point at, double alpha = 1) const { draw(cr, Rect{at.x, at.y, double(width()), double(height())}, alpha); }
    Image scaled(int w, int h) const;
    Image tinted(const Color &c) const;   // a template image in one colour (its alpha kept)
    Image copy(const Rect &part) const;

    // A file: PNG, JPEG (decoded at the smallest scale still covering `atLeast`), icns (the
    // representation closest above `atLeast`), TIFF, PDF (its first page drawn at the scale covering
    // `atLeast`, else at its size in points). Empty on failure.
    static Image load(const std::string &path, Size atLeast = {});
    // A picture's own size without decoding it (a PDF's: its page in points)
    static Size sizeOf(const std::string &path);
    bool savePng(const std::string &path) const;

private:
    std::shared_ptr<cairo_surface_t> m_surface;
};

// Rectangles as Cocoa's placements give them: an image's size inside a box
Size scaledToFit(Size s, Size box);     // NSImageScaleProportionallyDown/UpOrDown
Size scaledToFill(Size s, Size box);    // fill, cropping

} // namespace AK
