// SPDX-License-Identifier: LGPL-2.1-or-later
#include "graphics.h"

#include "foundation.h"

#include <poppler.h>
#include <lcms2.h>
#include <tiffio.h>
#include <turbojpeg.h>
#include <zlib.h>

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

// NSBitmapImageRep's TIFFRepresentation: 8 bits a sample, RGBA with its alpha unassociated (as
// NSBitmapImageRep writes one), LZW
bool Image::saveTiff(const std::string &path) const
{
    if (isNull())
        return false;
    cairo_surface_flush(surface());
    const int w = width(), h = height(), stride = cairo_image_surface_get_stride(surface());
    const unsigned char *data = cairo_image_surface_get_data(surface());
    TIFF *t = TIFFOpen(path.c_str(), "w");
    if (!t)
        return false;
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, uint32_t(w));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, uint32_t(h));
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
    const uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, &extra);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(t, 0));
    std::vector<unsigned char> row(size_t(w) * 4);
    bool ok = true;
    for (int y = 0; y < h && ok; ++y) {
        const uint32_t *px = reinterpret_cast<const uint32_t *>(data + size_t(y) * size_t(stride));
        for (int x = 0; x < w; ++x) {   // cairo's premultiplied ARGB to straight RGBA
            const uint32_t p = px[x];
            const unsigned a = p >> 24;
            auto un = [a](unsigned c) { return a ? (unsigned char)std::min(255u, (c * 255 + a / 2) / a) : (unsigned char)0; };
            row[size_t(x) * 4] = un((p >> 16) & 255);
            row[size_t(x) * 4 + 1] = un((p >> 8) & 255);
            row[size_t(x) * 4 + 2] = un(p & 255);
            row[size_t(x) * 4 + 3] = (unsigned char)a;
        }
        ok = TIFFWriteScanline(t, row.data(), uint32_t(y), 0) >= 0;
    }
    TIFFClose(t);
    return ok;
}

namespace {

std::vector<unsigned char> readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return in ? std::vector<unsigned char>(std::istreambuf_iterator<char>(in), {}) : std::vector<unsigned char>();
}

uint32_t be32(const unsigned char *p);
Image fromPngBytesAsIs(const unsigned char *data, size_t size);

// Colour matching, as ColorSync does on 10.6 for a picture that says what it is in: one tagged with
// a profile (most of the Mac's art: Generic RGB, gamma 1.8, or a PNG gAMA of 0.55) is converted to
// the screen's (taken as sRGB), which lightens its middle tones; the alpha is kept. CoreUI's own art
// (ArtFile.bin, SArtFile.bin) is made in sRGB (CUIElement::LoadFromArtFile,
// CUISharedArtReader::CreateImageFromBytes): untouched, as untagged pictures are.
void matchToScreen(Image &img, cmsHPROFILE source)
{
    if (!source)
        return;
    char desc[256] = {};
    cmsGetProfileInfoASCII(source, cmsInfoDescription, "en", "US", desc, sizeof desc);
    if (!img.isNull() && cmsGetColorSpace(source) == cmsSigRgbData && !std::strstr(desc, "sRGB")) {
        cmsHPROFILE screen = cmsCreate_sRGBProfile();
        if (cmsHTRANSFORM t = cmsCreateTransform(source, TYPE_BGRA_8_PREMUL, screen, TYPE_BGRA_8_PREMUL, INTENT_PERCEPTUAL,
                                                 cmsFLAGS_COPY_ALPHA)) {
            cairo_surface_flush(img.surface());
            unsigned char *px = cairo_image_surface_get_data(img.surface());
            const int stride = cairo_image_surface_get_stride(img.surface());
            for (int y = 0; y < img.height(); ++y)
                cmsDoTransform(t, px + size_t(y) * stride, px + size_t(y) * stride, cmsUInt32Number(img.width()));
            cairo_surface_mark_dirty(img.surface());
            cmsDeleteTransform(t);
        }
        cmsCloseProfile(screen);
    }
    cmsCloseProfile(source);
}

