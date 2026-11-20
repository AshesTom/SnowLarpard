// SPDX-License-Identifier: LGPL-2.1-or-later
// Magnetosphere as iTunes 8.2.1 runs it (FUN_0053fe00, a frame; docs/itunes-reverse.md section 16):
//   the sound (FUN_0054dada, FUN_0054de3a): the spectrum's 256 bins, the two channels averaged, their
//   least taken off; the level the bins' mean, each times the gain, at most 256; above three times
//   the threshold it is a beat (the threshold then the level, the kick 1, decaying × 0.975 a frame),
//   else the threshold eases to 10 (−0.15 of the difference a frame); the gain ±0.005 a frame toward a
//   level of 20 (the mean of the level's history), between 0.25 and 4.
//   The bands (FUN_0054e308) each sum their bins, their history's mean; a band beats past its
//   threshold and 16 − 2 × its number, a while after its last; the threshold decays × 0.99 after 25
//   quiet frames. A beat (FUN_0054dc56): band 0 or 1, one time in five, swings the camera; band 2
//   one in five swings it, one in two new colours; band 3 flips a flag of the particles' motion; band 4
//   long (opacity 1) or short (0.2) trails.
//   The camera (FUN_0054c430): a 60° field of view, near 10, far 5000, at a distance
//   (sin(phase) × 0.7 + 0.85) × its base, the base 300 to 500 and 5 a magnet (FUN_0054be34,
//   FUN_0054be68); the phase moving 0.006 a frame on beats; turned by two angles.
//   The background (FUN_00540758): its red, green and blue easing to their targets by 0.045, 0.035,
//   0.025 of the difference a frame.
//   The magnets (FUN_00542448, FUN_0054252c, FUN_00541250, FUN_00541834, FUN_005422fe, FUN_00541710):
//   3 at first (the scene 100 across, FUN_00009438), then 1 to 7 (FUN_00542702); a new one 1 from a
//   random other (the first in the middle), its radius growing by a tenth a frame to 25 to 100; a
//   pulse, its band's level × 3 + 0.1 (falling back by 0.35 a frame), makes its strength
//   (pulse × 0.25 + 10) × radius / its final radius; they push each other apart (each the other's
//   strength / d² × 10 × 0.25 along their difference), are pulled to the middle by 0.01 of their
//   place, their speed × 0.95 a frame and at most 2. One too many shrinks 30 to 50 frames and goes.
//   The particles (FUN_00547598, FUN_00545a02, FUN_0054a09a): 525 (FUN_000091da; 25 to 600), born in
//   a cube ±5 about the middle, still; each on a bin (its index % 128), its charge −(bin × 0.0666 + 0.5)
//   (FUN_00544bea). Their forces (FUN_005459a0): each magnet's (FUN_005415f8) along their difference
//   × charge / d² × strength / (strength / 10), turned about inside its sphere, all added, made 2 long;
//   the nearest magnet the one whose half radius is nearest; a noise field 0.5 long (FUN_00544ee8,
//   two angles from Perlin noise of x/100, y/100 and x/100, z/100), a swirl about the nearest magnet
//   sin²((100 + y) / 200 × 15.3) (FUN_00544d30); pushed apart two by two (FUN_0054784a), each by the
//   other's charge / d² × the push × 0.5 along their difference. Their motion (FUN_005453b4): the
//   speed × 0.975 a frame, the swirl, the field and the push in time, the magnets' at once, at most
//   6 to 8 a frame. Their trails 20 to 35 long, growing a point a frame (FUN_00544fc2), as wide as
//   sin((1 − i / (n − 1))⁴ × π) × 5 along them; their colours easing at 0.01 to 0.05 a frame to the
//   palette's (FUN_00544b2a); their energy falling by a tenth a frame.
//   The links (FUN_0054784a, FUN_0054779a, FUN_0054096a, FUN_00546f62): from a particle to its nearest
//   other 20 to 50 away when either's energy is past 12 and a random 0 to 50 is under the other's
//   less 12, at most 200; its ends bowed out of their magnets by up to 20 to 70,
//   growing over 15 to 25 frames (a tenth of the rest a frame), its opacity (1 − grown)² × 0.95;
//   drawn as three lines: a to b, b's bowed end to a, b's bowed end to a's.
// By eye: the spectrum (iTunes gets 512 bins from its player; here libmpv's RMS level, spectral
// centroid and flux, FFmpeg's astats and aspectralstats, make 128 bins: the level shared by the
// centroid, and the five bands from them); a particle's energy set to its bin + 3 when its bin passes
// its last peak; the magnets' number and the settings (the field, the swirl, the push) drawn again
// with new colours; the noise a smooth value noise, not Apple's Perlin table. Drawn by cairo: the
// particles glowing points added up, their trails one stroke each, not GL; the magnets not drawn.
#include "visualizer.h"

