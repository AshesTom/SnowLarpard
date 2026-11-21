// SPDX-License-Identifier: LGPL-2.1-or-later
// Photo Booth's backdrops (pages 3 and 4 of the effects): Backdrops.qtz and the Backdrops Quartz
// Composer plug-in (/System/Library/Graphics/Quartz Composer Patches/Backdrops.plugin), reversed from
// its x86_64 code. The camera's picture at half its size (BackdropsDownsamplerPatch) in Y'CbCr
// (Backdrops_601); BackdropsAccumulatorPatch learns the background from the last eight frames once
// it stays still; BackdropsSegmentationPatch tells each pixel apart from it (a Markov random field
// relaxed by its neighbours, small components flipped); BackdropsMattingPatch makes the band inside
// the edge soft; the upsampler's joint bilateral kernel and the composite kernel (Backdrops.qtz's
// own, as written) put the person over the backdrop.
#pragma once

#include "graphics.h"

#include <array>
#include <deque>
#include <vector>

namespace PB {

class Backdrops {
public:
    enum State { Idle, Capturing, Ready };
    State state() const { return m_state; }
    // -[RendererController startBackdrops]: a new background to learn
    void start();
    void stop() { m_state = Idle; }
    // A frame of the camera (as shown, 640 × 480): learnt while capturing (true when the background
    // was just found), else cut out over `backdrop`
    bool learn(const AK::Image &frame, double time);
    AK::Image composite(const AK::Image &frame, const AK::Image &backdrop);
    // BackdropsAccumulatorPatch's outputBackgroundMotion: the moving pixels (shown red when the
    // capture takes too long)
    AK::Image motionOverlay(const AK::Image &frame) const;

private:
    struct YCC {
        float y, cb, cr;
    };
    void downsample(const AK::Image &frame, std::vector<YCC> &out) const;
    void segment(const std::vector<YCC> &image);
    void icm(float constant, int iterations);
    void connectedComponents();
    void matte(const std::vector<YCC> &image);

    State m_state = Idle;
    int m_w = 0, m_h = 0;
    std::deque<std::vector<YCC>> m_frames;     // the accumulator's eight
    std::vector<unsigned char> m_motion;
    bool m_stable = false;
    double m_stableSince = 0;
    std::vector<YCC> m_background;
    std::array<float, 3> m_noise{};             // y, cb, cr
    std::vector<float> m_test;
    std::vector<unsigned char> m_mask;          // 1: the person
    std::vector<float> m_alpha;                 // 0 to 1, at half size
    std::vector<std::array<float, 3>> m_fg;     // the foreground's colours (RGB), at half size
};

} // namespace PB
