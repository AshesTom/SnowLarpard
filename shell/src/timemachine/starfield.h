// SPDX-License-Identifier: LGPL-2.1-or-later
// Time Machine's backdrop, Backup.framework's MPStarField and VortexGL (docs/timemachine-reverse.md,
// sections 3 and 4): the stars drifting out of the galaxy, the background stars, the sun and the gas,
// 100 Cloud.png puffs flying out of the centre into a buffer twirled by RemapVortex (its GLSL read
// from the framework's Resources). Legacy OpenGL (2.1, fixed-function matrices) as the framework's.
#pragma once

#include <QImage>
#include <memory>
#include <vector>

class QOpenGLFunctions_2_1;
class QOpenGLFramebufferObject;
class QOpenGLShaderProgram;

namespace TM {

// GVInterpolator / the stack's curves: Numerical Recipes' natural cubic spline through the points
struct Spline {
    std::vector<double> x, y, y2;
    Spline() = default;
    Spline(std::initializer_list<std::pair<double, double>> points);
    double operator()(double t) const;
    bool empty() const { return x.empty(); }
};

// CAMediaTimingFunction's cubic Bézier from (0, 0) to (1, 1)
double bezier(double x1, double y1, double x2, double y2, double t);

// a texture of the current context from a picture (premultiplied RGBA)
unsigned uploadTexture(QOpenGLFunctions_2_1 *f, const QImage &image, bool mipmap = false);

class StarField {
public:
    explicit StarField(QOpenGLFunctions_2_1 *f);
    ~StarField();
    // the frame at `time` (seconds) in a viewport of w x h; galaxyY: globalYOffset, view points from
    // the bottom; zOffset: the stars' z offset (the flights move it)
    void draw(double time, int w, int h, double galaxyY, double zOffset);
    bool animated() const { return m_animated; }

private:
    struct Star {
        float x, y, z, s;
        float vx = 0, vy = 0, life = 0;
    };
    void setup(double aspect);
    void renderClouds(double time);
    void drawQuads(const std::vector<Star> &stars, unsigned texture);

    QOpenGLFunctions_2_1 *f;
    bool m_animated = false;
    double m_aspect = 0;
    double m_farW = 0, m_farH = 0, m_bgW = 0, m_bgH = 0, m_nearW = 0, m_nearH = 0;
    unsigned m_star[4] = {}, m_bgStar[4] = {}, m_sun = 0, m_cloud = 0, m_remap = 0, m_static = 0;
    std::vector<Star> m_stars[4], m_bgStars[4];
    std::unique_ptr<QOpenGLFramebufferObject> m_cloudBuffer, m_drawBuffer;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    double m_prevTime = -1, m_T = 1.0, m_lastBG = 0, m_prevZ = 0, m_prevOffsetY = 0;
    double m_r1[100] = {}, m_r2[100] = {};
    Spline m_A, m_W, m_I1, m_I2;
};

} // namespace TM