#include "resources.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

using namespace AK;

namespace iTunes {

namespace {

constexpr double SceneSize = 100;   // the magnets' container +0xC (FUN_00009438)
constexpr int ParticleCount = 525;  // FUN_000091da's +0x40
constexpr size_t MaxLinks = 200;    // FUN_0054779a

void hsv(double h, double s, double v, double &r, double &g, double &b)
{
    h = std::fmod(h, 1.0);
    if (h < 0)
        h += 1;
    const double i = std::floor(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (int(i) % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    default: r = v, g = p, b = q; break;
    }
}

double hash3(int x, int y, int z)
{
    unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u + unsigned(z) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return double(h ^ (h >> 16)) / 4294967295.0;
}

// a smooth value noise in 0..1 (FUN_0054bd60 is Perlin's)
double noise(double x, double y, double z)
{
    const int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
    auto s = [](double t) { return t * t * (3 - 2 * t); };
    const double fx = s(x - xi), fy = s(y - yi), fz = s(z - zi);
    double c[2][2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                c[i][j][k] = hash3(xi + i, yi + j, zi + k);
    auto l = [](double a, double b, double t) { return a + (b - a) * t; };
    return l(l(l(c[0][0][0], c[1][0][0], fx), l(c[0][1][0], c[1][1][0], fx), fy),
             l(l(c[0][0][1], c[1][0][1], fx), l(c[0][1][1], c[1][1][1], fx), fy), fz);
}

double length(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

} // namespace

Visualizer::Visualizer(Player &player) : m_player(player) { newScene(); }

Visualizer::~Visualizer()
{
    if (m_timer)
        App::shared().removeTimer(m_timer);
}

double Visualizer::rnd(double a, double b) { return std::uniform_real_distribution<double>(a, b)(m_rng); }

void Visualizer::mouseDown(Event &)
{
    if (onDismiss)
        onDismiss();
}

void Visualizer::setTrack(const std::string &name, const std::string &artist)
{
    if (name == m_trackName && artist == m_trackArtist)
        return;
    m_trackName = name;
    m_trackArtist = artist;
    m_infoUntil = m_time + 5 * 60;   // a new song: its name a few seconds (by eye)
}

// Magnetosphere's keys (5801.001–016)
void Visualizer::keyDown(Event &e)
{
    const char c = e.text.size() == 1 ? char(std::tolower(e.text[0])) : 0;
    switch (c) {
    case '?':
    case '/':
        m_help = !m_help;
        break;
    case 'm':   // Change mode: the next scene
        newScene();
        break;
    case 'p':   // Change palette
        newColours();
        break;
    case 'i':   // Display track info
        m_infoUntil = m_time + 5 * 60;
        break;
    case 'c':   // Toggle auto-cycle (on by default): the scenes changing with the music
        m_autoCycle = !m_autoCycle;
        break;
    case 'f':   // Toggle freeze mode: nothing new, scenes nor colours
        m_frozen = !m_frozen;
        break;
    case 'n':   // Toggle nebula mode: the noise field (by eye)
        m_field = !m_field;
        m_push = m_field ? 0.2 : 1;
        break;
    case 'l':   // Toggle camera lock
        m_cameraLock = !m_cameraLock;
        break;
    default:
        if (onDismiss)
            onDismiss();
        else
            View::keyDown(e);
        return;
    }
    setNeedsDisplay();
}

void Visualizer::setRunning(bool on)
{
    m_player.setAnalysis(on);
    if (on && !m_timer)
        m_timer = App::shared().addTimer(1.0 / 30, true, [this] { step(); });
    else if (!on && m_timer) {
        App::shared().removeTimer(m_timer);
        m_timer = 0;
    }
}

// FUN_0054082a: the palette's new colours, for the particles to ease to
void Visualizer::newColours()
{
    m_hue = rnd(0, 1);
    double r, g, b;
    hsv(m_hue + 0.5, 0.6, 0.12 + 0.1 * rnd(0, 1), r, g, b);
    m_bgTo[0] = r * 255;
    m_bgTo[1] = g * 255;
    m_bgTo[2] = b * 255;
    for (Particle &p : m_particles)
        hsv(m_hue + rnd(-0.08, 0.08), rnd(0.3, 0.8), 1, p.rgbTo[0], p.rgbTo[1], p.rgbTo[2]);
}

// FUN_00543d80 (by eye when): the settings drawn again, the magnets' number, the camera's distance
void Visualizer::newScene()
{
    m_field = rnd(0, 1) < 0.34;
    m_push = m_field ? 0.2 : 1;
    m_swirlOn = rnd(0, 1) < 0.34;
    m_magnetsWanted = std::uniform_int_distribution<int>(1, 7)(m_rng);
    m_cameraBase = rnd(300, 500) + 5 * m_magnetsWanted;
    newColours();
}

void Visualizer::bandBeat(int band)
{
    std::uniform_int_distribution<int> five(0, 4), two(0, 1);
    std::uniform_real_distribution<double> angle(-M_PI, M_PI);
    switch (band) {
    case 0:
    case 1:
        if (five(m_rng) == 0 && !m_cameraLock) {
            if (band == 0)
                m_yawTo += angle(m_rng);
            else
                m_pitchTo = angle(m_rng) / 3;
        }
        break;
    case 2:
        if (five(m_rng) == 0 && !m_cameraLock)
            m_yawTo += angle(m_rng) / 2;
        if (two(m_rng) == 0 && m_autoCycle && !m_frozen)
            newScene();
        break;
    case 3:
        if (!m_frozen)
            m_swirlOn = !m_swirlOn;
        break;
    case 4:
        m_longTrails = two(m_rng) == 0;
        break;
    }
}

// FUN_0054db32: a bin, times the gain, at most 256
double Visualizer::bin(int i) const { return m_bins[std::clamp(i, 0, 127)]; }

// FUN_0054de3a and FUN_0054e308 on libmpv's measures
void Visualizer::listen(double dt)
{
    const Player::Levels l = m_player.levels();
    const double amp = l.rms > -90 ? std::pow(10, l.rms / 20) : 0;
    m_level = std::clamp(amp * 256 * m_gain, 0.0, 256.0);
    // the gain toward a mean level of 20
    m_history.push_back(m_level);
    if (m_history.size() > 60)
        m_history.erase(m_history.begin());
    m_average = std::accumulate(m_history.begin(), m_history.end(), 0.0) / m_history.size();
    if (m_player.isPlaying())
        m_gain = std::clamp(m_gain + (m_average < 20 ? 0.005 : -0.005), 0.25, 4.0);
    if (m_level > m_threshold * 3) {
        m_threshold = m_level;
        m_kick = 1;
    } else {
        m_threshold += (m_threshold - 10) * -0.15 * dt;
        m_kick += m_kick * -0.025 * dt;
    }
    // 128 bins from the level shared about the spectrum's centroid, their mean the level
    const double c = std::clamp(l.centroid / 8000.0, 0.0, 1.0);
    double shape[128], sum = 0;
    for (int i = 0; i < 128; ++i) {
        const double f = i / 127.0, d = f - c;
        shape[i] = (0.15 + std::exp(-d * d / 0.02)) * std::exp(-2 * f) * (1 + l.flux * 4 * (rnd(0, 1) - 0.5));
        sum += shape[i];
    }
    for (int i = 0; i < 128; ++i)
        m_bins[i] = sum > 0 ? std::clamp(m_level * shape[i] * 128 / sum, 0.0, 256.0) : 0;
    // the five bands
    for (int i = 0; i < 5; ++i) {
        double sum = 0;
        for (int k = i * 25; k < i * 25 + 25; ++k)
            sum += m_bins[k];
        m_bandValue[i] = sum / 25 * (1 + 4 * l.flux);
        ++m_bandFrames[i];
        if (m_bandValue[i] > m_bandThreshold[i] && m_bandValue[i] > 16 - 2 * i && m_bandFrames[i] > 8) {
            m_bandThreshold[i] = m_bandValue[i];
            m_bandFrames[i] = 0;
            bandBeat(i);
        } else if (m_bandFrames[i] > 25) {
            m_bandThreshold[i] *= 0.99;
        }
    }
}

// FUN_0054252c and FUN_00542448
void Visualizer::stepMagnets(double dt)
{
    const int alive = int(std::count_if(m_magnets.begin(), m_magnets.end(), [](const Magnet &m) { return !m.dying; }));
    if (alive < m_magnetsWanted) {
        Magnet m;
        if (!m_magnets.empty()) {
            const Magnet &o = m_magnets[std::uniform_int_distribution<size_t>(0, m_magnets.size() - 1)(m_rng)];
            m.p = {o.p.x + rnd(-1, 1), o.p.y + rnd(-1, 1), o.p.z + rnd(-1, 1)};
        }
        m.targetRadius = rnd(SceneSize * 0.25, SceneSize);
        m.radius = m.targetRadius * 0.1;
        m_magnets.push_back(m);
    } else if (alive > m_magnetsWanted) {
        for (auto it = m_magnets.rbegin(); it != m_magnets.rend(); ++it)
            if (!it->dying) {
                it->dying = true;
                it->dyingFrames = std::uniform_int_distribution<int>(30, 50)(m_rng);
                break;
            }
    }
    for (size_t i = 0; i < m_magnets.size(); ++i) {
        Magnet &m = m_magnets[i];
        // FUN_00541834: the pulse, the strength, the radius
        const double level = m_bandValue[i % 3 + 1] / 64;
        const double pulse = level * 3 + 0.1;
        m.pulse = pulse > m.pulse ? pulse : m.pulse + (m.pulse - 0.1) * -0.35 * dt;
        m.strength = (m.pulse * 0.25 + 10) * (m.radius / m.targetRadius);
        m.div = m.strength / 10;
        if (m.dying) {
            --m.dyingFrames;
            m.targetRadius += m.targetRadius * -0.1 * dt;
        }
        m.radius += dt * (m.radius - m.targetRadius) * -0.1;
        m.acc = {};
    }
    // FUN_005422fe: apart
    for (size_t i = 0; i < m_magnets.size(); ++i)
        for (size_t j = i + 1; j < m_magnets.size(); ++j) {
            Magnet &a = m_magnets[i], &b = m_magnets[j];
            double dx = a.p.x - b.p.x, dy = a.p.y - b.p.y, dz = a.p.z - b.p.z;
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 1) {
                const double s = b.strength / d2 * (a.strength / a.div) * 0.25;
                dx *= s, dy *= s, dz *= s;
            }
            a.acc.x += dx, a.acc.y += dy, a.acc.z += dz;
            b.acc.x -= dx, b.acc.y -= dy, b.acc.z -= dz;
        }
    // FUN_005411ee, FUN_00541710: to the middle, the motion
    const double damp = std::pow(0.95, dt);
    for (Magnet &m : m_magnets) {
        m.v.x = (m.v.x + dt * (m.acc.x - 0.01 * m.p.x)) * damp;
        m.v.y = (m.v.y + dt * (m.acc.y - 0.01 * m.p.y)) * damp;
        m.v.z = (m.v.z + dt * (m.acc.z - 0.01 * m.p.z)) * damp;
        const double s = length(m.v.x, m.v.y, m.v.z), most = 2 * dt;
        if (s > most)
            m.v.x *= most / s, m.v.y *= most / s, m.v.z *= most / s;
        m.p.x += dt * m.v.x, m.p.y += dt * m.v.y, m.p.z += dt * m.v.z;
    }
    std::vector<int> map(m_magnets.size());
    int k = 0;
    for (size_t i = 0; i < m_magnets.size(); ++i)
        map[i] = m_magnets[i].dying && m_magnets[i].dyingFrames <= 0 ? -1 : k++;
    if (k != int(m_magnets.size())) {
        for (Particle &p : m_particles)
            p.magnet = p.magnet >= 0 ? map[p.magnet] : -1;
        m_magnets.erase(std::remove_if(m_magnets.begin(), m_magnets.end(),
                                       [](const Magnet &m) { return m.dying && m.dyingFrames <= 0; }),
                        m_magnets.end());
    }
}

// FUN_0054a09a
void Visualizer::stepParticles(double dt)
{
    // FUN_00547598: one more or one less a frame
    if (m_particles.size() < size_t(ParticleCount)) {
        Particle p;
        p.bin = int(m_particles.size() % 128);
        p.p = {rnd(-5, 5), rnd(-5, 5), rnd(-5, 5)};
        p.maxSpeed = rnd(6, 8);
        p.trailMax = std::uniform_int_distribution<int>(20, 35)(m_rng);
        p.trail.assign(size_t(p.trailMax), p.p);
        p.ease = rnd(0.01, 0.05);
        hsv(m_hue + rnd(-0.08, 0.08), rnd(0.3, 0.8), 1, p.rgbTo[0], p.rgbTo[1], p.rgbTo[2]);
        std::copy(p.rgbTo, p.rgbTo + 3, p.rgb);
        m_particles.push_back(std::move(p));
    }
    const size_t n = m_particles.size();
    // FUN_005459a0: the magnets', the field, the swirl
    for (Particle &a : m_particles) {
        a.magnetForce = a.pairForce = a.field = a.swirl = {};
        a.magnet = -1;
        double nearest = 100000;
        for (size_t k = 0; k < m_magnets.size(); ++k) {
            const Magnet &m = m_magnets[k];
            double dx = a.p.x - m.p.x, dy = a.p.y - m.p.y, dz = a.p.z - m.p.z;
            const double d = length(dx, dy, dz);
            if (d != 0) {
                double s = a.charge / (d * d) * m.strength / (m.div + 0.0001);
                if (m.radius - d > 0)
                    s = -s;
                a.magnetForce.x += dx * s * a.invCharge;
                a.magnetForce.y += dy * s * a.invCharge;
                a.magnetForce.z += dz * s * a.invCharge;
            }
            if (d - m.radius * 0.5 < nearest) {
                nearest = d - m.radius * 0.5;
                a.magnet = int(k);
            }
        }
        const double f = length(a.magnetForce.x, a.magnetForce.y, a.magnetForce.z);
        if (f > 0)
            a.magnetForce = {a.magnetForce.x / f * 2, a.magnetForce.y / f * 2, a.magnetForce.z / f * 2};
        if (m_field) {
            const double t = m_time / 100;
            const double a1 = noise(a.p.x / 100, a.p.y / 100, t) * 2 * M_PI;
            const double a2 = noise(a.p.x / 100, a.p.z / 100, t + 7) * 2 * M_PI;
            a.field = {std::cos(a1) * 0.5, -std::sin(a2) * 0.5, std::sin(a1) * 0.5};
        }
        if (m_swirlOn && a.magnet >= 0) {
            const Magnet &m = m_magnets[size_t(a.magnet)];
            const double s = std::sin((SceneSize + a.p.y) / (2 * SceneSize) * 15.3);
            const double w = s * -s, ang = std::atan2(a.p.z - m.p.z, a.p.x - m.p.x) + M_PI / 2;
            a.swirl = {w * std::cos(ang), 0, w * std::sin(ang)};
        }
    }
    // FUN_0054784a: apart, two by two; the links to the nearest
    for (size_t i = 0; i < n; ++i) {
        Particle &a = m_particles[i];
        double best = 10000;
        size_t bestJ = n;
        for (size_t j = i + 1; j < n; ++j) {
            Particle &b = m_particles[j];
            double dx = a.p.x - b.p.x, dy = a.p.y - b.p.y, dz = a.p.z - b.p.z;
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best)
                best = d2, bestJ = j;
            if (d2 > 1) {
                const double s = b.charge / d2 * -1 * m_push * 0.5;
                dx *= s, dy *= s, dz *= s;
            }
            a.pairForce.x += dx, a.pairForce.y += dy, a.pairForce.z += dz;
            b.pairForce.x -= dx, b.pairForce.y -= dy, b.pairForce.z -= dz;
        }
        if (bestJ < n && best < 2500 && best > 400 && m_links.size() < MaxLinks) {
            const Particle &b = m_particles[bestJ];
            if ((b.energy > 12 || a.energy > 12) && rnd(0, 50) < b.energy - 12) {
                const bool have = std::any_of(m_links.begin(), m_links.end(), [&](const Link &l) {
                    return (l.a == int(i) && l.b == int(bestJ)) || (l.b == int(i) && l.a == int(bestJ));
                });
                if (!have) {
                    Link l;
                    l.a = int(bestJ), l.b = int(i);
                    l.bulge = rnd(20, 70);
                    l.frames = std::uniform_int_distribution<int>(15, 25)(m_rng);
                    m_links.push_back(l);
                }
            }
        }
    }
    // FUN_005453b4, FUN_00545054, FUN_00544b2a, FUN_00544bea, FUN_00544fc2
    const double damp = std::pow(0.975, dt);
    for (Particle &a : m_particles) {
        a.v.x = a.v.x * damp + dt * (a.swirl.x + a.field.x + a.pairForce.x) + a.magnetForce.x;
        a.v.y = a.v.y * damp + dt * (a.swirl.y + a.field.y + a.pairForce.y) + a.magnetForce.y;
        a.v.z = a.v.z * damp + dt * (a.swirl.z + a.field.z + a.pairForce.z) + a.magnetForce.z;
        const double s = length(a.v.x, a.v.y, a.v.z), most = dt * a.maxSpeed;
        if (s > most)
            a.v.x *= most / s, a.v.y *= most / s, a.v.z *= most / s;
        a.p.x += dt * a.v.x, a.p.y += dt * a.v.y, a.p.z += dt * a.v.z;
        a.trail.insert(a.trail.begin(), a.p);
        a.trail.pop_back();
        for (int c = 0; c < 3; ++c)
            a.rgb[c] -= (a.rgb[c] - a.rgbTo[c]) * a.ease * dt;
        a.value = bin(a.bin);
        a.value15 = bin(a.bin % 15);
        a.width = (a.value / 15 + a.value15 / 15) * 0.5;
        if (a.value > a.peak) {
            a.peak = a.value;
            a.energy = a.value + 3;
        } else {
            a.peak += (a.peak - 1) * -0.15 * dt;
        }
        a.charge = -(a.value * 0.0666 + 0.5);
        a.invCharge = 1 / std::abs(a.charge);
        a.energy += a.energy * -0.1 * dt;
        if (a.trailLen < a.trailMax)
            ++a.trailLen;
    }
}

// FUN_0054096a: the links grow and bow out of their magnets
void Visualizer::stepLinks()
{
    for (Link &l : m_links) {
        l.grown += (l.grown - l.frames) * -0.1;
        if (l.grown > l.frames - 1)
            l.done = true;
        l.t = l.grown / l.frames;
        l.shown = false;
        if (l.a >= int(m_particles.size()) || l.b >= int(m_particles.size())) {
            l.done = true;
            continue;
        }
        const Particle &a = m_particles[size_t(l.a)], &b = m_particles[size_t(l.b)];
        if (a.magnet < 0 || b.magnet < 0)
            continue;
        const Magnet &ma = m_magnets[size_t(a.magnet)], &mb = m_magnets[size_t(b.magnet)];
        auto out = [](const Vec &p, const Vec &c) {
            const double dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z, d = length(dx, dy, dz);
            return d > 0 ? Vec{dx / d, dy / d, dz / d} : Vec{};
        };
        const Vec na = out(a.p, ma.p), nb = out(b.p, mb.p);
        const double s = l.t * l.bulge;
        l.pa = a.p, l.pb = b.p;
        l.ca = {a.p.x + na.x * s, a.p.y + na.y * s, a.p.z + na.z * s};
        l.cb = {b.p.x + nb.x * s, b.p.y + nb.y * s, b.p.z + nb.z * s};
        l.shown = true;
    }
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [](const Link &l) { return l.done; }), m_links.end());
}

