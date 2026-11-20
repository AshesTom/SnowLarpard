// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's visualizer (View ▸ Show Visualizer, ⌘T): Magnetosphere (MagnetosphereApplet, its scene
// at 0x53f000–0x54e000 drawn by RendererOGL; docs/itunes-reverse.md section 16): charged particles
// held on the spheres of a few magnets, their trails and the links between them, a camera swinging
// about them, all moved by the sound.
#pragma once

#include "appkit.h"
#include "player.h"

#include <functional>
#include <random>
#include <string>
#include <vector>

namespace iTunes {

class Visualizer : public AK::View {
public:
    explicit Visualizer(Player &player);
    ~Visualizer() override;
    void setRunning(bool on);   // the frames and the sound's analysis
    void draw(cairo_t *cr) override;
    // full screen: a click or a key ends it
    std::function<void()> onDismiss;
    void mouseDown(AK::Event &) override;
    void keyDown(AK::Event &) override;
    bool acceptsFirstResponder() const override { return true; }
    // the song shown by I (Display track info) and when a song begins
    void setTrack(const std::string &name, const std::string &artist);

private:
    struct Vec {
        double x = 0, y = 0, z = 0;
    };
    // a magnet (records of 0xDC bytes, FUN_00541250)
    struct Magnet {
        Vec p, v, acc;
        double strength = 10, div = 1;        // +0x64, +0x68
        double radius = 0, targetRadius = 0;  // +0xAC, +0xB8
        double pulse = 0.1;                   // +0xB4
        bool dying = false;                   // +0xD1
        int dyingFrames = 0;                  // +0xD4
    };
    // a particle (records of 0x5D4 bytes, FUN_00545a02)
    struct Particle {
        int bin = 0;                          // +0x00: its spectrum bin, its index % 128
        int trailMax = 20;                    // +0x04: 20 to 35
        int trailLen = 0;                     // +0x08
        double charge = -1, invCharge = 1;    // +0x0C, +0x10 (its sign at +0x14 always −1)
        Vec magnetForce, pairForce, field, swirl;   // +0x18, +0x24, +0x30, +0x3C
        int magnet = -1;                      // +0x60: the nearest magnet
        Vec p, v;                             // +0x64, +0x70
        std::vector<Vec> trail;               // +0x88
        double width = 0;                     // +0x45C
        double rgb[3] = {1, 1, 1}, rgbTo[3] = {1, 1, 1}, ease = 0.03;   // +0x584, +0x594, +0x5A0
        double maxSpeed = 7;                  // +0x5A4
        double value = 0, peak = 0, value15 = 0;    // +0x5A8, +0x5AC, +0x5B0
        double energy = 0;                    // +0x5B8
    };
    // a link between two particles (records of 100 bytes, FUN_005408bc)
    struct Link {
        int a = 0, b = 0;
        double bulge = 20;                    // +0x50: 20 to 70
        double grown = 3;                     // +0x54
        int frames = 15;                      // +0x5C: 15 to 25
        double t = 0;                         // +0x58
        bool done = false;                    // +0x60
        Vec pa, pb, ca, cb;                   // its two ends and their bowed ends
        bool shown = false;
    };
    void step();
    void listen(double dt);
    void bandBeat(int band);
    void newColours();
    void newScene();
    void stepMagnets(double dt);
    void stepParticles(double dt);
    void stepLinks();
    double bin(int i) const;
    double rnd(double a, double b);

    Player &m_player;
    int m_timer = 0;
    std::mt19937 m_rng{std::random_device{}()};
    std::vector<Magnet> m_magnets;
    int m_magnetsWanted = 3;   // FUN_00009438: 3; FUN_00542702: 1 to 7
    std::vector<Particle> m_particles;
    std::vector<Link> m_links;
    double m_time = 0;
    // the scene's settings (the global at 0xC0E700, FUN_00543d80): the noise field (34%, then the
    // particles' push 0.2, else 1), the swirl (34%)
    bool m_field = false, m_swirlOn = false;
    double m_push = 1;
    // the sound (FUN_0054de3a): the gain (0.25 to 4, aiming at 20), the level, its threshold, the kick
    double m_gain = 1, m_level = 0, m_threshold = 10, m_kick = 0, m_average = 0;
    std::vector<double> m_history;
    double m_bins[128] = {};
    // the five bands (FUN_0054e308): their thresholds, frames since their last beat
    double m_bandThreshold[5] = {}, m_bandValue[5] = {};
    int m_bandFrames[5] = {};
    // the camera (FUN_0054c430): its phase, its two turns and where they go, its distance
    double m_phase = 0, m_yaw = 0, m_pitch = 0, m_yawTo = 0, m_pitchTo = 0, m_cameraBase = 400;
    // the background (FUN_00540758) and its target; the particles' hue; long trails
    double m_bg[3] = {0, 0, 0}, m_bgTo[3] = {0, 0, 0}, m_hue = 0.6;
    bool m_longTrails = true;
    // the keys (Localizable.strings 5801): ? help, M mode, P palette, I track info, C auto-cycle (on at
    // first), F freeze, N nebula, L camera lock
    bool m_help = false, m_autoCycle = true, m_frozen = false, m_cameraLock = false;
    std::string m_trackName, m_trackArtist;
    double m_infoUntil = 0;   // m_time until which the song's name is shown
};

} // namespace iTunes
