// SPDX-License-Identifier: LGPL-2.1-or-later
#include "starfield.h"

#include "assets.h"

#include <QFile>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_2_1>
#include <QOpenGLShaderProgram>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace TM {

namespace {

const QString Resources = "/System/Library/PrivateFrameworks/Backup.framework/Versions/A/Resources/";

// FUN_00011bd6: U(a, b) = random()/2147483647·(b−a)+a
double U(double a, double b)
{
    if (a > b)
        std::swap(a, b);
    return double(random()) / 2147483647.0 * (b - a) + a;
}

// FUN_00011ad9(0, 1, i, seed): the clouds' fixed targets, Park–Miller (Schrage) steps
double cloudRandom(int i, long seed)
{
    long v = seed + std::lround(std::round(i * 1000.0) * 0.001 * 24.855 * 1000.0);
    if (v == 0)
        v = 0x7fffffff;
    const int steps = int(std::labs(v) & 15);
    for (int k = 0; k < steps; ++k) {
        v = 48271 * (v % 44488) - 3399 * (v / 44488);
        if (v < 0)
            v += 2147483647;
        if (v == 0)
            v = 0x7fffffff;
    }
    return double(v) / 2147483647.0;
}

// GVInterpolator: v1 + (v2 − v1)·S(t′), t′ the time warped by the tension
struct Interpolator {
    double v1, v2, tension;
    const Spline *spline;
    double operator()(double t) const
    {
        t -= std::floor(t);   // inputRepeat
        const double w = t * t + (1 - (tension + 1) / 2) * 2 * t * (1 - t);
        // valueAtTime: clamps the time to [0, 1], not the spline's value: A's curve overshoots to about
        // 1.9 between its points, the clouds twice as opaque as a clamped curve would make them
        const double s = spline && !spline->empty() ? (*spline)(w) : w;
        return v1 + (v2 - v1) * s;
    }
};

void lookAt(QOpenGLFunctions_2_1 *f, double ex, double ey, double ez, double cx, double cy, double cz, double ux, double uy, double uz)
{
    double fx = cx - ex, fy = cy - ey, fz = cz - ez;
    const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl, fy /= fl, fz /= fl;
    double sx = fy * uz - fz * uy, sy = fz * ux - fx * uz, sz = fx * uy - fy * ux;
    const double sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    sx /= sl, sy /= sl, sz /= sl;
    const double vx = sy * fz - sz * fy, vy = sz * fx - sx * fz, vz = sx * fy - sy * fx;
    const double m[16] = {sx, vx, -fx, 0, sy, vy, -fy, 0, sz, vz, -fz, 0, 0, 0, 0, 1};
    f->glMultMatrixd(m);
    f->glTranslated(-ex, -ey, -ez);
}

} // namespace

Spline::Spline(std::initializer_list<std::pair<double, double>> points)
{
    for (const auto &p : points) {
        x.push_back(p.first);
        y.push_back(p.second);
    }
    // Numerical Recipes' spline(), natural ends
    const size_t n = x.size();
    y2.assign(n, 0);
    std::vector<double> u(n, 0);
    for (size_t i = 1; i + 1 < n; ++i) {
        const double sig = (x[i] - x[i - 1]) / (x[i + 1] - x[i - 1]);
        const double p = sig * y2[i - 1] + 2;
        y2[i] = (sig - 1) / p;
        u[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]) - (y[i] - y[i - 1]) / (x[i] - x[i - 1]);
        u[i] = (6 * u[i] / (x[i + 1] - x[i - 1]) - sig * u[i - 1]) / p;
    }
    if (n > 1)
        y2[n - 1] = 0;
    for (size_t k = n - 1; k-- > 0;)
        y2[k] = y2[k] * y2[k + 1] + u[k];
}

