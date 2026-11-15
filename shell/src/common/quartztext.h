// SPDX-License-Identifier: LGPL-2.1-or-later
// Quartz's font smoothing on an LCD, as 10.6 does it (CoreGraphics, docs/reverse-status.md): the
// glyphs' outlines scan-converted four times finer across than a pixel, each pixel's red, green and
// blue coverage taken from those samples through a filter of 19 values of its own
// (CGSScanconverterRenderMask, CGSScanConvolveAndIntegrateRGB: rKernel, gKernel, bKernel), kept at
// five bits each, then the text's colour and the pixel under it mixed in each of the three by that
// coverage, in a gamma space whose power is the font smoothing contrast, 2 (CGSColorDeepMaskCopy-
// XRGB8888: the square root of the squares' mix). Where the pixel under it isn't opaque the text is
// grey, as Quartz's is.
//
// No Qt and no cairo here: AquaKit (text.cpp) and the shell's Qt programs (aqua.cpp) both use it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace QuartzText {

struct Point {
    double x, y;
};
using Contours = std::vector<std::vector<Point>>;   // closed, in the pixels of what is drawn on

// The coverage of a rectangle of pixels at (x, y): red, green and blue, five bits each (0RRRRRGGGGGBBBBB)
struct Mask {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<uint16_t> bits;
};

// AppleFontSmoothing, as an environment variable of the session: 0 turns the LCD smoothing off
inline bool smoothing()
{
    static const bool on = [] {
        const char *v = std::getenv("SNOWLEOPARD_FONT_SMOOTHING");
        return !v || std::atoi(v) != 0;
    }();
    return on;
}

// Quartz's dilation of a glyph's outline (path_dilator_iterate): every segment is moved, across by
// dx outwards (to the right when it goes up, to the left when it goes down) and, when it runs along
// the top of a stroke, up by twice dy; those along the bottom stay. The outer contours are made to
// turn the way Quartz's own orientation pass leaves them.
inline Contours dilated(Contours contours, double dx, double dy)
{
    double area = 0;
    for (std::vector<Point> &c : contours) {
        if (c.size() > 1 && c.front().x == c.back().x && c.front().y == c.back().y)
            c.pop_back();
        for (size_t i = 0; i < c.size(); ++i) {
            const Point &a = c[i], &b = c[(i + 1) % c.size()];
            area += a.x * b.y - b.x * a.y;
        }
    }
    Contours out;
    for (std::vector<Point> &c : contours) {
        if (area > 0)
            std::reverse(c.begin(), c.end());
        std::vector<Point> moved;
        for (size_t i = 0; i < c.size(); ++i) {
            const Point &a = c[i], &b = c[(i + 1) % c.size()];
            const double across = b.x - a.x, down = b.y - a.y;
            if (across == 0 && down == 0)
                continue;
            const double ox = down < 0 ? dx : down > 0 ? -dx : 0, oy = across > 0 ? 0 : across < 0 ? -2 * dy : -dy;
            moved.push_back({a.x + ox, a.y + oy});
            moved.push_back({b.x + ox, b.y + oy});
        }
        if (moved.size() > 2)
            out.push_back(std::move(moved));
    }
    return out;
}

// The medium smoothing style's dilation for a size (CGFontGetDilationParametersForStyle, 0x30;
// 0.3 pixel at most)
inline Contours dilatedForSize(Contours contours, double size)
{
    return dilated(std::move(contours), std::min(0.3, 0.0121 * size), std::min(0.3, 0.015125 * size));
}

