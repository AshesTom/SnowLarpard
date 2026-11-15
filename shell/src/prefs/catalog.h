// SPDX-License-Identifier: LGPL-2.1-or-later
// System Preferences' panes, as 10.6 lists them: the groups and their order from the application's
// NSPrefPaneGroups.xml, each pane's name, icon and nib from its bundle (/System/Library/PreferencePanes,
// the DVD's). The panes for hardware that isn't there are left out, as on the Mac (Trackpad without a
// touchpad, Ink without a tablet, Fibre Channel without its card).
#pragma once

#include "graphics.h"

#include <string>
#include <vector>

struct PaneInfo {
    std::string id;       // CFBundleIdentifier: com.apple.preference.dock
    std::string bundle;   // its Mac path
    std::string label;    // NSPrefPaneIconLabel, with its line break ("Desktop &\nScreen Saver")
    std::string nib;      // NSMainNibFile
    std::string group;
    AK::Image icon;       // 32 points
    std::string name() const;   // the label on one line: the window's title
    std::string resources() const;
};

struct PaneGroup {
    std::string id, title;
    std::vector<PaneInfo> panes;
};

const std::vector<PaneGroup> &catalog();
const PaneInfo *paneById(const std::string &id);
// "dock", "desktop", "screensaver", "sound"... (the --pane option) to a pane
const PaneInfo *paneByShortName(const std::string &name);
// The panes whose words (their search terms) contain `text`
std::vector<std::string> searchPanes(const std::string &text);
