// SPDX-License-Identifier: LGPL-2.1-or-later
#include "menubarfilter.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

using namespace AK;

namespace {

struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;   // r, g, b per pixel
    float *at(int x, int y) { return v.data() + 3 * (y * w + x); }
    const float *at(int x, int y) const { return v.data() + 3 * (y * w + x); }
};

// a separable Gaussian, sigma = radius as CIGaussianBlur takes it; edges clamped
Plane blur(const Plane &in, float sigma)
{
    const int r = int(std::ceil(3 * sigma));
    std::vector<float> k(size_t(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; ++i)
        sum += k[size_t(i + r)] = std::exp(-(i * i) / (2 * sigma * sigma));
    for (float &f : k)
        f /= sum;
    Plane tmp = in, out = in;
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int i = -r; i <= r; ++i)
                    s += k[size_t(i + r)] * in.v[size_t(3 * (y * in.w + std::clamp(x + i, 0, in.w - 1)) + c)];
                tmp.v[size_t(3 * (y * in.w + x) + c)] = s;
            }
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int i = -r; i <= r; ++i)
                    s += k[size_t(i + r)] * tmp.v[size_t(3 * (std::clamp(y + i, 0, in.h - 1) * in.w + x) + c)];
                out.v[size_t(3 * (y * in.w + x) + c)] = s;
            }
    return out;
}

// an image's pixels as floats (opaque: its RGB)
Plane plane(const Image &img)
{
    Plane p;
    if (img.isNull())
        return p;
    cairo_surface_t *s = img.surface();
    cairo_surface_flush(s);
    p.w = img.width();
    p.h = img.height();
    p.v.resize(size_t(3 * p.w * p.h));
    const unsigned char *data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < p.h; ++y) {
        const uint32_t *line = reinterpret_cast<const uint32_t *>(data + y * stride);
        for (int x = 0; x < p.w; ++x) {
            float *q = p.at(x, y);
            q[0] = ((line[x] >> 16) & 0xff) / 255.f, q[1] = ((line[x] >> 8) & 0xff) / 255.f, q[2] = (line[x] & 0xff) / 255.f;
        }
    }
    return p;
}

float mix(float a, float b, float t) { return a * (1 - t) + b * t; }

} // namespace

Image generateMenuBar(const Image &behind)
{
    const Plane desk = plane(behind);
    if (desk.w == 0)
        return {};
    double mean = 0;
    for (float f : desk.v)
        mean += f;
    const float L = float(mean / double(desk.v.size()));
    const float white = mix(0.60f, 0.25f, L), lift = mix(0.35f, 0.15f, L);
    const Plane b = blur(desk, 1.6f);
    Image out(desk.w, desk.h);
    cairo_surface_t *s = out.surface();
    cairo_surface_flush(s);
    unsigned char *data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    const float h = float(desk.h);
    for (int row = 0; row < desk.h; ++row) {
        const float y = row + 0.5f;
        const float ramp = std::pow(std::max(0.f, 1 - y / h), 0.75f);
        const float top = std::clamp(1.5f - y, 0.f, 1.f) * 0.5f;
        const float edge = std::clamp(y - h + 1.5f, 0.f, 1.f);
        uint32_t *line = reinterpret_cast<uint32_t *>(data + row * stride);
        for (int x = 0; x < desk.w; ++x) {
            const float *p = b.at(x, row);
            uint32_t px = 0xff000000u;
            for (int c = 0; c < 3; ++c) {
                float v = (p[c] * (1 - white) + white) / (1 - lift * ramp);
                v = v * (1 - top) + top;
                v = mix(v, p[c] * 0.59f, edge);
                px |= uint32_t(std::clamp(int(std::lround(v * 255)), 0, 255)) << (16 - 8 * c);
            }
            line[x] = px;
        }
    }
    cairo_surface_mark_dirty(s);
    return out;
}

