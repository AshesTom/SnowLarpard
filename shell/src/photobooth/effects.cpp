// SPDX-License-Identifier: LGPL-2.1-or-later
// The compositions' graphs, read from the DVD's .qtz files (binary property lists: rootPatch, its
// nodes' classes, their input port values and the connections). Every number below is a
// composition's. What is the composition's own code: the kernels of Fish Eye (fishEye), Stretch
// (funHouse), Comic Book (bw), Pop Art (spotColor) and the distortions' CIMirrorEdge. What stands
// for Core Image's filters (their kernels are in QuartzCore, not read yet): CISepiaTone,
// CIColorMonochrome, CIColorControls, CIEdges, CIBloom, CIGloom, CICMYKHalftone, CIColorMap and the
// bump, pinch, twirl and circle splash distortions, written from what the filters are documented to
// do; in Core Image's linear working space (the pixels taken as Generic RGB's, gamma 1.8).
#include "effects.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace PB {

using namespace AK;

const EffectInfo &effectInfo(int effect)
{
    static const EffectInfo table[EffectCount] = {
        {"/sepia", "Sepia", false},
        {"/blackandwhite", "Black and White", false},
        {"/glow", "Glow", false},
        {"/comicbook", "Comic Book", false},
        {"/colorpencil", "Color Pencil", false},
        {"/thermalcamera", "Thermal Camera", false},
        {"/xray", "X-Ray", false},
        {"/popart", "Pop Art", false},
        {"/bulge", "Bulge", true},
        {"/dent", "Dent", true},
        {"/twirl", "Twirl", true},
        {"/squeeze", "Squeeze", true},
        {"/mirror", "Mirror", false},
        {"/lighttunnel", "Light Tunnel", true},
        {"/fisheye", "Fish Eye", false},
        {"/stretch", "Stretch", false},
    };
    static const EffectInfo normal = {"", "Normal", false};
    return effect >= 0 && effect < EffectCount ? table[effect] : normal;
}

namespace {

struct Planes {
    int w = 0, h = 0;
    std::vector<float> r, g, b;
    Planes() = default;
    Planes(int width, int height) : w(width), h(height), r(size_t(width) * height), g(r.size()), b(r.size()) {}
    std::vector<float> &plane(int c) { return c == 0 ? r : c == 1 ? g : b; }
    const std::vector<float> &plane(int c) const { return c == 0 ? r : c == 1 ? g : b; }
};

// Core Image works in a linear working space and 10.6's pictures are Generic RGB's (gamma 1.8):
// the pixels are decoded on the way in and encoded on the way out
const float Gamma = 1.8f;
const float *decodeTable()
{
    static float table[256];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < 256; ++i)
            table[i] = std::pow(i / 255.0f, Gamma);
        made = true;
    }
    return table;
}
const unsigned char *encodeTable()
{
    static unsigned char table[4097];
    static bool made = false;
    if (!made) {
        for (int i = 0; i <= 4096; ++i)
            table[i] = static_cast<unsigned char>(std::pow(i / 4096.0f, 1 / Gamma) * 255.0f + 0.5f);
        made = true;
    }
    return table;
}
inline float linear(float v) { return std::pow(v, Gamma); }

Planes fromImage(const Image &image)
{
    Planes p(image.width(), image.height());
    cairo_surface_flush(image.surface());
    const unsigned char *data = cairo_image_surface_get_data(image.surface());
    const int stride = cairo_image_surface_get_stride(image.surface());
    const float *decode = decodeTable();
    for (int y = 0; y < p.h; ++y) {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
        const size_t o = size_t(y) * p.w;
        for (int x = 0; x < p.w; ++x) {
            p.r[o + x] = decode[(row[x] >> 16) & 255];
            p.g[o + x] = decode[(row[x] >> 8) & 255];
            p.b[o + x] = decode[row[x] & 255];
        }
    }
    return p;
}

