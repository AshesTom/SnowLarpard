// SPDX-License-Identifier: LGPL-2.1-or-later
#include "graphics.h"

#include <tiffio.h>
#include <turbojpeg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace AK {

Color Color::parse(const std::string &hex)
{
    unsigned r = 0, g = 0, b = 0;
    if (hex.size() == 7 && hex[0] == '#' && std::sscanf(hex.c_str() + 1, "%2x%2x%2x", &r, &g, &b) == 3)
        return rgb(int(r), int(g), int(b));
    return {0, 0, 0, 0};
}

std::string Color::name() const
{
    char s[8];
    std::snprintf(s, sizeof s, "#%02x%02x%02x", int(std::lround(r * 255)), int(std::lround(g * 255)), int(std::lround(b * 255)));
    return s;
}

void fill(cairo_t *cr, const Rect &r, const Color &c)
{
    c.set(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
}

Image::Image(cairo_surface_t *adopt)
{
    if (adopt && cairo_surface_status(adopt) == CAIRO_STATUS_SUCCESS)
        m_surface.reset(adopt, cairo_surface_destroy);
    else if (adopt)
        cairo_surface_destroy(adopt);
}

Image::Image(int width, int height) : Image(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, width), std::max(1, height))) {}

int Image::width() const { return m_surface ? cairo_image_surface_get_width(m_surface.get()) : 0; }
int Image::height() const { return m_surface ? cairo_image_surface_get_height(m_surface.get()) : 0; }

void Image::draw(cairo_t *cr, const Rect &dst, double alpha) const
{
    draw(cr, Rect{0, 0, double(width()), double(height())}, dst, alpha);
}