double Spline::operator()(double t) const
{
    if (x.empty())
        return t;
    t = std::clamp(t, x.front(), x.back());
    size_t lo = 0, hi = x.size() - 1;
    while (hi - lo > 1) {
        const size_t k = (hi + lo) / 2;
        (x[k] > t ? hi : lo) = k;
    }
    const double h = x[hi] - x[lo];
    if (h == 0)
        return y[lo];
    const double a = (x[hi] - t) / h, b = (t - x[lo]) / h;
    return a * y[lo] + b * y[hi] + ((a * a * a - a) * y2[lo] + (b * b * b - b) * y2[hi]) * h * h / 6;
}

double bezier(double x1, double y1, double x2, double y2, double t)
{
    if (t <= 0)
        return 0;
    if (t >= 1)
        return 1;
    auto bx = [&](double s) { return 3 * x1 * s * (1 - s) * (1 - s) + 3 * x2 * s * s * (1 - s) + s * s * s; };
    auto by = [&](double s) { return 3 * y1 * s * (1 - s) * (1 - s) + 3 * y2 * s * s * (1 - s) + s * s * s; };
    double lo = 0, hi = 1, s = t;
    for (int i = 0; i < 40; ++i) {
        s = (lo + hi) / 2;
        (bx(s) < t ? lo : hi) = s;
    }
    return by(s);
}

unsigned uploadTexture(QOpenGLFunctions_2_1 *f, const QImage &image, bool mipmap)
{
    const QImage img = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    GLuint t = 0;
    f->glGenTextures(1, &t);
    f->glBindTexture(GL_TEXTURE_2D, t);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mipmap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mipmap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (mipmap)
        f->glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, img.bytesPerLine() / 4);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return t;
}

StarField::StarField(QOpenGLFunctions_2_1 *fn) : f(fn)
{
    srandom(unsigned(std::time(nullptr)));   // FUN_00011bc3
    const char *const colours[4] = {"White", "Red", "Blue", "Yellow"};
    for (int c = 0; c < 4; ++c) {
        m_star[c] = uploadTexture(f, Assets::image(Resources + "Normal" + colours[c] + "Star.tiff"));
        m_bgStar[c] = uploadTexture(f, Assets::image(Resources + "Hyper" + colours[c] + "Star.tiff"));
    }
    m_sun = uploadTexture(f, Assets::image(Resources + "sun.tiff"));
    m_cloud = uploadTexture(f, Assets::image(Resources + "Cloud.png"), true);

    // the interpolators (_initInterpolators)
    m_A = Spline{{0, 0.8846}, {0.8538, 1.0}, {1, 0}};
    m_W = Spline{{0, 0.2731}, {0.2923, 0.3269}, {0.5731, 0.4846}, {1, 1.0}};
    m_I1 = Spline{{0, 0}, {0.2885, 0.3769}, {0.5, 0.5}, {0.7038, 0.65}, {1, 1}};
    m_I2 = Spline{{0, 0}, {0.2923, 0.3692}, {0.5, 0.5}, {0.7423, 0.6385}, {1, 1}};
    for (int i = 0; i < 100; ++i) {
        m_r1[i] = cloudRandom(i, 0x634846be);
        m_r2[i] = cloudRandom(i, 0x0da4ba97);
    }

    // RemapVortex, from the framework's Resources
    QFile vs(Assets::path(Resources + "RemapVortex.vert")), fs(Assets::path(Resources + "RemapVortex.frag"));
    if (vs.open(QIODevice::ReadOnly) && fs.open(QIODevice::ReadOnly)) {
        m_program = std::make_unique<QOpenGLShaderProgram>();
        if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vs.readAll()) ||
            !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fs.readAll()) || !m_program->link())
            m_program.reset();
    }
    if (m_program) {
        m_cloudBuffer = std::make_unique<QOpenGLFramebufferObject>(512, 512);
        m_drawBuffer = std::make_unique<QOpenGLFramebufferObject>(512, 512);
        // the twirl, once on the processor (drawCachedVortex, VortexCache.frag's formula): radius 3,
        // 7 radians, power 75; B the mask, R and G the source's halves, A their dropped bits
        std::vector<unsigned char> remap(512 * 512 * 4);
        for (int y = 0; y < 512; ++y) {
            for (int x = 0; x < 512; ++x) {
                const double u = x / 512.0, v = y / 512.0, dx = u - 0.5, dy = v - 0.5;
                const double d = std::sqrt(dx * dx + dy * dy);
                double sx = u, sy = v;
                if (d < 3.0) {
                    const double k = (3.0 - d) / 3.0;
                    const double a = (dx == 0 ? (dy >= 0 ? M_PI / 2 : -M_PI / 2) : std::atan(dy / dx)) + 7.0 * std::pow(k, 75.0) * k;
                    const double sign = dx < 0 ? -1 : 1;
                    sx = 0.5 + d * sign * std::cos(a);
                    sy = 0.5 + d * sign * std::sin(a);
                }
                const int px = std::clamp(int(sx * 512), 0, 511), py = std::clamp(int(sy * 512), 0, 511);
                const double mask = d <= 0.475 ? 255 : d >= 0.5 ? 0 : 255 * (1 - (d - 0.475) * 40);
                unsigned char *o = &remap[size_t(y * 512 + x) * 4];
                o[0] = (unsigned char)(px >> 1);
                o[1] = (unsigned char)(py >> 1);
                o[2] = (unsigned char)std::lround(mask);
                o[3] = (px & 1) && (py & 1) ? 255 : (py & 1) ? 170 : (px & 1) ? 85 : 0;
            }
        }
        GLuint t = 0;
        f->glGenTextures(1, &t);
        f->glBindTexture(GL_TEXTURE_2D, t);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, remap.data());
        m_remap = t;
        m_animated = true;
    } else {
        // the static background: vortex.png, the galaxy pre-rendered, over the whole view
        m_static = uploadTexture(f, Assets::image(Resources + "vortex.png"));
    }
}

