// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's Equalizer (Window ▸ Equalizer, ⌥⌘2; EQWindow, docs/itunes-reverse.md section 9): a
// gradient utility window from EQWindow.nib, its On box, its presets' popup and its eleven sliders (the
// preamp and ten bands), the built-in presets from the table at 0xa2b520; the presets' list kept by name
// (Make Preset…, Edit List…: DLOG 148 with Rename… and Delete).
#pragma once

#include "appkit.h"
#include "controls.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    struct Preset {
        std::string name;
        std::array<int, 11> values;
    };
    void build();
    void setValue(int slider, int hundredths, bool manual);
    void choosePreset(int index);
    void save();
    void savePresets();
    void updateTitle();
    int find(const std::string &name) const;   // a preset by its name, the case ignored; -1
    void insert(const Preset &p);              // in the name's order (FUN_000a5d60)
    void makePreset();
    void editList();
    void renamePreset(int index);
    void deletePreset(int index);
    void getString(const std::string &title, const std::string &text, std::function<void(std::string)> done);
    void refreshList();

    std::vector<Preset> m_presets;
    std::unique_ptr<AK::Window> m_editWindow, m_askWindow;
    AK::TableView *m_editList = nullptr;
    AK::Button *m_rename = nullptr, *m_delete = nullptr;

    Player &m_player;
    std::unique_ptr<AK::Window> m_window;
    Content *m_content = nullptr;
    bool m_on = false;
    int m_preset = 7;   // m_presets' (Flat at first); -1 Manual
    std::array<int, 11> m_values{};   // the preamp, then the ten bands (hundredths of a dB)
};

} // namespace iTunes
