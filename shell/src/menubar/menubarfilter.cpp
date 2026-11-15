// SPDX-License-Identifier: LGPL-2.1-or-later
#include "menubarfilter.h"

#include "assets.h"

#include <QFile>
#include <QPainter>
#include <QtMath>
#include <QVector>

namespace {

struct Plane {
    int w = 0, h = 0;
    QVector<float> v;   // r, g, b per pixel
    float *at(int x, int y) { return v.data() + 3 * (y * w + x); }
};

// a separable Gaussian, sigma = radius as CoreImage's CIGaussianBlur takes it; edges clamped
Plane blur(const Plane &in, float sigma)
{
    const int r = qCeil(3 * sigma);
    QVector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; ++i)
        sum += k[i + r] = qExp(-(i * i) / (2 * sigma * sigma));
    for (float &f : k)
        f /= sum;
    Plane tmp = in, out = in;
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int i = -r; i <= r; ++i)
                    s += k[i + r] * in.v[3 * (y * in.w + qBound(0, x + i, in.w - 1)) + c];
                tmp.v[3 * (y * in.w + x) + c] = s;
            }
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int i = -r; i <= r; ++i)
                    s += k[i + r] * tmp.v[3 * (qBound(0, y + i, in.h - 1) * in.w + x) + c];
                out.v[3 * (y * in.w + x) + c] = s;
            }
    return out;
}

} // namespace

QImage generateMenuBar(const QImage &behind)
{
    QImage src = behind.convertToFormat(QImage::Format_RGB32);
    Plane desk;
    desk.w = src.width();
    desk.h = src.height();
    desk.v.resize(3 * desk.w * desk.h);
    double mean = 0;
    for (int y = 0; y < desk.h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < desk.w; ++x) {
            float *p = desk.at(x, y);
            p[0] = qRed(line[x]) / 255.f, p[1] = qGreen(line[x]) / 255.f, p[2] = qBlue(line[x]) / 255.f;
            mean += p[0] + p[1] + p[2];
        }
    }
    const float L = desk.w && desk.h ? float(mean / (3.0 * desk.w * desk.h)) : 0.5f;
    auto mix = [](float a, float b, float t) { return a * (1 - t) + b * t; };
    const float white = mix(0.60f, 0.25f, L), lift = mix(0.35f, 0.15f, L);
    Plane b = blur(desk, 1.6f);

    QImage out(desk.w, desk.h, QImage::Format_RGB32);
    const float h = desk.h;
    for (int row = 0; row < desk.h; ++row) {
        const float y = row + 0.5f;   // from the top
        const float ramp = qPow(qMax(0.f, 1 - y / h), 0.75f);
        const float top = qBound(0.f, 1.5f - y, 1.f) * 0.5f;
        const float edge = qBound(0.f, y - h + 1.5f, 1.f);
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(row));
        for (int x = 0; x < desk.w; ++x) {
            const float *p = b.at(x, row);
            int rgb[3];
            for (int c = 0; c < 3; ++c) {
                float v = (p[c] * (1 - white) + white) / (1 - lift * ramp);
                v = v * (1 - top) + top;
                v = mix(v, p[c] * 0.59f, edge);
                rgb[c] = qBound(0, qRound(v * 255), 255);
            }
            line[x] = qRgb(rgb[0], rgb[1], rgb[2]);
        }
    }
    return out;
}

