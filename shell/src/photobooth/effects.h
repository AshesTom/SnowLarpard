// SPDX-License-Identifier: LGPL-2.1-or-later
// Photo Booth's effects. 10.6's are Quartz compositions of the system's repository
// (/System/Library/Compositions/*.qtz, image filters by protocol), named by EffectsOrdering.plist
// ("/sepia", "/blackandwhite"...) and rendered by a QCRenderer each. Their patches are Core Image
// filters with the parameters the compositions store, and a few kernels in Core Image's language:
// they are done here on the processor, with those parameters (effects.cpp says which filter's maths
// are the composition's own kernel and which stand for Core Image's).
#pragma once

#include "graphics.h"

#include <vector>

namespace PB {

constexpr int EffectCount = 16;   // EffectsOrdering.plist's first two pages; the backdrops are not done
constexpr int NormalEffect = -1;  // Normal.qtz: the picture as it is

struct EffectInfo {
    const char *identifier;   // EffectsOrdering.plist's
    const char *name;         // the composition's name attribute
    bool positioned;          // it takes the protocol's X and Y inputs (a click moves its centre)
};
const EffectInfo &effectInfo(int effect);

class Effects {
public:
    Effects();
    // `xy`: the composition's X and Y inputs, 0 to 1 from the picture's bottom left (0.5, 0.5 at
    // rest); `amount`: its master parameter (0.5: the compositions' own)
    AK::Image apply(int effect, const AK::Image &source, AK::Point xy = {0.5, 0.5}, double amount = 0.5) const;

private:
    struct Map {
        std::vector<float> r, g, b;
    };
    static Map loadMap(const char *file);
    Map m_thermal, m_xray;        // CIColorMap's gradient images
    int m_paperW = 0, m_paperH = 0;
    std::vector<float> m_paper;   // Color Pencil's paper, tiled and motion blurred
};

} // namespace PB