// The colour space ImageIO gives an icns' raw data (_CGImagePluginInitICNS: kCGColorSpaceGenericRGB):
// the DVD's Generic RGB Profile.icc, else its values (gamma 1.8, D65, Apple's primaries)
cmsHPROFILE genericRgbProfile()
{
    std::vector<unsigned char> icc;
    {
        std::ifstream in(Assets::path("/System/Library/ColorSync/Profiles/Generic RGB Profile.icc"), std::ios::binary);
        icc.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (!icc.empty())
        if (cmsHPROFILE p = cmsOpenProfileFromMem(icc.data(), cmsUInt32Number(icc.size())))
            return p;
    const cmsCIExyY white = {0.3127, 0.3290, 1};
    const cmsCIExyYTRIPLE primaries = {{0.6300, 0.3400, 1}, {0.2950, 0.6050, 1}, {0.1550, 0.0770, 1}};
    cmsToneCurve *curve = cmsBuildGamma(nullptr, 1.8);
    cmsToneCurve *curves[3] = {curve, curve, curve};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(curve);
    return p;
}

} // namespace

Color Color::generic(int r, int g, int b, double alpha) { return rgb(r, g, b, alpha).matched(); }

Color Color::matched() const
{
    static cmsHTRANSFORM t = [] {
        cmsHPROFILE src = genericRgbProfile(), dst = cmsCreate_sRGBProfile();
        cmsHTRANSFORM x = cmsCreateTransform(src, TYPE_RGB_DBL, dst, TYPE_RGB_DBL, INTENT_PERCEPTUAL, 0);
        cmsCloseProfile(src);
        cmsCloseProfile(dst);
        return x;
    }();
    if (!t)
        return *this;
    const double in[3] = {r, g, b};
    double out[3] = {r, g, b};
    cmsDoTransform(t, in, out, 1);
    return {std::clamp(out[0], 0.0, 1.0), std::clamp(out[1], 0.0, 1.0), std::clamp(out[2], 0.0, 1.0), a};
}