Image toImage(const Planes &p)
{
    Image image(p.w, p.h);
    unsigned char *data = cairo_image_surface_get_data(image.surface());
    const int stride = cairo_image_surface_get_stride(image.surface());
    const unsigned char *encode = encodeTable();
    auto byte = [encode](float v) { return uint32_t(encode[int(std::clamp(v, 0.0f, 1.0f) * 4096.0f)]); };
    for (int y = 0; y < p.h; ++y) {
        uint32_t *row = reinterpret_cast<uint32_t *>(data + y * stride);
        const size_t o = size_t(y) * p.w;
        for (int x = 0; x < p.w; ++x)
            row[x] = 0xff000000u | (byte(p.r[o + x]) << 16) | (byte(p.g[o + x]) << 8) | byte(p.b[o + x]);
    }
    cairo_surface_mark_dirty(image.surface());
    return image;
}

inline float luminance(float r, float g, float b) { return 0.2125f * r + 0.7154f * g + 0.0721f * b; }
inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float smooth(float t)
{
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}

// ---- Core Image's colour filters

void colorControls(Planes &p, float saturation, float brightness, float contrast)
{
    for (size_t i = 0; i < p.r.size(); ++i) {
        const float l = luminance(p.r[i], p.g[i], p.b[i]);
        for (int c = 0; c < 3; ++c) {
            float &v = p.plane(c)[i];
            v = l + (v - l) * saturation;
            v = (v - 0.5f) * contrast + 0.5f + brightness;
        }
    }
}

void colorMonochrome(Planes &p, const float color[3], float intensity)
{
    for (size_t i = 0; i < p.r.size(); ++i) {
        const float l = clamp01(luminance(p.r[i], p.g[i], p.b[i]));
        for (int c = 0; c < 3; ++c) {
            float &v = p.plane(c)[i];
            const float m = l < 0.5f ? 2 * l * color[c] : 1 - 2 * (1 - l) * (1 - color[c]);
            v += (m - v) * intensity;
        }
    }
}

void colorInvert(Planes &p)
{
    for (int c = 0; c < 3; ++c)
        for (float &v : p.plane(c))
            v = 1 - v;
}

void colorPosterize(Planes &p, float levels)
{
    for (int c = 0; c < 3; ++c)
        for (float &v : p.plane(c))
            v = clamp01(std::floor(clamp01(v) * levels) / (levels - 1));
}

void sepiaTone(Planes &p)
{
    for (size_t i = 0; i < p.r.size(); ++i) {
        const float r = p.r[i], g = p.g[i], b = p.b[i];
        p.r[i] = 0.393f * r + 0.769f * g + 0.189f * b;
        p.g[i] = 0.349f * r + 0.686f * g + 0.168f * b;
        p.b[i] = 0.272f * r + 0.534f * g + 0.131f * b;
    }
}

// One pass of a box blur along rows or columns, the edge pixels repeated
void boxPass(const std::vector<float> &in, std::vector<float> &out, int w, int h, int radius, bool horizontal)
{
    const int n = horizontal ? w : h, lines = horizontal ? h : w, step = horizontal ? 1 : w;
    const float norm = 1.0f / (2 * radius + 1);
    for (int line = 0; line < lines; ++line) {
        const size_t base = horizontal ? size_t(line) * w : size_t(line);
        auto at = [&](int i) { return in[base + size_t(std::clamp(i, 0, n - 1)) * step]; };
        float sum = 0;
        for (int i = -radius; i <= radius; ++i)
            sum += at(i);
        for (int i = 0; i < n; ++i) {
            out[base + size_t(i) * step] = sum * norm;
            sum += at(i + radius + 1) - at(i - radius);
        }
    }
}

// CIGaussianBlur: three box passes each way come within a few percent of the Gaussian
void gaussianBlur(Planes &p, float sigma)
{
    if (sigma < 0.3f)
        return;
    const int radius = std::max(1, int(std::lround(std::sqrt(sigma * sigma * 12 / 3 + 1) / 2 - 0.5)));
    std::vector<float> tmp(p.r.size());
    for (int c = 0; c < 3; ++c) {
        std::vector<float> &plane = p.plane(c);
        for (int pass = 0; pass < 3; ++pass) {
            boxPass(plane, tmp, p.w, p.h, radius, true);
            boxPass(tmp, plane, p.w, p.h, radius, false);
        }
    }
}

