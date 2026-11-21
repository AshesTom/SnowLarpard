// SPDX-License-Identifier: LGPL-2.1-or-later
#include "backdrops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace AK;

namespace PB {

namespace {

// -[BackdropsOptionsPatch defaultBaseOptions] and -execute:time:arguments: (a camera that isn't DV)
constexpr float VarianceThreshold = 4;        // kBackdrops_Accumulator_VarianceThreshold
constexpr float OutlierThreshold = 0.04f;     // kBackdrops_Accumulator_OutlierThreshold
constexpr double TimeThreshold = 2;           // kBackdrops_Accumulator_TimeThreshold (seconds)
constexpr float StaticThreshold = 6.5f;       // kBackdrops_Segmentation_StaticThreshold
constexpr float LumaRelaxation = 0.5f;        // kBackdrops_Segmentation_LumaRelaxation
constexpr float ComponentsThreshold = 0.0002f;   // kBackdrops_Segmentation_ConnectedComponentsThreshold
constexpr float MRFTemporal = 0.075f;         // kBackdrops_Segmentation_MRFTemporalConstant
constexpr float MRFSpatial = 0.75f;           // kBackdrops_Segmentation_MRFSpatialConstant
constexpr int MRFIterations = 5;              // kBackdrops_Segmentation_MRFIterations
constexpr float ErodeFactor = 0.02f;          // kBackdrops_Matting_ErodeFactor
constexpr int ErodeMinimum = 3;               // kBackdrops_Matting_ErodeMinimum
constexpr float UpsamplerRange = 2, UpsamplerDomain = 1;   // kBackdrops_Upsampler_RangeConstant, DomainConstant

struct Pixels {
    unsigned char *data;
    int stride, w, h;
    explicit Pixels(const Image &img)
    {
        cairo_surface_t *s = img.surface();
        cairo_surface_flush(s);
        data = cairo_image_surface_get_data(s);
        stride = cairo_image_surface_get_stride(s);
        w = img.width();
        h = img.height();
    }
    // ARGB32, little endian: B, G, R, A
    const unsigned char *at(int x, int y) const { return data + y * stride + x * 4; }
    unsigned char *at(int x, int y) { return data + y * stride + x * 4; }
};

} // namespace

void Backdrops::start()
{
    m_state = Capturing;
    m_frames.clear();
    m_stable = false;
    m_mask.clear();
}

// BackdropsDownsamplerPatch: each 2 × 2 averaged (pavgb), into BT.601 Y'CbCr (Backdrops_601)
void Backdrops::downsample(const Image &frame, std::vector<YCC> &out) const
{
    const Pixels p(frame);
    out.resize(size_t(m_w) * m_h);
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) {
            float r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const unsigned char *q = p.at(std::min(p.w - 1, 2 * x + dx), std::min(p.h - 1, 2 * y + dy));
                    b += q[0], g += q[1], r += q[2];
                }
            r /= 4, g /= 4, b /= 4;
            YCC &c = out[size_t(y) * m_w + x];
            c.y = 16 + (65.481f * r + 128.553f * g + 24.966f * b) / 255;
            c.cb = 128 + (-37.797f * r - 74.203f * g + 112.0f * b) / 255;
            c.cr = 128 + (112.0f * r - 93.786f * g - 18.214f * b) / 255;
        }
}