namespace {

// A PNG's own colour space: iCCP (first), else gAMA with cHRM's primaries or sRGB's; none with an
// sRGB chunk or nothing
cmsHPROFILE pngProfile(const unsigned char *d, size_t size)
{
    double gamma = 0;
    double chrm[8] = {0.3127, 0.3290, 0.64, 0.33, 0.30, 0.60, 0.15, 0.06};
    for (size_t at = 8; at + 12 <= size;) {
        const uint32_t length = be32(d + at);
        const char *type = reinterpret_cast<const char *>(d + at + 4);
        const unsigned char *body = d + at + 8;
        if (at + 12 + length > size || !std::memcmp(type, "IDAT", 4))
            break;
        if (!std::memcmp(type, "sRGB", 4))
            return nullptr;
        if (!std::memcmp(type, "iCCP", 4)) {
            const unsigned char *zero = static_cast<const unsigned char *>(std::memchr(body, 0, length));
            if (zero && zero + 2 <= body + length) {
                std::vector<unsigned char> icc(1 << 16);
                for (;;) {
                    uLongf out = icc.size();
                    const int r = uncompress(icc.data(), &out, zero + 2, uLong(body + length - zero - 2));
                    if (r == Z_BUF_ERROR && icc.size() < (1u << 24)) {
                        icc.resize(icc.size() * 4);
                        continue;
                    }
                    if (r == Z_OK)
                        return cmsOpenProfileFromMem(icc.data(), cmsUInt32Number(out));
                    break;
                }
            }
        } else if (!std::memcmp(type, "gAMA", 4) && length == 4) {
            gamma = be32(body) / 100000.0;
        } else if (!std::memcmp(type, "cHRM", 4) && length == 32) {
            for (int i = 0; i < 8; ++i)
                chrm[i] = be32(body + 4 * i) / 100000.0;
        }
        at += 12 + length;
    }
    if (gamma <= 0 || std::abs(gamma - 1 / 2.2) < 0.01)
        return nullptr;
    const cmsCIExyY white = {chrm[0], chrm[1], 1};
    const cmsCIExyYTRIPLE primaries = {{chrm[2], chrm[3], 1}, {chrm[4], chrm[5], 1}, {chrm[6], chrm[7], 1}};
    cmsToneCurve *curve = cmsBuildGamma(nullptr, 1 / gamma);
    cmsToneCurve *curves[3] = {curve, curve, curve};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(curve);
    return p;
}

// A JPEG's ICC profile (APP2 "ICC_PROFILE" segments, in their order)
cmsHPROFILE jpegProfile(const std::vector<unsigned char> &b)
{
    std::vector<std::pair<int, std::vector<unsigned char>>> parts;
    for (size_t at = 2; at + 4 <= b.size() && b[at] == 0xFF;) {
        const unsigned char marker = b[at + 1];
        if (marker == 0xDA || marker == 0xD9)
            break;
        const size_t length = size_t(b[at + 2]) << 8 | b[at + 3];
        if (marker == 0xE2 && length > 16 && at + 2 + length <= b.size() && !std::memcmp(&b[at + 4], "ICC_PROFILE", 12))
            parts.push_back({b[at + 16], std::vector<unsigned char>(b.begin() + long(at + 18), b.begin() + long(at + 2 + length))});
        at += 2 + length;
    }
    if (parts.empty())
        return nullptr;
    std::sort(parts.begin(), parts.end(), [](const auto &a, const auto &c) { return a.first < c.first; });
    std::vector<unsigned char> icc;
    for (const auto &p : parts)
        icc.insert(icc.end(), p.second.begin(), p.second.end());
    return cmsOpenProfileFromMem(icc.data(), cmsUInt32Number(icc.size()));
}

Image fromPngBytes(const unsigned char *data, size_t size)
{
    Image img = fromPngBytesAsIs(data, size);
    matchToScreen(img, pngProfile(data, size));
    return img;
}

Image fromPngBytesAsIs(const unsigned char *data, size_t size)
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
    matchToScreen(out, jpegProfile(bytes));
    return out;
}