// CIEdges: each channel's gradient across the two diagonals, squared, times the intensity (squared:
// what Comic Book's threshold of 0.5 on an intensity of 20 asks for; the filter's kernel is not read)
Planes edges(const Planes &p, float intensity)
{
    Planes out(p.w, p.h);
    for (int c = 0; c < 3; ++c) {
        const std::vector<float> &in = p.plane(c);
        std::vector<float> &o = out.plane(c);
        for (int y = 0; y < p.h; ++y) {
            const int y1 = std::min(y + 1, p.h - 1);
            for (int x = 0; x < p.w; ++x) {
                const int x1 = std::min(x + 1, p.w - 1);
                const float a = in[size_t(y) * p.w + x] - in[size_t(y1) * p.w + x1];
                const float b = in[size_t(y) * p.w + x1] - in[size_t(y1) * p.w + x];
                o[size_t(y) * p.w + x] = (a * a + b * b) * intensity;
            }
        }
    }
    return out;
}

// CICMYKHalftone: four screens of dots, each ink's dot as large as the ink is dense
void cmykHalftone(Planes &p, float width, float sharpness, float ucr)
{
    static const float degrees[4] = {75, 15, 0, 45};   // cyan, magenta, yellow, black
    float cs[4], sn[4];
    for (int i = 0; i < 4; ++i) {
        cs[i] = std::cos(degrees[i] * float(M_PI) / 180);
        sn[i] = std::sin(degrees[i] * float(M_PI) / 180);
    }
    const float edge = width / (0.5f * (1 - sharpness) * width + 0.5f);
    for (int y = 0; y < p.h; ++y) {
        for (int x = 0; x < p.w; ++x) {
            const size_t i = size_t(y) * p.w + x;
            float ink[4] = {1 - clamp01(p.r[i]), 1 - clamp01(p.g[i]), 1 - clamp01(p.b[i]), 0};
            ink[3] = std::min({ink[0], ink[1], ink[2]}) * ucr;   // under colour removal
            for (int c = 0; c < 3; ++c)
                ink[c] -= ink[3];
            float cover[4];
            const float px = x + 0.5f - 150, py = y + 0.5f - 150;   // inputCenter
            for (int c = 0; c < 4; ++c) {
                const float u = (px * cs[c] + py * sn[c]) / width, v = (-px * sn[c] + py * cs[c]) / width;
                const float du = u - std::round(u), dv = v - std::round(v);
                const float dot = std::sqrt(ink[c] / float(M_PI));
                cover[c] = clamp01((dot - std::sqrt(du * du + dv * dv)) * edge + 0.5f);
            }
            p.r[i] = (1 - cover[0]) * (1 - cover[3]);
            p.g[i] = (1 - cover[1]) * (1 - cover[3]);
            p.b[i] = (1 - cover[2]) * (1 - cover[3]);
        }
    }
}

// ---- sampling, as the distortions' compositions do it: through CIMirrorEdge
//   dC = 1 - abs(2 * fract(.5 * dC / sZ) - 1);  i = dC * (sZ - 1) + .5

inline float mirrorEdge(float v, float size)
{
    float t = 0.5f * v / size;
    t -= std::floor(t);
    return (1 - std::fabs(2 * t - 1)) * (size - 1);
}

inline void sample(const Planes &p, float x, float y, float &r, float &g, float &b)
{
    x = mirrorEdge(x, float(p.w));
    y = mirrorEdge(y, float(p.h));
    const int x0 = std::min(int(x), p.w - 1), y0 = std::min(int(y), p.h - 1);
    const int x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
    const float fx = x - x0, fy = y - y0;
    const size_t a = size_t(y0) * p.w + x0, bq = size_t(y0) * p.w + x1, c = size_t(y1) * p.w + x0, d = size_t(y1) * p.w + x1;
    const float wa = (1 - fx) * (1 - fy), wb = fx * (1 - fy), wc = (1 - fx) * fy, wd = fx * fy;
    r = p.r[a] * wa + p.r[bq] * wb + p.r[c] * wc + p.r[d] * wd;
    g = p.g[a] * wa + p.g[bq] * wb + p.g[c] * wc + p.g[d] * wd;
    b = p.b[a] * wa + p.b[bq] * wb + p.b[c] * wc + p.b[d] * wd;
}