bool Backdrops::learn(const Image &frame, double time)
{
    if (m_state != Capturing || frame.isNull())
        return false;
    m_w = frame.width() / 2;
    m_h = frame.height() / 2;
    std::vector<YCC> img;
    downsample(frame, img);
    m_frames.push_back(std::move(img));
    if (m_frames.size() > 8)
        m_frames.pop_front();
    if (m_frames.size() < 8)
        return false;
    // -processImagesForContext:time:: the eight frames' average; a pixel moves when their deviation
    // from it passes VarianceThreshold; the background is still when fewer than OutlierThreshold of
    // the pixels move, for TimeThreshold seconds
    const size_t n = size_t(m_w) * m_h;
    std::vector<YCC> avg(n);
    m_motion.assign(n, 0);
    size_t moving = 0;
    for (size_t i = 0; i < n; ++i) {
        YCC a{0, 0, 0};
        for (const auto &f : m_frames)
            a.y += f[i].y, a.cb += f[i].cb, a.cr += f[i].cr;
        a.y /= 8, a.cb /= 8, a.cr /= 8;
        avg[i] = a;
        float sy = 0, sb = 0, sr = 0;
        for (const auto &f : m_frames) {
            sy += (f[i].y - a.y) * (f[i].y - a.y);
            sb += (f[i].cb - a.cb) * (f[i].cb - a.cb);
            sr += (f[i].cr - a.cr) * (f[i].cr - a.cr);
        }
        const int moved = (std::sqrt(sy / 8) > VarianceThreshold) + (std::sqrt(sb / 8) > VarianceThreshold) + (std::sqrt(sr / 8) > VarianceThreshold);
        if (moved) {
            m_motion[i] = 255;
            moving += size_t(moved);
        }
    }
    if (float(moving) / float(n * 2) >= OutlierThreshold) {
        m_stable = false;
        return false;
    }
    if (!m_stable) {
        m_stable = true;
        m_stableSince = time;
        return false;
    }
    if (time - m_stableSince < TimeThreshold)
        return false;
    // -analyzeBackground: the noise, each channel's deviation from the average over the frames
    double ny = 0, nb = 0, nr = 0;
    for (const auto &f : m_frames)
        for (size_t i = 0; i < n; ++i) {
            ny += double(f[i].y - avg[i].y) * (f[i].y - avg[i].y);
            nb += double(f[i].cb - avg[i].cb) * (f[i].cb - avg[i].cb);
            nr += double(f[i].cr - avg[i].cr) * (f[i].cr - avg[i].cr);
        }
    const double count = double(n) * 8;
    m_noise = {std::max(0.5f, float(std::sqrt(ny / count))), std::max(0.5f, float(std::sqrt(nb / count))),
               std::max(0.5f, float(std::sqrt(nr / count)))};
    m_background = std::move(avg);
    m_mask.assign(n, 0);
    m_frames.clear();
    m_state = Ready;
    return true;
}

Image Backdrops::motionOverlay(const Image &frame) const
{
    // Backdrops.qtz's Patch_4: red at 0.5, multiplied, where the background moves
    Image out = frame;
    if (m_motion.empty() || m_w == 0)
        return out;
    Pixels p(out);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            const int lx = std::min(m_w - 1, x / 2), ly = std::min(m_h - 1, y / 2);
            if (!m_motion[size_t(ly) * m_w + lx])
                continue;
            unsigned char *q = p.at(x, y);
            q[0] = (unsigned char)(q[0] / 2);   // red at 0.5 multiplied: the other channels halved
            q[1] = (unsigned char)(q[1] / 2);
        }
    cairo_surface_mark_dirty(out.surface());
    return out;
}

// -[BackdropsSegmentationPatch computeTestStatisticWithImage:]: each pixel's distance to the
// background, its channels weighed by the noise (Cr 1, Cb Cr's noise over Cb's, Y Cr's over Y's times
// LumaRelaxation), the closest of the background's pixel and the ones over and under it (camera shake
// relaxation)
void Backdrops::segment(const std::vector<YCC> &image)
{
    const float cbScale = m_noise[2] / m_noise[1], yScale = m_noise[2] / m_noise[0] * LumaRelaxation;
    const size_t n = size_t(m_w) * m_h;
    m_test.assign(n, 0);
    auto dist = [&](const YCC &p, const YCC &b) {
        const float dr = p.cr - b.cr, db = (p.cb - b.cb) * cbScale, dy = (p.y - b.y) * yScale;
        return dr * dr + db * db + dy * dy;
    };
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) {
            const size_t i = size_t(y) * m_w + x;
            float d = dist(image[i], m_background[i]);
            if (y > 0 && y < m_h - 1) {
                d = std::min(d, dist(image[i], m_background[i - m_w]));
                d = std::min(d, dist(image[i], m_background[i + m_w]));
            }
            m_test[i] = std::sqrt(d + 0.0001f);
        }
    // -execute:time:arguments:: the last mask relaxed once (temporal), small regions flipped, then
    // relaxed again by space
    icm(MRFTemporal, 1);
    connectedComponents();
    icm(MRFSpatial, MRFIterations);
}