uint32_t be32(const unsigned char *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// The classic 16 x 16 icons of an icns without RGB ones (the keyboard layouts' older flags): ics8 in
// the Mac's 256-colour system palette (clut 8: the 6 x 6 x 6 cube from white without black, then
// ramps of red, green, blue and grey, black last), ics4 in its 16 colours, ics# one bit a pixel then
// its mask; the mask s8mk's when there is one, else ics#'s
Image classicIcon(const unsigned char *mono, const unsigned char *four, const unsigned char *eight, const unsigned char *alpha)
{
    if (!mono && !four && !eight)
        return {};
    static const uint32_t clut4[16] = {0xFFFFFF, 0xFCF305, 0xFF6402, 0xDD0806, 0xF20884, 0x4600A5, 0x0000D4, 0x02ABEA,
                                       0x1FB714, 0x006411, 0x562C05, 0x90713A, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
    auto clut8 = [](int i) -> uint32_t {
        if (i < 215) {
            const int r = i / 36, g = i / 6 % 6, b = i % 6;
            return uint32_t(0xFF - 0x33 * r) << 16 | uint32_t(0xFF - 0x33 * g) << 8 | uint32_t(0xFF - 0x33 * b);
        }
        if (i == 255)
            return 0;
        static const int ramp[10] = {0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
        const int v = ramp[(i - 215) % 10];
        switch ((i - 215) / 10) {
        case 0: return uint32_t(v) << 16;
        case 1: return uint32_t(v) << 8;
        case 2: return uint32_t(v);
        default: return uint32_t(v) * 0x010101;
        }
    };
    Image out(16, 16);
    cairo_surface_flush(out.surface());
    unsigned char *dst = cairo_image_surface_get_data(out.surface());
    const int stride = cairo_image_surface_get_stride(out.surface());
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int i = y * 16 + x;
            uint32_t rgb = 0;
            if (eight)
                rgb = clut8(eight[i]);
            else if (four)
                rgb = clut4[(four[i / 2] >> (i % 2 ? 0 : 4)) & 0xF];
            else
                rgb = (mono[i / 8] >> (7 - i % 8)) & 1 ? 0 : 0xFFFFFF;
            unsigned a = 255;
            if (alpha)
                a = alpha[i];
            else if (mono)
                a = (mono[32 + i / 8] >> (7 - i % 8)) & 1 ? 255 : 0;
            auto *px = reinterpret_cast<uint32_t *>(dst + y * stride) + x;
            *px = a << 24 | (((rgb >> 16) & 0xFF) * a / 255) << 16 | (((rgb >> 8) & 0xFF) * a / 255) << 8 | ((rgb & 0xFF) * a / 255);
        }
    cairo_surface_mark_dirty(out.surface());
    return out;
}

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
    const unsigned char *icsMono = nullptr, *ics4 = nullptr, *ics8 = nullptr, *mask16 = nullptr;
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
        else if (type == "s8mk") rep(16).mask = data, mask16 = data;
        else if (type == "ics#" && n >= 64) icsMono = data;
        else if (type == "ics4" && n >= 128) ics4 = data;
        else if (type == "ics8" && n >= 256) ics8 = data;
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
        return classicIcon(icsMono, ics4, ics8, mask16);
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
    matchToScreen(out, genericRgbProfile());
    return out;
}

// TIFF (the panes' pictures): its first page, straight RGBA to cairo's premultiplied ARGB
Image tiff(const std::string &path)
{
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);   // a TIFF the RGBA reader refuses is read below
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
    } else if (w && h && w < 16384 && h < 16384) {
        // NeXT's own TIFFs (Grab's camera, 1995): 1, 2 or 4 bits a sample, RGB or grey, with an
        // associated alpha, in one plane, which libtiff's RGBA reader refuses: unpacked here
        uint16_t bps = 0, spp = 0, photometric = 0, planar = PLANARCONFIG_CONTIG;
        TIFFGetField(t, TIFFTAG_BITSPERSAMPLE, &bps);
        TIFFGetField(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &photometric);
        TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
        const bool rgb = photometric == PHOTOMETRIC_RGB, grey = photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE;
        const int colours = rgb ? 3 : 1;
        if ((bps == 1 || bps == 2 || bps == 4) && planar == PLANARCONFIG_CONTIG && (rgb || grey) && spp >= colours && spp <= colours + 1) {
            const bool alpha = spp == colours + 1;
            const unsigned max = (1u << bps) - 1;
            std::vector<unsigned char> line(size_t(TIFFScanlineSize(t)));
            out = Image(int(w), int(h));
            cairo_surface_flush(out.surface());
            unsigned char *dst = cairo_image_surface_get_data(out.surface());
            const int stride = cairo_image_surface_get_stride(out.surface());
            for (uint32_t y = 0; y < h; ++y) {
                if (TIFFReadScanline(t, line.data(), y, 0) < 0)
                    break;
                size_t bit = 0;
                auto sample = [&] {
                    const unsigned v = (line[bit / 8] >> (8 - bps - bit % 8)) & max;
                    bit += bps;
                    return v * 255 / max;
                };
                for (uint32_t x = 0; x < w; ++x) {
                    unsigned c[3];
                    for (int i = 0; i < colours; ++i)
                        c[i] = sample();
                    if (grey) {
                        if (photometric == PHOTOMETRIC_MINISWHITE)
                            c[0] = 255 - c[0];
                        c[1] = c[2] = c[0];
                    }
                    const unsigned a = alpha ? sample() : 255;   // associated: the colours already by it
                    reinterpret_cast<uint32_t *>(dst + y * uint32_t(stride))[x] =
                        a << 24 | std::min(c[0], a) << 16 | std::min(c[1], a) << 8 | std::min(c[2], a);
                }
            }
            cairo_surface_mark_dirty(out.surface());
        }
    }
    uint32_t iccLength = 0;
    void *icc = nullptr;
    cmsHPROFILE profile = nullptr;
    if (!out.isNull() && TIFFGetField(t, TIFFTAG_ICCPROFILE, &iccLength, &icc) && icc && iccLength)
        profile = cmsOpenProfileFromMem(icc, iccLength);
    TIFFClose(t);
    matchToScreen(out, profile);
    return out;
}

} // namespace