StarField::~StarField() = default;

void StarField::setup(double aspect)
{
    m_aspect = aspect;
    const double tanHalf = std::tan(0.30543);   // 35° horizontal field of view
    m_nearW = 2 * 0.01 * tanHalf;
    m_nearH = m_nearW / aspect;
    m_farW = 2 * 5.0 * tanHalf;
    m_farH = m_farW / aspect;
    m_bgW = 2 * 4.8 * tanHalf;
    m_bgH = m_bgW / aspect;
    // initStarsWithRandomZ: 4 × 300, keeping the axis clear
    for (auto &list : m_stars) {
        list.clear();
        for (int i = 0; i < 300; ++i) {
            Star s;
            s.z = float(U(-5, 0));
            s.s = float(U(0.01, 0.01) / 2);
            double x = U(-m_farW / 2, m_farW / 2), y = U(-m_farH / 2, m_farH / 2);
            if (x >= -m_nearW / 2 - s.s && x <= 0)
                x = U(-m_farW / 2, -m_nearW / 2 - s.s);
            else if (x >= 0 && x <= m_nearW / 2 + s.s)
                x = U(m_nearW / 2 + s.s, m_farW / 2);
            if (y >= -m_nearH / 2 - s.s && y <= 0)
                y = U(-m_farH / 2, -m_nearH / 2 - s.s);
            else if (y >= 0 && y <= m_nearH / 2 + s.s)
                y = U(m_nearH / 2 + s.s, m_farH / 2);
            s.x = float(x);
            s.y = float(y);
            list.push_back(s);
        }
    }
    // initBackgroundStars: 4 × 850 on the plane z = −4.8, each with a radial direction
    for (auto &list : m_bgStars) {
        list.clear();
        for (int i = 0; i < 850; ++i) {
            Star s;
            s.x = float(U(-m_bgW / 2, m_bgW / 2));
            s.y = float(U(-m_bgH / 2, m_bgH / 2));
            s.z = -4.8f;
            s.s = float(U(0.005, 0.015) / 2);
            const double th = std::atan2(s.y - m_prevOffsetY * m_bgW / m_farW, s.x);
            const double v = U(0.0005, 0.0005);
            s.vx = float(v * std::cos(th));
            s.vy = float(v * std::sin(th));
            s.life = float(U(0, 5));
            list.push_back(s);
        }
    }
}