// -segmentWithLocalityConstant:iterations:: a pixel is the person when its statistic reaches the
// threshold lowered by c for each of its eight neighbours already the person (4 the even point); the
// edges' and corners' counts scaled up (indexes + 3, + 4)
void Backdrops::icm(float constant, int iterations)
{
    const float t = StaticThreshold * m_noise[2], c = constant * t;
    float table[9];
    for (int k = 0; k < 9; ++k)
        table[k] = (4 - float(k)) * c + t;
    for (int it = 0; it < iterations; ++it)
        for (int y = 0; y < m_h; ++y)
            for (int x = 0; x < m_w; ++x) {
                int count = 0, neighbours = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy)
                            continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= m_w || ny >= m_h)
                            continue;
                        ++neighbours;
                        count += m_mask[size_t(ny) * m_w + nx];
                    }
                const int index = std::min(8, count + (neighbours == 3 ? 4 : neighbours == 5 ? 3 : 0));
                const size_t i = size_t(y) * m_w + x;
                m_mask[i] = m_test[i] >= table[index];
            }
}

// -connectedComponents: the mask's regions (of either kind) smaller than ComponentsThreshold of the
// picture flipped
void Backdrops::connectedComponents()
{
    const size_t n = size_t(m_w) * m_h;
    const int minimum = int(float(m_w) * ComponentsThreshold * float(m_h));
    std::vector<int> label(n, -1);
    std::vector<size_t> stack;
    for (size_t start = 0; start < n; ++start) {
        if (label[start] >= 0)
            continue;
        std::vector<size_t> region;
        stack.push_back(start);
        label[start] = int(start);
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            region.push_back(i);
            const int x = int(i % m_w), y = int(i / m_w);
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto &q : nb) {
                if (q[0] < 0 || q[1] < 0 || q[0] >= m_w || q[1] >= m_h)
                    continue;
                const size_t j = size_t(q[1]) * m_w + q[0];
                if (label[j] < 0 && m_mask[j] == m_mask[i]) {
                    label[j] = int(start);
                    stack.push_back(j);
                }
            }
        }
        if (int(region.size()) < minimum)
            for (size_t i : region)
                m_mask[i] ^= 1;
    }
}

// -[BackdropsMattingPatch computeAlphaWithMask:image:erodeIterations:]: from the mask's edge inwards,
// a band of erodeIterations layers (the picture's height times ErodeFactor, ErodeMinimum at least);
// in it the alpha and the person's colour are worked out from the background's and the nearest
// inner pixel's (the band's own arithmetic estimated: the rest of the function not read)
void Backdrops::matte(const std::vector<YCC> &image)
{
    const size_t n = size_t(m_w) * m_h;
    const int band = std::max(ErodeMinimum, int(float(m_h) * ErodeFactor));
    std::vector<int> layer(n, 0);   // 0: background or not reached; 1…band: the band; band + 1: inside
    std::vector<size_t> frontier;
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) {
            const size_t i = size_t(y) * m_w + x;
            if (!m_mask[i])
                continue;
            layer[i] = band + 1;
            const bool edge = (x > 0 && !m_mask[i - 1]) || (x < m_w - 1 && !m_mask[i + 1]) || (y > 0 && !m_mask[i - m_w]) ||
                              (y < m_h - 1 && !m_mask[i + m_w]);
            if (edge) {
                layer[i] = 1;
                frontier.push_back(i);
            }
        }
    for (int k = 2; k <= band; ++k) {
        std::vector<size_t> next;
        for (size_t i : frontier) {
            const int x = int(i % m_w), y = int(i / m_w);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= m_w || ny >= m_h)
                        continue;
                    const size_t j = size_t(ny) * m_w + nx;
                    if (layer[j] == band + 1) {
                        layer[j] = k;
                        next.push_back(j);
                    }
                }
        }
        frontier.swap(next);
    }
    auto rgb = [](const YCC &c) {
        const float y = (c.y - 16) * 255 / 219, cb = c.cb - 128, cr = c.cr - 128;
        return std::array<float, 3>{std::clamp(y + 1.402f * cr * 255 / 224, 0.f, 255.f),
                                    std::clamp(y - (0.344136f * cb + 0.714136f * cr) * 255 / 224, 0.f, 255.f),
                                    std::clamp(y + 1.772f * cb * 255 / 224, 0.f, 255.f)};
    };
    m_alpha.assign(n, 0);
    m_fg.assign(n, {0, 0, 0});
    // the inner colours carried outwards into the band
    std::vector<std::array<float, 3>> inner(n);
    std::vector<char> known(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (layer[i] == band + 1) {
            inner[i] = rgb(image[i]);
            known[i] = 1;
        }
    for (int k = band; k >= 1; --k)
        for (size_t i = 0; i < n; ++i) {
            if (layer[i] != k)
                continue;
            const int x = int(i % m_w), y = int(i / m_w);
            std::array<float, 3> sum{0, 0, 0};
            int c = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= m_w || ny >= m_h)
                        continue;
                    const size_t j = size_t(ny) * m_w + nx;
                    if (known[j]) {
                        for (int ch = 0; ch < 3; ++ch)
                            sum[ch] += inner[j][ch];
                        ++c;
                    }
                }
            inner[i] = c ? std::array<float, 3>{sum[0] / c, sum[1] / c, sum[2] / c} : rgb(image[i]);
            known[i] = 1;
        }
    for (size_t i = 0; i < n; ++i) {
        if (!layer[i])
            continue;
        const auto I = rgb(image[i]);
        if (layer[i] == band + 1) {
            m_alpha[i] = 1;
            m_fg[i] = I;
            continue;
        }
        const auto B = rgb(m_background[i]), F = inner[i];
        float num = 0, den = 0;
        for (int ch = 0; ch < 3; ++ch) {
            num += (I[ch] - B[ch]) * (F[ch] - B[ch]);
            den += (F[ch] - B[ch]) * (F[ch] - B[ch]);
        }
        m_alpha[i] = den > 1 ? std::clamp(num / den, 0.f, 1.f) : 1.f;
        m_fg[i] = F;
    }
}