void Image::draw(cairo_t *cr, const Rect &src, const Rect &dst, double alpha) const
{
    if (isNull() || src.empty() || dst.empty())
        return;
    cairo_save(cr);
    cairo_rectangle(cr, dst.x, dst.y, dst.w, dst.h);
    cairo_clip(cr);
    cairo_translate(cr, dst.x, dst.y);
    cairo_scale(cr, dst.w / src.w, dst.h / src.h);
    cairo_set_source_surface(cr, surface(), -src.x, -src.y);
    // a stretched one-pixel piece: its edge, not a blend with the transparent outside
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    const bool scaling = std::abs(dst.w - src.w) > 0.01 || std::abs(dst.h - src.h) > 0.01;
    const bool shrinking = dst.w < src.w * 0.75 || dst.h < src.h * 0.75;
    cairo_pattern_set_filter(cairo_get_source(cr), !scaling ? CAIRO_FILTER_NEAREST : shrinking ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
    if (alpha >= 1)
        cairo_paint(cr);
    else
        cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

Image Image::scaled(int w, int h) const
{
    if (isNull())
        return {};
    Image out(w, h);
    cairo_t *cr = cairo_create(out.surface());
    draw(cr, Rect{0, 0, double(w), double(h)});
    cairo_destroy(cr);
    return out;
}

Image Image::copy(const Rect &part) const
{
    Image out(int(part.w), int(part.h));
    cairo_t *cr = cairo_create(out.surface());
    draw(cr, part, Rect{0, 0, part.w, part.h});
    cairo_destroy(cr);
    return out;
}

bool Image::savePng(const std::string &path) const
{
    return !isNull() && cairo_surface_write_to_png(surface(), path.c_str()) == CAIRO_STATUS_SUCCESS;
}

namespace {

std::vector<unsigned char> readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return in ? std::vector<unsigned char>(std::istreambuf_iterator<char>(in), {}) : std::vector<unsigned char>();
}

Image fromPngBytes(const unsigned char *data, size_t size)
{
    struct Reader {
        const unsigned char *p;
        size_t left;
    } r{data, size};
    auto read = [](void *closure, unsigned char *out, unsigned int length) {
        auto *rd = static_cast<Reader *>(closure);
        if (length > rd->left)
            return CAIRO_STATUS_READ_ERROR;
        std::memcpy(out, rd->p, length);
        rd->p += length;
        rd->left -= length;
        return CAIRO_STATUS_SUCCESS;
    };
    return Image(cairo_image_surface_create_from_png_stream(read, &r));
}

Image jpeg(const std::vector<unsigned char> &bytes, Size atLeast)
{
    tjhandle tj = tjInitDecompress();
    if (!tj)
        return {};
    int w = 0, h = 0, sub = 0, space = 0;
    Image out;
    if (tjDecompressHeader3(tj, bytes.data(), (unsigned long)bytes.size(), &w, &h, &sub, &space) == 0) {
        // the smallest of libjpeg's scales that still covers the size asked for (a 2560 px picture
        // for a 73 px thumbnail is decoded at an eighth)
        int sw = w, sh = h, count = 0;
        if (!atLeast.empty())
            if (tjscalingfactor *f = tjGetScalingFactors(&count))
                for (int i = 0; i < count; ++i) {
                    const int fw = TJSCALED(w, f[i]), fh = TJSCALED(h, f[i]);
                    if (fw >= atLeast.w && fh >= atLeast.h && fw * fh < sw * sh)
                        sw = fw, sh = fh;
                }
        out = Image(sw, sh);
        cairo_surface_flush(out.surface());
        // cairo's ARGB32 is BGRA in memory on little-endian machines; JPEGs are opaque
        if (tjDecompress2(tj, bytes.data(), (unsigned long)bytes.size(), cairo_image_surface_get_data(out.surface()), sw,
                          cairo_image_surface_get_stride(out.surface()), sh, TJPF_BGRA, TJFLAG_ACCURATEDCT) != 0 &&
            tjGetErrorCode(tj) == TJERR_FATAL)
            out = {};
        else
            cairo_surface_mark_dirty(out.surface());
    }
    tjDestroy(tj);
    return out;
}

uint32_t be32(const unsigned char *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// icns: 'icns', u32 length, then elements (type, u32 length with its header). The RGB elements
// (is32 16, il32 32, ih32 48, it32 128 after 4 zero bytes) are PackBits per channel, their masks
// (s8mk, l8mk, h8mk, t8mk) 8 bits a pixel; ic07 and later hold a PNG (or a JPEG 2000, not read).
Image icns(const std::vector<unsigned char> &b, Size atLeast)
{
    if (b.size() < 8 || std::memcmp(b.data(), "icns", 4) != 0)
        return {};
    struct Rep {
        int size;
        const unsigned char *rgb = nullptr, *mask = nullptr;
        size_t rgbLength = 0;
        const unsigned char *png = nullptr;
        size_t pngLength = 0;
    };
    std::vector<Rep> reps;
    auto rep = [&](int size) -> Rep & {
        for (Rep &r : reps)
            if (r.size == size)
                return r;
        reps.push_back({size});
        return reps.back();
    };
    for (size_t at = 8; at + 8 <= b.size();) {
        const std::string type(reinterpret_cast<const char *>(&b[at]), 4);
        const size_t length = be32(&b[at + 4]);
        if (length < 8 || at + length > b.size())
            break;
        const unsigned char *data = &b[at + 8];
        const size_t n = length - 8;
        if (type == "is32") rep(16).rgb = data, rep(16).rgbLength = n;
        else if (type == "il32") rep(32).rgb = data, rep(32).rgbLength = n;
        else if (type == "ih32") rep(48).rgb = data, rep(48).rgbLength = n;
        else if (type == "it32" && n > 4) rep(128).rgb = data + 4, rep(128).rgbLength = n - 4;
        else if (type == "s8mk") rep(16).mask = data;
        else if (type == "l8mk") rep(32).mask = data;
        else if (type == "h8mk") rep(48).mask = data;
        else if (type == "t8mk") rep(128).mask = data;
        else if (n > 8 && std::memcmp(data, "\x89PNG", 4) == 0) {
            const int size = type == "ic07" ? 128 : type == "ic08" ? 256 : type == "ic09" ? 512 : type == "ic10" ? 1024 : 0;
            if (size)
                rep(size).png = data, rep(size).pngLength = n;
        }
        at += length;
    }
    std::sort(reps.begin(), reps.end(), [](const Rep &a, const Rep &c) { return a.size < c.size; });
    const Rep *best = nullptr;
    for (const Rep &r : reps)
        if ((r.rgb && r.mask) || r.png) {
            best = &r;
            if (r.size >= atLeast.w && r.size >= atLeast.h)
                break;
        }
    if (!best)
        return {};
    if (best->png)
        return fromPngBytes(best->png, best->pngLength);
    const int s = best->size, count = s * s;
    std::vector<unsigned char> planes(size_t(count) * 3);
    const unsigned char *p = best->rgb, *end = best->rgb + best->rgbLength;
    for (int c = 0; c < 3; ++c) {
        int i = 0;
        while (i < count && p < end) {
            const unsigned char k = *p++;
            if (k < 0x80) {
                for (int j = 0; j <= k && i < count && p < end; ++j)
                    planes[size_t(c) * count + i++] = *p++;
            } else if (p < end) {
                const unsigned char v = *p++;
                for (int j = 0; j < k - 125 && i < count; ++j)
                    planes[size_t(c) * count + i++] = v;
            }
        }
    }
    Image out(s, s);
    cairo_surface_flush(out.surface());
    unsigned char *dst = cairo_image_surface_get_data(out.surface());
    const int stride = cairo_image_surface_get_stride(out.surface());
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            const int i = y * s + x;
            const unsigned a = best->mask[i];
            auto *px = reinterpret_cast<uint32_t *>(dst + y * stride) + x;
            *px = a << 24 | (planes[i] * a / 255) << 16 | (planes[count + i] * a / 255) << 8 | (planes[2 * count + i] * a / 255);
        }
    cairo_surface_mark_dirty(out.surface());
    return out;
}

// TIFF (the panes' pictures): its first page, straight RGBA to cairo's premultiplied ARGB
Image tiff(const std::string &path)
{
    TIFFSetWarningHandler(nullptr);
    TIFF *t = TIFFOpen(path.c_str(), "r");
    if (!t)
        return {};
    uint32_t w = 0, h = 0;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    Image out;
    std::vector<uint32_t> px(size_t(w) * h);
    if (w && h && w < 16384 && h < 16384 && TIFFReadRGBAImageOriented(t, w, h, px.data(), ORIENTATION_TOPLEFT, 0)) {
        out = Image(int(w), int(h));
        cairo_surface_flush(out.surface());
        unsigned char *dst = cairo_image_surface_get_data(out.surface());
        const int stride = cairo_image_surface_get_stride(out.surface());
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t v = px[size_t(y) * w + x];
                const uint32_t a = TIFFGetA(v);
                reinterpret_cast<uint32_t *>(dst + y * uint32_t(stride))[x] =
                    a << 24 | (TIFFGetR(v) * a / 255) << 16 | (TIFFGetG(v) * a / 255) << 8 | (TIFFGetB(v) * a / 255);
            }
        cairo_surface_mark_dirty(out.surface());
    }
    TIFFClose(t);
    return out;
}

} // namespace