bool drawMenuApple(QImage &bar, const QImage &behind)
{
    static const QImage apple = [] {
        // RGBA, premultiplied, the top row first (QuartzCore's _imageApple)
        QFile f(Assets::path("/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/MenuApple-32x22.rgba"));
        if (!f.open(QIODevice::ReadOnly))
            return QImage();
        QByteArray bytes = f.readAll();
        if (bytes.size() != 32 * 22 * 4)
            return QImage();
        return QImage(reinterpret_cast<const uchar *>(bytes.constData()), 32, 22, 32 * 4, QImage::Format_RGBA8888_Premultiplied).copy();
    }();
    if (apple.isNull() || bar.height() < apple.height())
        return false;
    const int X = 17, W = apple.width(), H = apple.height();   // CIMenuGenerator: the Apple's extent starts 17 px in
    auto mix = [](float a, float b, float t) { return a * (1 - t) + b * t; };

    // the desktop: its mean from x 17 on, and blurred
    QImage src = behind.convertToFormat(QImage::Format_RGB32);
    Plane desk;
    desk.w = src.width();
    desk.h = src.height();
    desk.v.resize(3 * desk.w * desk.h);
    double mean = 0;
    int count = 0;
    for (int y = 0; y < desk.h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < desk.w; ++x) {
            float *p = desk.at(x, y);
            p[0] = qRed(line[x]) / 255.f, p[1] = qGreen(line[x]) / 255.f, p[2] = qBlue(line[x]) / 255.f;
            if (x >= X)
                mean += p[0] + p[1] + p[2], count += 3;
        }
    }
    const float L = count ? float(mean / count) : 0.5f;
    Plane local;   // the part under the Apple, with room for the blur
    const int pad = 24, x0 = qMax(0, X - pad), x1 = qMin(desk.w, X + W + pad);
    local.w = x1 - x0;
    local.h = desk.h;
    local.v.resize(3 * local.w * local.h);
    for (int y = 0; y < local.h; ++y)
        for (int x = 0; x < local.w; ++x)
            std::copy_n(desk.at(x0 + x, y), 3, local.at(x, y));
    Plane blurred = blur(local, 8.f);

    // the Apple's alpha, and blurred by 1.5 for the inner shadow
    Plane alpha;
    alpha.w = W, alpha.h = H;
    alpha.v.resize(3 * W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float a = qAlpha(apple.pixel(x, y)) / 255.f;
            float *p = alpha.at(x, y);
            p[0] = p[1] = p[2] = a;
        }
    Plane shadowAlpha = blur(alpha, 1.5f);

    const float black = mix(0.30f, 0.075f, L), lift = mix(0.75f, 0.5f, L), shine = mix(0.6f, 0.85f, L);
    const float h = H;
    QImage glass(W, H, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < H; ++y) {
        const float py = y + 0.5f, t = py / h;
        const float divisor = 1 - lift * mix(0.5f, 1.f, qPow(t, 0.75f));
        for (int x = 0; x < W; ++x) {
            const float px = x + 0.5f;
            const float dx = px - 7.5f, dy = py - 4.f;
            const float v = dx < 0 ? dy : dy < 0 ? dx : qSqrt(dx * dx + dy * dy);
            const float gleam = qBound(0.f, v - 8, 1.f) * qPow(t, 2.75f) * shine;
            const float shade = 1 - shadowAlpha.at(x, y)[0];   // the inner shadow's alpha
            const float a = alpha.at(x, y)[0];
            const int bx = X + x - x0;
            int rgb[3];
            for (int c = 0; c < 3; ++c) {
                float f = blurred.at(qBound(0, bx, local.w - 1), qMin(y, local.h - 1))[c] * black / divisor + gleam;
                f = f * (1 - shade);   // black over it
                rgb[c] = qBound(0, qRound(f * a * 255), 255);   // kept inside the Apple: premultiplied
            }
            glass.setPixel(x, y, qRgba(rgb[0], rgb[1], rgb[2], qRound(a * 255)));
        }
    }

    QPainter p(&bar);
    // the bevel: the Apple in white at 0.6, one pixel lower, under it
    QImage bevel(W, H, QImage::Format_ARGB32_Premultiplied);
    bevel.fill(Qt::transparent);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int a = qRound(qAlpha(apple.pixel(x, y)) * 0.6f);
            bevel.setPixel(x, y, qRgba(a, a, a, a));
        }
    p.drawImage(X, 1, bevel);
    p.drawImage(X, 0, glass);
    return true;
}