Image Backdrops::composite(const Image &frame, const Image &backdrop)
{
    if (m_state != Ready || frame.isNull())
        return frame;
    m_w = frame.width() / 2;
    m_h = frame.height() / 2;
    std::vector<YCC> image;
    downsample(frame, image);
    if (m_background.size() != image.size())
        return frame;
    segment(image);
    matte(image);
    // Backdrops.qtz's upsample kernel (the joint bilateral upsampler: the low mask weighed by the
    // offset, exp(−|d|²/σf), and the colour difference, exp(−|Ip − Iq|²/σg)), then its composite
    // kernel: alpha from 0.2 to 0.8 stretched over 0 to 1, the picture where it is whole, the
    // foreground's colour times alpha elsewhere; over the backdrop
    Image copy(frame.width(), frame.height());
    {
        cairo_t *cr = cairo_create(copy.surface());
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        if (!backdrop.isNull())
            backdrop.draw(cr, Rect{0, 0, double(frame.width()), double(frame.height())});
        cairo_destroy(cr);
    }
    const Pixels src(frame);
    Pixels dst(copy);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const unsigned char *ip = src.at(x, y);
            const float lx = float(x) / 2, ly = float(y) / 2;
            float sum = 0, den = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -1; dy < 1; ++dy) {
                    const int qx = std::clamp(int(lx) + dx, 0, m_w - 1), qy = std::clamp(int(ly) + dy, 0, m_h - 1);
                    const unsigned char *iq = src.at(std::clamp(x + 2 * dx, 0, src.w - 1), std::clamp(y + 2 * dy, 0, src.h - 1));
                    float d2 = 0;
                    for (int ch = 0; ch < 3; ++ch) {
                        const float d = (float(ip[ch]) - float(iq[ch])) / 255;
                        d2 += d * d;
                    }
                    const float w = std::exp(-float(dx * dx + dy * dy) / UpsamplerDomain) * std::exp(-d2 / UpsamplerRange);
                    sum += m_alpha[size_t(qy) * m_w + qx] * w;
                    den += w;
                }
            float a = den > 0 ? sum / den : 0;
            a = (std::clamp(a, 0.2f, 0.8f) - 0.2f) / 0.6f;
            if (a <= 0)
                continue;
            unsigned char *o = dst.at(x, y);
            if (a > 0.99f) {
                std::memcpy(o, ip, 4);
                continue;
            }
            const auto &fg = m_fg[size_t(std::min(m_h - 1, y / 2)) * m_w + std::min(m_w - 1, x / 2)];
            // a × the foreground, over the backdrop (SourceOverCompositing)
            o[0] = (unsigned char)std::clamp(a * fg[2] + (1 - a) * o[0], 0.f, 255.f);
            o[1] = (unsigned char)std::clamp(a * fg[1] + (1 - a) * o[1], 0.f, 255.f);
            o[2] = (unsigned char)std::clamp(a * fg[0] + (1 - a) * o[2], 0.f, 255.f);
            o[3] = 255;
        }
    cairo_surface_mark_dirty(copy.surface());
    return copy;
}

} // namespace PB