// A distortion: where each destination point (Core Image's coordinates: pixel centres, y upwards)
// takes its colour from
template <typename Warp> Planes distort(const Planes &src, Warp warp)
{
    Planes out(src.w, src.h);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            float sx = x + 0.5f, sy = src.h - (y + 0.5f);
            warp(sx, sy);
            const size_t i = size_t(y) * src.w + x;
            sample(src, sx - 0.5f, src.h - sy - 0.5f, out.r[i], out.g[i], out.b[i]);
        }
    }
    return out;
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

Planes halfSize(const Planes &src)
{
    Planes out(src.w / 2, src.h / 2);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const size_t i = size_t(y) * out.w + x, a = size_t(y * 2) * src.w + x * 2, b = a + src.w;
            out.r[i] = (src.r[a] + src.r[a + 1] + src.r[b] + src.r[b + 1]) / 4;
            out.g[i] = (src.g[a] + src.g[a + 1] + src.g[b] + src.g[b + 1]) / 4;
            out.b[i] = (src.b[a] + src.b[a + 1] + src.b[b] + src.b[b + 1]) / 4;
        }
    return out;
}

// Pop Art's kernel, spotColor: three spot colours taken out of the picture, each replaced, over white
struct Spot {
    float replacement1[3], replacement2[3], replacement3[3];
};
void spotColor(const Planes &src, Planes &dst, int dx, int dy, const Spot &s)
{
    static const float center1[3] = {0.4153f, 0.3831f, 0.3508f}, center2[3] = {1, 1, 1}, center3[3] = {0, 0.2863f, 0};
    const float closeness[3] = {0.8504f, 1.0f, 0.25f}, contrast[3] = {0.5826f, 20.0f, 10.0f};
    const float *centers[3] = {center1, center2, center3};
    const float *replacements[3] = {s.replacement1, s.replacement2, s.replacement3};
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const size_t i = size_t(y) * src.w + x;
            const float pix[3] = {src.r[i], src.g[i], src.b[i]};
            float out[3] = {1, 1, 1};
            for (int k = 0; k < 3; ++k) {
                float d = 0;
                for (int c = 0; c < 3; ++c)
                    d += (pix[c] - linear(centers[k][c])) * (pix[c] - linear(centers[k][c]));
                const float alpha = 1 - clamp01((std::sqrt(d) - closeness[k]) * contrast[k] + 0.5f);
                for (int c = 0; c < 3; ++c)
                    out[c] = linear(replacements[k][c]) * alpha + (1 - alpha) * out[c];
            }
            const size_t o = size_t(y + dy) * dst.w + x + dx;
            dst.r[o] = out[0];
            dst.g[o] = out[1];
            dst.b[o] = out[2];
        }
    }
}

} // namespace

Effects::Map Effects::loadMap(const char *file)
{
    Map map;
    const Image image = Image::load(Assets::path(std::string("/System/Library/Compositions/") + file));
    if (image.isNull())
        return map;
    const Planes p = fromImage(image);
    const size_t row = size_t(p.h / 2) * p.w;
    map.r.assign(p.r.begin() + row, p.r.begin() + row + p.w);
    map.g.assign(p.g.begin() + row, p.g.begin() + row + p.w);
    map.b.assign(p.b.begin() + row, p.b.begin() + row + p.w);
    return map;
}