void Visualizer::step()
{
    const double dt = 2;   // iTunes' frames at 60 a second, these at 30
    m_time += dt;
    listen(dt);
    stepMagnets(dt);
    stepParticles(dt);
    stepLinks();
    // the camera: its phase on beats, its turns easing to theirs
    if (m_kick > 0.1)
        m_phase += 0.006 * dt * 5;
    m_yaw += (m_yawTo - m_yaw) * 0.02 * dt + (m_cameraLock ? 0 : 0.002 * dt);
    m_pitch += (m_pitchTo - m_pitch) * 0.02 * dt;
    // the background
    static const double ease[3] = {0.045, 0.035, 0.025};
    for (int i = 0; i < 3; ++i)
        m_bg[i] += (m_bg[i] - m_bgTo[i]) * -ease[i] * dt;
    setNeedsDisplay();
}

void Visualizer::draw(cairo_t *cr)
{
    cairo_set_source_rgb(cr, m_bg[0] / 255, m_bg[1] / 255, m_bg[2] / 255);
    cairo_paint(cr);
    const double W = frame().w, H = frame().h;
    // the camera: 60° vertically, at (sin(phase) × 0.7 + 0.85) × its base
    const double f = (H / 2) / std::tan(M_PI / 6);
    const double dist = (std::sin(m_phase) * 0.7 + 0.85) * m_cameraBase;
    const double cy = std::cos(m_yaw), sy = std::sin(m_yaw), cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    auto project = [&](const Vec &v, double &sx, double &sy2, double &depth) {
        const double x = v.x * cy - v.z * sy, z0 = v.x * sy + v.z * cy;
        const double y = v.y * cp - z0 * sp, z = v.y * sp + z0 * cp + dist;
        depth = z;
        if (z < 10)
            return false;
        sx = W / 2 + x * f / z;
        sy2 = H / 2 - y * f / z;
        return true;
    };
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    const double trailAlpha = m_longTrails ? 1.0 : 0.2;
    for (const Particle &p : m_particles) {
        // the trail: its points so far, as wide as sin((1 − i / (n − 1))⁴ × π) × 5 at its middle
        const size_t len = std::min(size_t(p.trailLen), p.trail.size());
        double x, y, d;
        if (len > 2 && project(p.trail[0], x, y, d)) {
            cairo_move_to(cr, x, y);
            bool ok = true;
            for (size_t i = 1; i < len && ok; ++i) {
                ok = project(p.trail[i], x, y, d);
                if (ok)
                    cairo_line_to(cr, x, y);
            }
            cairo_set_source_rgba(cr, p.rgb[0], p.rgb[1], p.rgb[2], trailAlpha * 0.3);
            cairo_set_line_width(cr, std::max(0.5, 5 * f / d * 0.15));
            cairo_stroke(cr);
            cairo_new_path(cr);
        }
        // the particle, a glow as wide as its bins
        if (!project(p.p, x, y, d))
            continue;
        const double rad = std::clamp((2 + p.width * 2) * f / d, 1.0, 40.0);
        cairo_pattern_t *g = cairo_pattern_create_radial(x, y, 0, x, y, rad);
        cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.9);
        cairo_pattern_add_color_stop_rgba(g, 0.2, p.rgb[0], p.rgb[1], p.rgb[2], 0.6);
        cairo_pattern_add_color_stop_rgba(g, 1, p.rgb[0], p.rgb[1], p.rgb[2], 0);
        cairo_set_source(cr, g);
        cairo_arc(cr, x, y, rad, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }
    // the links (FUN_00546f62, GL_LINES): a to b, b's bowed end to a, b's bowed end to a's, each end
    // in its particle's colour, (1 − grown)² × 0.95 opaque
    cairo_set_line_width(cr, 1);
    for (const Link &l : m_links) {
        if (!l.shown)
            continue;
        const Particle &a = m_particles[size_t(l.a)], &b = m_particles[size_t(l.b)];
        const double alpha = (1 - l.t) * (1 - l.t) * 0.95;
        auto line = [&](const Vec &u, const Particle &pu, const Vec &v, const Particle &pv) {
            double x0, y0, d0, x1, y1, d1;
            if (!project(u, x0, y0, d0) || !project(v, x1, y1, d1))
                return;
            cairo_pattern_t *g = cairo_pattern_create_linear(x0, y0, x1, y1);
            cairo_pattern_add_color_stop_rgba(g, 0, pu.rgb[0], pu.rgb[1], pu.rgb[2], alpha);
            cairo_pattern_add_color_stop_rgba(g, 1, pv.rgb[0], pv.rgb[1], pv.rgb[2], alpha);
            cairo_set_source(cr, g);
            cairo_move_to(cr, x0, y0);
            cairo_line_to(cr, x1, y1);
            cairo_stroke(cr);
            cairo_pattern_destroy(g);
        };
        line(l.pa, a, l.pb, b);
        line(l.cb, b, l.pa, a);
        line(l.cb, b, l.ca, a);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    AK::TextStyle t;
    t.color = Color::gray(230);
    // I: the song's name and artist at the bottom left (by eye)
    if (m_time < m_infoUntil && !m_trackName.empty()) {
        t.font = Font::boldSystem(18);
        drawText(cr, m_trackName, Rect{20, H - 58, W - 40, 24}, t);
        t.font = Font::system(14);
        drawText(cr, m_trackArtist, Rect{20, H - 32, W - 40, 20}, t);
    }
    // ?: the keys, as 5801's pairs
    if (m_help) {
        t.font = Font::system(13);
        double y = 20;
        for (int i = 1; i < 16; i += 2) {
            char key[16], what[16];
            std::snprintf(key, sizeof key, "5801.%03d", i);
            std::snprintf(what, sizeof what, "5801.%03d", i + 1);
            drawText(cr, tr(key), Rect{20, y, 30, 18}, t);
            drawText(cr, tr(what), Rect{50, y, W - 70, 18}, t);
            y += 20;
        }
    }
}

} // namespace iTunes