bool drawMenuApple(Image &bar, const Image &behind)
{
    // RGBA, premultiplied, the top row first (QuartzCore's _imageApple)
    static const std::vector<unsigned char> apple = [] {
        std::ifstream f(Assets::path("/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/MenuApple-32x22.rgba"),
                        std::ios::binary);
        std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
        return b.size() == 32 * 22 * 4 ? b : std::vector<unsigned char>();
    }();
    const int X = 17, W = 32, H = 22;
    if (apple.empty() || bar.height() < H)
        return false;
    auto alphaAt = [&](int x, int y) { return apple[size_t((y * W + x) * 4 + 3)] / 255.f; };

    const Plane desk = plane(behind);
    double mean = 0;
    int count = 0;
    for (int y = 0; y < desk.h; ++y)
        for (int x = X; x < desk.w; ++x)
            for (int c = 0; c < 3; ++c)
                mean += desk.at(x, y)[c], ++count;
    const float L = count ? float(mean / count) : 0.5f;
    const int pad = 24, x0 = std::max(0, X - pad), x1 = std::min(desk.w, X + W + pad);
    Plane local;
    local.w = x1 - x0;
    local.h = desk.h;
    local.v.resize(size_t(3 * local.w * local.h));
    for (int y = 0; y < local.h; ++y)
        for (int x = 0; x < local.w; ++x)
            std::copy_n(desk.at(x0 + x, y), 3, local.at(x, y));
    const Plane blurred = blur(local, 8.f);
    Plane alpha;
    alpha.w = W, alpha.h = H;
    alpha.v.resize(size_t(3 * W * H));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float *p = alpha.at(x, y);
            p[0] = p[1] = p[2] = alphaAt(x, y);
        }
    const Plane shadowAlpha = blur(alpha, 1.5f);
    const float black = mix(0.30f, 0.075f, L), lift = mix(0.75f, 0.5f, L), shine = mix(0.6f, 0.85f, L);

    Image glass(W, H), bevel(W, H);
    for (Image *img : {&glass, &bevel})
        cairo_surface_flush(img->surface());
    unsigned char *g = cairo_image_surface_get_data(glass.surface()), *bv = cairo_image_surface_get_data(bevel.surface());
    const int gs = cairo_image_surface_get_stride(glass.surface()), bs = cairo_image_surface_get_stride(bevel.surface());
    for (int y = 0; y < H; ++y) {
        const float py = y + 0.5f, t = py / float(H);
        const float divisor = 1 - lift * mix(0.5f, 1.f, std::pow(t, 0.75f));
        for (int x = 0; x < W; ++x) {
            const float px = x + 0.5f, dx = px - 7.5f, dy = py - 4.f;
            const float v = dx < 0 ? dy : dy < 0 ? dx : std::sqrt(dx * dx + dy * dy);
            const float gleam = std::clamp(v - 8, 0.f, 1.f) * std::pow(t, 2.75f) * shine;
            const float shade = 1 - shadowAlpha.at(x, y)[0];
            const float a = alpha.at(x, y)[0];
            const int bx = std::clamp(X + x - x0, 0, local.w - 1);
            uint32_t px32 = uint32_t(std::lround(a * 255)) << 24;
            for (int c = 0; c < 3; ++c) {
                float f = blurred.at(bx, std::min(y, local.h - 1))[c] * black / divisor + gleam;
                f *= 1 - shade;
                px32 |= uint32_t(std::clamp(int(std::lround(f * a * 255)), 0, 255)) << (16 - 8 * c);
            }
            reinterpret_cast<uint32_t *>(g + y * gs)[x] = px32;
            const uint32_t b = uint32_t(std::lround(a * 255 * 0.6f));
            reinterpret_cast<uint32_t *>(bv + y * bs)[x] = b << 24 | b << 16 | b << 8 | b;
        }
    }
    cairo_surface_mark_dirty(glass.surface());
    cairo_surface_mark_dirty(bevel.surface());
    cairo_t *cr = cairo_create(bar.surface());
    bevel.draw(cr, Point{double(X), 1});
    glass.draw(cr, Point{double(X), 0});
    cairo_destroy(cr);
    return true;
}