Effects::Effects()
{
    // extract.py's composition_images takes these out of the compositions' Image Loader patches
    m_thermal = loadMap("ThermalCamera.qtz.ImageLoader_1.png");
    m_xray = loadMap("XRay.qtz.ImageLoader_1.png");
    // Color Pencil: the paper through CIAffineTile, then CIMotionBlur (radius 7.1429, angle 45): the
    // mean along the diagonal, the paper repeating
    const Image paper = Image::load(Assets::path("/System/Library/Compositions/ColorPencil.qtz.ImageLoader_1.tiff"));
    if (!paper.isNull()) {
        const Planes p = fromImage(paper);
        m_paperW = p.w;
        m_paperH = p.h;
        m_paper.resize(p.r.size());
        const int radius = 7;
        for (int y = 0; y < p.h; ++y)
            for (int x = 0; x < p.w; ++x) {
                float sum = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int step = int(std::lround(k * 0.7071));
                    const int sx = ((x + step) % p.w + p.w) % p.w, sy = ((y - step) % p.h + p.h) % p.h;
                    sum += p.g[size_t(sy) * p.w + sx];
                }
                m_paper[size_t(y) * p.w + x] = sum / (2 * radius + 1);
            }
    }
}

Image Effects::apply(int effect, const Image &source, Point xy, double amountIn) const
{
    if (source.isNull() || effect < 0 || effect >= EffectCount)
        return source;
    Planes p = fromImage(source);
    const float w = float(p.w), h = float(p.h), diagonal = std::sqrt(w * w + h * h);
    const float amount = float(amountIn);
    const float cx = float(xy.x) * w, cy = float(xy.y) * h;   // Math patches: X * width, Y * height
    auto mapColors = [&p](const Map &map) {
        if (map.r.empty())
            return;
        const float last = float(map.r.size() - 1);
        for (size_t i = 0; i < p.r.size(); ++i) {
            const float t = clamp01(luminance(p.r[i], p.g[i], p.b[i])) * last;
            const size_t a = size_t(t), b = std::min(a + 1, map.r.size() - 1);
            const float f = t - a;
            p.r[i] = lerp(map.r[a], map.r[b], f);
            p.g[i] = lerp(map.g[a], map.g[b], f);
            p.b[i] = lerp(map.b[a], map.b[b], f);
        }
    };
    switch (effect) {
    case 0:   // Sepia.qtz: CISepiaTone, intensity 1
        sepiaTone(p);
        break;
    case 1: {   // BlackAndWhite.qtz: CIColorMonochrome, colour 0.6 grey, intensity 1
        const float grey[3] = {linear(0.6f), linear(0.6f), linear(0.6f)};
        colorMonochrome(p, grey, 1);
        break;
    }
    case 2: {   // Glow.qtz: CIGloom, CIColorControls, CIBloom, their radii the diagonal's 0.03 and 0.0485 times the amount
        Planes blurred = p;
        gaussianBlur(blurred, 0.03f * diagonal * amount);
        for (int c = 0; c < 3; ++c)   // CIGloom, intensity 1: the highlights dulled by their surroundings
            for (size_t i = 0; i < p.r.size(); ++i)
                p.plane(c)[i] *= blurred.plane(c)[i];
        colorControls(p, 1, 0.4444f * amount, (3.893f - 1) * amount + 1);
        blurred = p;
        gaussianBlur(blurred, 0.0485f * diagonal * amount);
        for (int c = 0; c < 3; ++c)   // CIBloom, intensity 0.5794: the picture's glow screened over it
            for (size_t i = 0; i < p.r.size(); ++i) {
                float &v = p.plane(c)[i];
                v = clamp01(v);
                v += 0.5794f * clamp01(blurred.plane(c)[i]) * (1 - v);
            }
        break;
    }
    case 3: {   // ComicBook.qtz
        // the lines: CIEdges (20), then the bw kernel: v = r + g + b; step(v, threshold 0.5)
        const Planes lines = edges(p, 20);
        // the colours: CIColorControls (1, brightness 1, contrast 3.8512), CIColorPosterize (3),
        // CIColorControls (saturation 0.7), CICMYKHalftone (width 0.0047 of the diagonal, sharpness 0.9, UCR 0.5)
        colorControls(p, 1, 1, 3.8512f);
        colorPosterize(p, 3);
        colorControls(p, 0.7f, 0, 1);
        cmykHalftone(p, 0.0047f * diagonal, 0.9f, 0.5f);
        for (size_t i = 0; i < p.r.size(); ++i)   // CIMultiplyCompositing
            if (lines.r[i] + lines.g[i] + lines.b[i] > 0.5f)
                p.r[i] = p.g[i] = p.b[i] = 0;
        break;
    }
    case 4: {   // ColorPencil.qtz
        // the pencil: CIEdges (2), CIColorInvert, CIColorMonochrome (0.6015, 0.5857, 0.5781)
        Planes pencil = edges(p, 2);
        colorInvert(pencil);
        const float lead[3] = {linear(0.6015f), linear(0.5857f), linear(0.5781f)};
        colorMonochrome(pencil, lead, 1);
        // the colours: the picture and the paper added (CIAdditionCompositing), CIColorControls
        // (saturation 0.2063, brightness 0.25, contrast 3.0774)
        if (!m_paper.empty())
            for (int y = 0; y < p.h; ++y)
                for (int x = 0; x < p.w; ++x) {
                    const float paper = m_paper[size_t(y * m_paperH / p.h) * m_paperW + x * m_paperW / p.w];
                    const size_t i = size_t(y) * p.w + x;
                    p.r[i] += paper;
                    p.g[i] += paper;
                    p.b[i] += paper;
                }
        colorControls(p, 0.2063f, 0.25f, 3.0774f);
        for (int c = 0; c < 3; ++c)   // CIMinimumCompositing
            for (size_t i = 0; i < p.r.size(); ++i)
                p.plane(c)[i] = std::min(p.plane(c)[i], pencil.plane(c)[i]);
        break;
    }
    case 5:   // ThermalCamera.qtz: CIColorMap through its gradient
        mapColors(m_thermal);
        break;
    case 6: {   // XRay.qtz: the negative, its blur (CIGaussianBlur 5) at half alpha over it
                // (CIColorMatrix, alpha 0.5), CIColorControls (0, -0.25, 1.5), CIColorMap
        colorInvert(p);
        Planes blurred = p;
        gaussianBlur(blurred, 5);
        for (int c = 0; c < 3; ++c)
            for (size_t i = 0; i < p.r.size(); ++i)
                p.plane(c)[i] = 0.5f * (p.plane(c)[i] + blurred.plane(c)[i]);
        colorControls(p, 0, -0.25f, 1.5f);
        mapColors(m_xray);
        break;
    }
    case 7: {   // PopArt.qtz: the picture at half size four times, each through spotColor
        static const Spot spots[4] = {
            {{0.993f, 0.549f, 0.919f}, {0.946f, 0.999f, 0.729f}, {0.237f, 0.638f, 0.080f}},
            {{0.323f, 0.577f, 0.388f}, {0.999f, 0.869f, 0.000f}, {0.993f, 0.112f, 0.000f}},
            {{0.856f, 0.175f, 0.152f}, {0.620f, 0.806f, 0.889f}, {0.100f, 0.118f, 0.195f}},
            {{0.986f, 0.387f, 0.064f}, {0.838f, 0.997f, 0.423f}, {0.097f, 0.149f, 0.994f}},
        };
        const Planes half = halfSize(p);
        Planes out(half.w * 2, half.h * 2);
        for (int q = 0; q < 4; ++q)
            spotColor(half, out, (q % 2) * half.w, (q / 2) * half.h, spots[q]);
        if (out.w == p.w && out.h == p.h)
            p = std::move(out);
        break;
    }
    case 8: {   // Bulge.qtz: CIBumpDistortion, radius from 1 to 0.75 of the diagonal, scale 0.5
        const float radius = lerp(1, 0.75f * diagonal, amount), scale = 0.5f;
        p = distort(p, [=](float &x, float &y) {
            const float dx = x - cx, dy = y - cy, f = 1 - scale * smooth(1 - std::sqrt(dx * dx + dy * dy) / radius);
            x = cx + dx * f;
            y = cy + dy * f;
        });
        break;
    }
    case 9: {   // Dent.qtz: CIBumpDistortion, radius to 0.425 of the diagonal, scale -2 times the amount
        const float radius = lerp(1, 0.425f * diagonal, amount), scale = -2 * amount;
        p = distort(p, [=](float &x, float &y) {
            const float dx = x - cx, dy = y - cy, f = 1 - scale * smooth(1 - std::sqrt(dx * dx + dy * dy) / radius);
            x = cx + dx * f;
            y = cy + dy * f;
        });
        break;
    }
    case 10: {   // Twirl.qtz: CITwirlDistortion, radius to 0.35 of the diagonal, angle 180 degrees times the amount
        const float radius = lerp(1, 0.35f * diagonal, amount), angle = float(M_PI) * amount;
        p = distort(p, [=](float &x, float &y) {
            const float dx = x - cx, dy = y - cy, d = std::sqrt(dx * dx + dy * dy);
            if (d >= radius)
                return;
            const float t = 1 - d / radius, a = angle * t * t, c = std::cos(a), s = std::sin(a);
            x = cx + dx * c - dy * s;
            y = cy + dx * s + dy * c;
        });
        break;
    }
    case 11: {   // Squeeze.qtz: CIPinchDistortion, radius to 0.215 of the diagonal, scale to 1.4127
        const float radius = lerp(1, 0.215f * diagonal, amount), scale = lerp(0, 1.4127f, amount);
        p = distort(p, [=](float &x, float &y) {
            const float dx = x - cx, dy = y - cy, d = std::sqrt(dx * dx + dy * dy);
            if (d >= radius)
                return;
            const float f = 1 + (1 - d / radius) * scale;
            x = cx + dx * f;
            y = cy + dy * f;
        });
        break;
    }
    case 12: {   // Mirror.qtz: CIKaleidoscope, count 1, angle 90 degrees, about the middle
        const float mx = 0.5f * w;
        p = distort(p, [=](float &x, float &) { x = mx - std::fabs(x - mx); });
        break;
    }
    case 13: {   // LightTunnel.qtz: CICircleSplashDistortion, radius to 0.336 of the diagonal
        const float radius = lerp(0, 0.336f * diagonal, amount);
        p = distort(p, [=](float &x, float &y) {
            const float dx = x - cx, dy = y - cy, d = std::sqrt(dx * dx + dy * dy);
            if (d <= radius)
                return;
            x = cx + dx * radius / d;
            y = cy + dy * radius / d;
        });
        break;
    }
    case 14: {   // FishEye.qtz's kernel: d = 0.6 * d / (1.0 - (d * d) / (factor * factor))
        const float factor = lerp(1.69f * diagonal, 0, 0.5f), mx = 0.5f * w, my = 0.5f * h;
        p = distort(p, [=](float &x, float &y) {
            const float dx = x - mx, dy = y - my, d = std::sqrt(dx * dx + dy * dy);
            if (d <= 0)
                return;
            const float k = 0.6f / std::max(1e-3f, 1 - d * d / (factor * factor));
            x = mx + dx * k;
            y = my + dy * k;
        });
        break;
    }
    case 15: {   // Stretch.qtz's kernel, funHouse:
        //   g = 1.0 - clamp(abs(p - center) / param1, 0.0, 1.0); g = smoothstep(0.0, 1.0, g);
        //   f = param2 * sin_((p - center.y) / param3); p -= f * g;
        // centre 0.6674 of the width, half the height; the parameters 0.84, 0.18 and 0.234 of the diagonal at 0.5
        const float mx = 0.6674f * w, my = 0.5f * h;
        const float p1 = lerp(0, 0.84f * diagonal, 0.5f), p2 = lerp(0, 0.18f * diagonal, 0.5f), p3 = lerp(0, 0.234f * diagonal, 0.5f);
        p = distort(p, [=](float &x, float &y) {
            const float gx = smooth(1 - std::fabs(x - mx) / p1), gy = smooth(1 - std::fabs(y - my) / p1);
            const float fx = p2 * std::sin((x - my) / p3), fy = p2 * std::sin((y - my) / p3);
            x -= fx * gx;
            y -= fy * gy;
        });
        break;
    }
    }
    return toImage(p);
}

} // namespace PB
