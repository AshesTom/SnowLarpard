// SPDX-License-Identifier: LGPL-2.1-or-later
// BatteryExtra: SystemUIServer's own (Battery.menu's code isn't used: the class is found in
// SystemUIServer first), reversed from 10.6.8:
//
//   - -createMenu (0x100035647): the status item, the power source, a line, Show ▸ Icon Only, Time,
//     Percentage (tags 3, 1, 2), a line, Energy Saver Preferences…;
//   - -updateMenu (0x100036659), -_statusMenuItemTitleWithSourceState: (0x1000344e5): "Battery Is
//     Charged", "Calculating Time Remaining…", "2:15 Remaining", "Battery Is Not Charging"; with Time
//     in the bar, the menu tells the percentage ("85% Remaining", "85% Full"), and the other way round;
//   - -_menuBarStringWithSourceState: (0x100034a4b): "(2:15)", "(85%)", "Calculating…", "Charged",
//     "Not Charging" beside the icon when Show says so;
//   - BatteryImageCreator -singleBatteryImageForBattery:partialOpacity: (0x100039f43): BatteryEmpty.pdf,
//     22 x 12, with the juice (BatteryLevelCap pieces, 3 points in, as wide as 13 points full, red
//     pieces at the warning level) or the glyph charging or charged centred; a template unless red;
//   - BatteryViewInMenu -drawRect: (0x10003854b): the picture from x 1, the text 2 points after it.
//
// IOPowerSources on Linux: /sys/class/power_supply. Its preferences (com.apple.menuextra.battery):
// ~/.config/snowleopard/menuextra.battery.conf, ShowTime, ShowPercent.
#pragma once

#include "systemuiplugin.h"

class BatteryExtra : public SystemUI::MenuExtra {
public:
    explicit BatteryExtra(const std::string &bundle);
    ~BatteryExtra() override;
    std::vector<AK::MenuItem> menu() override;
    void willUnload() override;

    struct State {
        bool present = false, plugged = false, charging = false, charged = false, calculating = false;
        int minutes = -1;      // to empty, or to full while charging
        double percent = 0;
        bool warning = false;  // the low battery warning level: the juice red
    };
    const State &state() const { return m_state; }
    AK::Image picture(bool white) const;
    std::string barText() const;   // -_menuBarStringWithSourceState:

private:
    void update();
    std::string statusTitle() const;
    std::string timeString(int minutes) const;
    State m_state;
    bool m_showTime = false, m_showPercent = false;
    int m_timer = 0;
};