namespace detail {

constexpr int Oversampling = 4, Taps = 19;
// a sample's place among a pixel's taps: an edge on the pixel's left side falls on the eighth
constexpr int FirstTap = 7;
constexpr int16_t RedKernel[Taps] = {1, 0, -13, -30, -21, 41, 148, 257, 297, 238, 129, 30, -21, -24, -9, 0, 0, 0, 0};
constexpr int16_t GreenKernel[Taps] = {0, 0, 1, -11, -30, -27, 33, 143, 258, 312, 251, 133, 26, -30, -29, -9, 2, 0, 0};
constexpr int16_t BlueKernel[Taps] = {0, 0, -1, -2, -10, -21, -14, 40, 137, 238, 290, 247, 141, 38, -21, -29, -12, 0, 1};

// One edge into the rows' running areas (the signed area to its right, as the scan converter keeps
// it): x in samples, y in rows
inline void edge(std::vector<float> &area, int width, int height, Point p0, Point p1)
{
    if (p0.y == p1.y)
        return;
    const float dir = p0.y < p1.y ? 1.f : -1.f;
    if (p0.y > p1.y)
        std::swap(p0, p1);
    const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    double x = p0.x;
    if (p0.y < 0)
        x -= p0.y * dxdy;
    const int last = std::min(height, int(std::ceil(p1.y)));
    for (int y = std::max(0, int(std::floor(p0.y))); y < last; ++y) {
        float *row = area.data() + size_t(y) * size_t(width);
        const double dy = std::min(double(y + 1), p1.y) - std::max(double(y), p0.y);
        const double xnext = x + dxdy * dy;
        const float d = float(dy) * dir;
        const double x0 = std::min(x, xnext), x1 = std::max(x, xnext);
        const double x0floor = std::floor(x0), x1ceil = std::ceil(x1);
        const int x0i = int(x0floor), x1i = int(x1ceil);
        if (x0i >= 0 && x1i + 1 < width) {
            if (x1i <= x0i + 1) {
                const float xmf = float(0.5 * (x + xnext) - x0floor);
                row[x0i] += d - d * xmf;
                row[x0i + 1] += d * xmf;
            } else {
                const float s = float(1.0 / (x1 - x0));
                const float x0f = float(x0 - x0floor), a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
                const float x1f = float(x1 - x1ceil + 1), am = 0.5f * s * x1f * x1f;
                row[x0i] += d * a0;
                if (x1i == x0i + 2) {
                    row[x0i + 1] += d * (1 - a0 - am);
                } else {
                    const float a1 = s * (1.5f - x0f);
                    row[x0i + 1] += d * (a1 - a0);
                    for (int xi = x0i + 2; xi < x1i - 1; ++xi)
                        row[xi] += d * s;
                    const float a2 = a1 + float(x1i - x0i - 3) * s;
                    row[x1i - 1] += d * (1 - a2 - am);
                }
                row[x1i] += d * am;
            }
        }
        x = xnext;
    }
}

} // namespace detail

inline Mask lcdMask(const Contours &contours)
{
    using namespace detail;
    Mask m;
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (const std::vector<Point> &c : contours)
        for (const Point &p : c) {
            minX = std::min(minX, p.x), maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y), maxY = std::max(maxY, p.y);
        }
    if (minX > maxX || maxX - minX > 16384 || maxY - minY > 16384)
        return m;
    // the filter reaches a little over two pixels on each side
    m.x = int(std::floor(minX)) - 3;
    m.y = int(std::floor(minY));
    m.w = int(std::ceil(maxX)) + 3 - m.x;
    m.h = int(std::ceil(maxY)) - m.y + 1;
    const int samples = m.w * Oversampling, width = samples + 3;
    std::vector<float> area(size_t(width) * size_t(m.h), 0.f);
    for (const std::vector<Point> &c : contours)
        for (size_t i = 0; i < c.size(); ++i) {
            const Point &a = c[i], &b = c[(i + 1) % c.size()];
            edge(area, width, m.h, {(a.x - m.x) * Oversampling, a.y - m.y}, {(b.x - m.x) * Oversampling, b.y - m.y});
        }
    m.bits.assign(size_t(m.w) * size_t(m.h), 0);
    std::vector<float> coverage(static_cast<size_t>(samples));
    for (int y = 0; y < m.h; ++y) {
        const float *row = area.data() + size_t(y) * size_t(width);
        float sum = 0;
        bool any = false;
        for (int i = 0; i < samples; ++i) {
            sum += row[i];
            coverage[size_t(i)] = std::min(1.f, std::fabs(sum));
            any |= coverage[size_t(i)] > 0;
        }
        if (!any)
            continue;
        for (int x = 0; x < m.w; ++x) {
            float r = 0, g = 0, b = 0;
            for (int k = 0; k < Taps; ++k) {
                const int i = x * Oversampling + k - FirstTap;
                if (i < 0 || i >= samples)
                    continue;
                const float c = coverage[size_t(i)];
                r += RedKernel[k] * c, g += GreenKernel[k] * c, b += BlueKernel[k] * c;
            }
            // a sample wholly covered is 4080 in the scan converter's own buffer; 14 bits down
            auto level = [](float v) { return std::clamp((int(std::lround(v * 4080)) + 0x1c00) >> 14, 0, 255); };
            m.bits[size_t(y) * size_t(m.w) + size_t(x)] =
                uint16_t(((level(r) & 0xf8) << 7) | ((level(g) & 0xf8) << 2) | (level(b) >> 3));
        }
    }
    return m;
}