void StarField::drawQuads(const std::vector<Star> &stars, unsigned texture)
{
    std::vector<float> v, t;
    v.reserve(stars.size() * 12);
    t.reserve(stars.size() * 8);
    for (const Star &s : stars) {
        const float q[12] = {s.x - s.s, s.y + s.s, s.z, s.x + s.s, s.y + s.s, s.z, s.x + s.s, s.y - s.s, s.z, s.x - s.s, s.y - s.s, s.z};
        const float c[8] = {0, 0, 0, 1, 1, 1, 1, 0};
        v.insert(v.end(), q, q + 12);
        t.insert(t.end(), c, c + 8);
    }
    f->glBindTexture(GL_TEXTURE_2D, texture);
    f->glEnableClientState(GL_VERTEX_ARRAY);
    f->glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    f->glVertexPointer(3, GL_FLOAT, 0, v.data());
    f->glTexCoordPointer(2, GL_FLOAT, 0, t.data());
    f->glDrawArrays(GL_QUADS, 0, GLsizei(stars.size() * 4));
    f->glDisableClientState(GL_VERTEX_ARRAY);
    f->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

// VortexGL -renderAtTime: and -drawVortex: the clouds into their buffer, then twirled into the other
void StarField::renderClouds(double time)
{
    const double prev = m_prevTime < 0 ? time - 1.0 / 60 : m_prevTime;
    m_T += (time - prev) * 0.3;   // the gas speed at rest
    const Interpolator A{0, 0.03, 0, &m_A}, W{0, 1.5, -0.25, &m_W}, Z{-5, 0, 0, nullptr}, I1{-1, 1, 0, &m_I1}, I2{-1, 1, 0, &m_I2};

    m_cloudBuffer->bind();
    f->glViewport(0, 0, 512, 512);
    f->glClearColor(0, 0, 0, 0);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glMatrixMode(GL_PROJECTION);
    f->glLoadIdentity();
    const double t30 = std::tan(M_PI / 6);
    f->glFrustum(-0.1 * t30, 0.1 * t30, -0.1 * t30, 0.1 * t30, 0.1, 100);
    f->glMatrixMode(GL_MODELVIEW);
    f->glLoadIdentity();
    f->glTranslated(0, 0, -1 / t30);
    f->glEnable(GL_TEXTURE_2D);
    f->glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    f->glEnable(GL_BLEND);
    f->glBlendFunc(GL_ONE, GL_ONE);
    f->glBindTexture(GL_TEXTURE_2D, m_cloud);
    f->glBegin(GL_QUADS);
    for (int i = 0; i < 100; ++i) {
        const double phase = i / 100.0 * 360.0;
        double t = (m_T + 41) / 10 + phase / 360;   // _LFOTime: a saw, period 10
        t -= std::floor(t);
        const double a = A(t), w = W(t), z = Z(t);
        const Interpolator X{0, I1(m_r1[i]), 0, nullptr}, Y{0, I2(m_r2[i]), 0, nullptr};
        const double x = X(t), y = Y(t), hh = 0.8205 * w / 2;
        f->glColor4d(0.1544 * a, 0.7132 * a, 0.7574 * a, a);
        f->glTexCoord2d(0, 0);
        f->glVertex3d(x - w / 2, y - hh, z);
        f->glTexCoord2d(1, 0);
        f->glVertex3d(x + w / 2, y - hh, z);
        f->glTexCoord2d(1, 1);
        f->glVertex3d(x + w / 2, y + hh, z);
        f->glTexCoord2d(0, 1);
        f->glVertex3d(x - w / 2, y + hh, z);
    }
    f->glEnd();
    m_cloudBuffer->release();

    m_drawBuffer->bind();
    f->glViewport(0, 0, 512, 512);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glDisable(GL_BLEND);
    f->glMatrixMode(GL_PROJECTION);
    f->glLoadIdentity();
    f->glOrtho(-1, 1, -1, 1, -1, 1);
    f->glMatrixMode(GL_MODELVIEW);
    f->glLoadIdentity();
    m_program->bind();
    m_program->setUniformValue("clouds", 0);
    m_program->setUniformValue("remapTexture", 1);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, m_remap);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, m_cloudBuffer->texture());
    f->glColor4d(1, 1, 1, 1);
    f->glBegin(GL_QUADS);
    f->glTexCoord2d(0, 0);
    f->glVertex2d(-1, -1);
    f->glTexCoord2d(1, 0);
    f->glVertex2d(1, -1);
    f->glTexCoord2d(1, 1);
    f->glVertex2d(1, 1);
    f->glTexCoord2d(0, 1);
    f->glVertex2d(-1, 1);
    f->glEnd();
    m_program->release();
    m_drawBuffer->release();
}

