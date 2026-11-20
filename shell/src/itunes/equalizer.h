// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's Equalizer (Window ▸ Equalizer, ⌥⌘2; EQWindow, docs/itunes-reverse.md section 9): a
// gradient utility window from EQWindow.nib, its On box, its presets' popup and its eleven sliders (the
// preamp and ten bands), the built-in presets from the table at 0xa2b520.
#pragma once

#include "appkit.h"

#include <array>
#include <memory>
#include <string>

namespace iTunes {

class Player;

class Equalizer {
public:
    explicit Equalizer(Player &player);
    ~Equalizer();
    void show();
    bool isVisible() const;
    // the settings kept (EQOn, EQPreset, EQBands) to the player: at launch
    void apply();

    // a built-in preset (0 to 21: Acoustic … Vocal Booster, STR# 600's order) by its values in
    // hundredths of a dB, the preamp first
    static constexpr int PresetCount = 22;
    static std::array<int, 11> preset(int index);
    static std::string presetName(int index);

private:
    class Content;
    void build();
    void setValue(int slider, int hundredths, bool manual);
    void choosePreset(int index);
    void save();

    Player &m_player;
    std::unique_ptr<AK::Window> m_window;
    Content *m_content = nullptr;
    bool m_on = false;
    int m_preset = 7;   // Flat; -1 Manual
    std::array<int, 11> m_values{};   // the preamp, then the ten bands (hundredths of a dB)
};

} // namespace iTunes
