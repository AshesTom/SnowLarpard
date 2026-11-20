// SPDX-License-Identifier: LGPL-2.1-or-later
// AppleClockExtra: SystemUIServer's own clock (Clock.menu has its resources only), reversed from
// SystemUIServer 10.6.8:
//
//   - CLKDigitalView (-initWithFrame: 0x1000066d2): an NSTextFieldCell of systemFontOfSize:14, its
//     text the date formatter's (the DateFormat preference, "EEE H:mm:ss" until it is read); its frame
//     the cell's size, which it gives the extra (-_resize 0x100006f82, -setLength:); the cell one point
//     down (-cellFrame); every second when the format has seconds or the separators flash
//     (-_setUpdateTimeInterval), the separators hidden on even seconds (-_updateWithDate:);
//   - CLKAnalogView: 22 points (-initialLength: the menu bar's height), Clock.pdf as a template, the hour
//     hand height / 5.5 long and 1 wide, the minute hand height / 4 and 0.75 wide, from the centre
//     (width / 2 - 0.5, height / 2 + 0.5) (-drawRect: 0x10003dc61);
//   - -_buildMenu (0x10003c4be): the full date (NSDateFormatterFullStyle; with the time when analog,
//     -_dateFormatterForMenu), a line, Parental Controls' items (hidden without them), View as Analog
//     or View as Digital, a line, Date & Time Preferences… (-_menuActionOpenPrefPane:).
//
// Its preferences (com.apple.menuextra.clock): ~/.config/snowleopard/menuextra.clock.conf, DateFormat,
// IsAnalog, FlashDateSeparators.
#pragma once

#include "systemuiplugin.h"

class AppleClockExtra : public SystemUI::MenuExtra {
public:
    explicit AppleClockExtra(const std::string &bundle);
    ~AppleClockExtra() override;
    std::vector<AK::MenuItem> menu() override;
    void willUnload() override;

    // the digital clock's text now (its formatter's, separators hidden when they flash off)
    std::string text() const;
    bool isAnalog() const { return m_analog; }

private:
    void setup();       // -_setup: the view for the preference
    void schedule();    // the next update: the next second or minute
    void update();
    void readPreferences();
    std::string m_format;
    long long m_prefsStamp = 0;
    int m_prefsTimer = 0;
    bool m_analog = false, m_flash = false;
    int m_timer = 0;
};

// ICU's date patterns (NSDateFormatter's setDateFormat:), the fields the clock's preferences use
std::string formatDate(const std::string &pattern, time_t when);
