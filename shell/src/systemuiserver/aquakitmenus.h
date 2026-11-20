// SPDX-License-Identifier: LGPL-2.1-or-later
// The menus of the shell's AquaKit applications (Preview...): their MainMenu.nib's, which each
// publishes on its abstract socket "snowleopard-menus-<pid>" (frameworks/AppKit/mainmenu.h). A menu is
// asked for again when it opens, so its items are validated then (-validateMenuItem:); a chosen item
// is sent back as "do <path>". The application menu's Hide, Hide Others and Show All, and the Window
// menu, are the menu bar's (they act on the windows), unless the application's Window menu has actions
// of its own (Stickies': Collapse Window, Arrange By).
#pragma once

#include "controls.h"
#include "launchservices.h"

#include <functional>
#include <string>
#include <vector>

namespace AquaKitMenus {

bool available(int pid);
struct Top {
    std::string title, path;
    bool windowMenu = false;   // the menu bar's own Window menu stands for it
};
std::vector<Top> topLevel(int pid);
// a top-level menu's items now, validated by the application
std::vector<AK::MenuItem> menu(int pid, const std::string &path, const LaunchServices::App &app);

} // namespace AquaKitMenus