void StarField::draw(double time, int w, int h, double galaxyY, double zOffset)
{
    const double aspect = h > 0 ? double(w) / h : 1.6;
    // the galaxy's height: Y view points from the bottom
    const double offsetY = (2 / aspect) * (galaxyY / h - 0.5);
    if (aspect != m_aspect) {
        m_prevOffsetY = offsetY;
        setup(aspect);
    }
    if (!m_animated) {
        f->glViewport(0, 0, w, h);
        f->glClearColor(0, 0, 0, 1);
        f->glClear(GL_COLOR_BUFFER_BIT);
        f->glMatrixMode(GL_PROJECTION);
        f->glLoadIdentity();
        f->glOrtho(0, 1, 0, 1, -1, 1);
        f->glMatrixMode(GL_MODELVIEW);
        f->glLoadIdentity();
        f->glEnable(GL_TEXTURE_2D);
        f->glDisable(GL_BLEND);
        f->glBindTexture(GL_TEXTURE_2D, m_static);
        f->glColor4d(1, 1, 1, 1);
        f->glBegin(GL_QUADS);
        f->glTexCoord2d(0, 1);
        f->glVertex2d(0, 0);
        f->glTexCoord2d(1, 1);
        f->glVertex2d(1, 0);
        f->glTexCoord2d(1, 0);
        f->glVertex2d(1, 1);
        f->glTexCoord2d(0, 0);
        f->glVertex2d(0, 1);
        f->glEnd();
        return;
    }
    renderClouds(time);

    const double elapsedFrames = m_prevTime < 0 ? 1 : std::min((time - m_prevTime) / (1.0 / 60), 1.0);
    const double dz = zOffset - m_prevZ;
    m_prevZ = zOffset;
    m_prevTime = time;

    // checkAndUpdateStars: towards the viewer at 0.0003 × warp (starsSpeed 0.007 at rest × 400)
    const double warp = 0.007 * 400;
    for (auto &list : m_stars)
        for (Star &s : list) {
            s.z += float(dz + 0.0003 * warp * elapsedFrames);
            if (s.z >= 0)
                s.z = -5;
            else if (s.z < -5)
                s.z = 0;
        }
    // checkAndUpdateBackgroundStars: every 0.2 s, or every frame while the stars' z offset moves
    const double gy = offsetY * m_bgW / 2, prevGy = m_prevOffsetY * m_bgW / 2;
    if (dz != 0 || time - m_lastBG >= 0.2 || gy != prevGy) {
        m_lastBG = time;
        const double planeW = m_bgW, planeH = m_bgH;
        for (auto &list : m_bgStars)
            for (Star &s : list) {
                if (gy != prevGy) {   // the galaxy moved: they shift with it, wrapped vertically
                    s.y += float(gy - prevGy);
                    if (s.y > planeH / 2)
                        s.y -= float(planeH);
                    else if (s.y < -planeH / 2)
                        s.y += float(planeH);
                }
                double k = 1;
                if (dz != 0) {
                    const double ax = std::fabs(s.x) + 0.15, ay = std::fabs(s.y - gy) + 0.15;
                    k = (dz > 0 ? 1 : -1) * (std::fabs(dz) * 50 + 1) * (ax * ax + ay * ay) * 20 / (planeW * planeW + planeH * planeH);
                    if (dz < 0)
                        s.life -= float(std::fabs(k) * 0.05);
                }
                s.x += float(k * s.vx);
                s.y += float(k * s.vy);
                if (s.life <= 0 || std::fabs(s.x) > planeW / 2 || std::fabs(s.y) > planeH / 2) {
                    s.x = float(U(-planeW / 2, planeW / 2));
                    s.y = float(U(-planeH / 2, planeH / 2));
                    const double th = std::atan2(s.y - gy, s.x), v = 0.0005;
                    s.vx = float(v * std::cos(th));
                    s.vy = float(v * std::sin(th));
                    s.life = float(U(0, 5));
                }
            }
    }
    m_prevOffsetY = offsetY;

    // the frame: black, then the gas, the sun, the background stars, the foreground stars
    f->glViewport(0, 0, w, h);
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glMatrixMode(GL_PROJECTION);
    f->glLoadIdentity();
    f->glFrustum(-m_nearW / 2, m_nearW / 2, -m_nearH / 2, m_nearH / 2, 0.01, 5.01);
    f->glMatrixMode(GL_MODELVIEW);
    f->glLoadIdentity();
    f->glEnable(GL_TEXTURE_2D);
    // drawGas: and drawSun: set GL_REPLACE (0x1e01), and nothing sets it back in the view's context
    // (VortexGL's GL_MODULATE is its pbuffers' own contexts' state): the stars drawn after them take
    // their textures' colours as they are, the background stars' glColor 1 + fBGBrightness (0.4)
    // having no effect
    f->glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    f->glEnable(GL_BLEND);
    f->glColor4d(1, 1, 1, 1);

    auto quad = [&](double cx, double cy, double half, double z) {
        f->glBegin(GL_QUADS);
        f->glTexCoord2d(0, 0);
        f->glVertex3d(cx - half, cy - half, z);
        f->glTexCoord2d(1, 0);
        f->glVertex3d(cx + half, cy - half, z);
        f->glTexCoord2d(1, 1);
        f->glVertex3d(cx + half, cy + half, z);
        f->glTexCoord2d(0, 1);
        f->glVertex3d(cx - half, cy + half, z);
        f->glEnd();
    };
    // drawGas: the twirled clouds, fGasWidth 2.5
    f->glBlendFunc(GL_ONE, GL_ONE);
    f->glBindTexture(GL_TEXTURE_2D, m_drawBuffer->texture());
    quad(0, gy, 0.625 * m_bgW, -4.8);
    // drawSun: the same quad × 0.375
    f->glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    f->glBindTexture(GL_TEXTURE_2D, m_sun);
    quad(0, gy, 0.625 * 0.375 * m_bgW, -4.8);

    // the stars: additive, fogged to black from 4.81 to 5.01
    f->glEnable(GL_FOG);
    f->glFogi(GL_FOG_MODE, GL_LINEAR);
    f->glFogf(GL_FOG_START, 4.81f);
    f->glFogf(GL_FOG_END, 5.01f);
    const float black[4] = {0, 0, 0, 1};
    f->glFogfv(GL_FOG_COLOR, black);
    f->glColor4d(0.4, 0.4, 0.4, 1);   // 1 + fBGBrightness, ignored under GL_REPLACE
    for (int c = 0; c < 4; ++c)
        drawQuads(m_bgStars[c], m_bgStar[c]);
    f->glColor4d(1, 1, 1, 1);
    f->glLoadIdentity();
    const double starOffsetY = offsetY * m_farW / 2;
    lookAt(f, 0, 0, 0.01, 0, -starOffsetY, -5.01, 0, 1, 0);
    for (int c = 0; c < 4; ++c)
        drawQuads(m_stars[c], m_star[c]);
    f->glDisable(GL_FOG);
    f->glLoadIdentity();
}

} // namespace TM