// A PDF's first page (NSPDFImageRep, CGPDFDocument): its size in points; drawn into an image at
// `atLeast` (the scale that covers it, the page's proportions kept), else at its size. The menu extras'
// and the panels' pictures are PDFs. Poppler draws it, into cairo as Quartz draws into its context.
static Image pdf(const std::vector<unsigned char> &bytes, Size atLeast, Size *pageSize = nullptr)
{
    GBytes *data = g_bytes_new(bytes.data(), bytes.size());
    PopplerDocument *doc = poppler_document_new_from_bytes(data, nullptr, nullptr);
    g_bytes_unref(data);
    if (!doc)
        return {};
    Image img;
    if (PopplerPage *page = poppler_document_get_page(doc, 0)) {
        double w = 0, h = 0;
        poppler_page_get_size(page, &w, &h);
        if (pageSize)
            *pageSize = {w, h};
        if (w > 0 && h > 0 && !pageSize) {
            const double k = atLeast.empty() ? 1.0 : std::max(atLeast.w / w, atLeast.h / h);
            img = Image(int(std::ceil(w * k)), int(std::ceil(h * k)));
            cairo_t *cr = cairo_create(img.surface());
            cairo_scale(cr, k, k);
            poppler_page_render(page, cr);
            cairo_destroy(cr);
            cairo_surface_flush(img.surface());
        }
        g_object_unref(page);
    }
    g_object_unref(doc);
    return img;
}

Image Image::load(const std::string &path, Size atLeast)
{
    std::vector<unsigned char> bytes = readFile(path);
    if (bytes.size() < 8)
        return {};
    if (std::memcmp(bytes.data(), "II*", 3) == 0 || std::memcmp(bytes.data(), "MM\0*", 4) == 0)
        return tiff(path);
    return fromData(bytes, atLeast);
}

Image Image::fromData(const std::vector<unsigned char> &bytes, Size atLeast)
{
    if (bytes.size() < 8)
        return {};
    if (std::memcmp(bytes.data(), "\x89PNG", 4) == 0)
        return fromPngBytes(bytes.data(), bytes.size());
    if (bytes[0] == 0xFF && bytes[1] == 0xD8)
        return jpeg(bytes, atLeast);
    if (std::memcmp(bytes.data(), "icns", 4) == 0)
        return icns(bytes, atLeast);
    if (std::memcmp(bytes.data(), "%PDF", 4) == 0)
        return pdf(bytes, atLeast);
    return {};
}

Image Image::tinted(const Color &c) const
{
    // a template image (NSImage -isTemplate): its shape in one colour, its alpha kept (the menu bar's
    // and the menus' black pictures, white when highlighted)
    if (isNull())
        return {};
    Image out(width(), height());
    cairo_t *cr = cairo_create(out.surface());
    c.set(cr);
    cairo_mask_surface(cr, surface(), 0, 0);
    cairo_destroy(cr);
    cairo_surface_flush(out.surface());
    return out;
}

Size Image::sizeOf(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    unsigned char head[32] = {};
    in.read(reinterpret_cast<char *>(head), sizeof head);
    if (std::memcmp(head, "\x89PNG", 4) == 0)
        return {double(be32(head + 16)), double(be32(head + 20))};
    if (std::memcmp(head, "%PDF", 4) == 0) {
        Size page;
        pdf(readFile(path), {}, &page);
        return page;
    }
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