Image Image::load(const std::string &path, Size atLeast)
{
    std::vector<unsigned char> bytes = readFile(path);
    if (bytes.size() < 8)
        return {};
    if (std::memcmp(bytes.data(), "\x89PNG", 4) == 0)
        return fromPngBytes(bytes.data(), bytes.size());
    if (bytes[0] == 0xFF && bytes[1] == 0xD8)
        return jpeg(bytes, atLeast);
    if (std::memcmp(bytes.data(), "icns", 4) == 0)
        return icns(bytes, atLeast);
    if (std::memcmp(bytes.data(), "II*", 3) == 0 || std::memcmp(bytes.data(), "MM\0*", 4) == 0)
        return tiff(path);
    return {};
}

Size Image::sizeOf(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    unsigned char head[32] = {};
    in.read(reinterpret_cast<char *>(head), sizeof head);
    if (std::memcmp(head, "\x89PNG", 4) == 0)
        return {double(be32(head + 16)), double(be32(head + 20))};
    if (head[0] == 0xFF && head[1] == 0xD8) {
        std::vector<unsigned char> bytes = readFile(path);
        tjhandle tj = tjInitDecompress();
        int w = 0, h = 0, sub = 0, space = 0;
        Size s;
        if (tj && tjDecompressHeader3(tj, bytes.data(), (unsigned long)bytes.size(), &w, &h, &sub, &space) == 0)
            s = {double(w), double(h)};
        if (tj)
            tjDestroy(tj);
        return s;
    }
    return {};
}

Size scaledToFit(Size s, Size box)
{
    if (s.empty())
        return {};
    const double k = std::min(box.w / s.w, box.h / s.h);
    return {std::round(s.w * k), std::round(s.h * k)};
}

Size scaledToFill(Size s, Size box)
{
    if (s.empty())
        return {};
    const double k = std::max(box.w / s.w, box.h / s.h);
    return {std::round(s.w * k), std::round(s.h * k)};
}

} // namespace AK