// The mask onto rows of 32-bit pixels (0xAARRGGBB, premultiplied; alpha unused when !hasAlpha), inside
// the clip rectangle [cx0, cx1) × [cy0, cy1), in the colour r, g, b (0 to 1) at the opacity a
inline void blend(uint32_t *pixels, int stride, int width, int height, bool hasAlpha, int cx0, int cy0, int cx1, int cy1,
                  const Mask &m, double r, double g, double b, double a)
{
    static float squares[256];
    static const bool ready = [] {
        for (int i = 0; i < 256; ++i)
            squares[i] = float(i * i) / (255.f * 255.f);
        return true;
    }();
    (void)ready;
    const int x0 = std::max({m.x, cx0, 0}), x1 = std::min({m.x + m.w, cx1, width});
    const int y0 = std::max({m.y, cy0, 0}), y1 = std::min({m.y + m.h, cy1, height});
    const float colour[3] = {float(r), float(g), float(b)};
    const float colourSquared[3] = {float(r * r), float(g * g), float(b * b)};
    const float opacity = float(a);
    for (int y = y0; y < y1; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(pixels) + size_t(y) * size_t(stride));
        const uint16_t *bits = m.bits.data() + size_t(y - m.y) * size_t(m.w) - m.x;
        for (int x = x0; x < x1; ++x) {
            const uint16_t v = bits[x];
            if (!v)
                continue;
            // five bits to eight, as the server's own table does
            const int five[3] = {(v >> 10) & 31, (v >> 5) & 31, v & 31};
            float cover[3];
            for (int c = 0; c < 3; ++c)
                cover[c] = float((five[c] << 3) | (five[c] >> 2)) / 255.f;
            const uint32_t under = row[x];
            const int alpha = hasAlpha ? int(under >> 24) : 255;
            int out[3];
            if (alpha == 255) {
                for (int c = 0; c < 3; ++c) {
                    const int d = int((under >> (16 - 8 * c)) & 255);
                    const float mixed = std::sqrt(squares[d] + (colourSquared[c] - squares[d]) * cover[c]) * 255.f;
                    out[c] = std::clamp(int(std::lround(float(d) + (mixed - float(d)) * opacity)), 0, 255);
                }
                row[x] = 0xff000000u | uint32_t(out[0]) << 16 | uint32_t(out[1]) << 8 | uint32_t(out[2]);
            } else {
                // not opaque under it: grey, over
                const float grey = (cover[0] + cover[1] + cover[2]) / 3 * opacity;
                for (int c = 0; c < 3; ++c) {
                    const int d = int((under >> (16 - 8 * c)) & 255);
                    out[c] = std::clamp(int(std::lround(float(d) * (1 - grey) + colour[c] * 255.f * grey)), 0, 255);
                }
                const int outAlpha = std::clamp(int(std::lround(float(alpha) * (1 - grey) + 255.f * grey)), 0, 255);
                row[x] = uint32_t(outAlpha) << 24 | uint32_t(out[0]) << 16 | uint32_t(out[1]) << 8 | uint32_t(out[2]);
            }
        }
    }
}

} // namespace QuartzText
