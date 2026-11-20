// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's menus in the menu bar, as its MenuBar.nib has them (their titles, key equivalents and
// actions: cmdNewFileViewer:, cmdNewFolder:...). A command goes to the Finder's windows' process
// (FinderWindows --command <selector>) or, with the desktop in front, to the desktop's icons (their
// socket); what the front window allows, its titles and checks come from the state file it writes
// ($XDG_RUNTIME_DIR/snowleopard-finder-<uid>-<display>.state). Ported from MenuBar's (Qt) as it was.
#pragma once

#include "controls.h"

#include <string>
#include <vector>

namespace FinderMenus {

// File, Edit, View, Go; `desktop`: the desktop is the active window
std::vector<std::string> titles();
std::vector<AK::MenuItem> menu(const std::string &title, bool desktop);
void send(const std::string &command, bool desktop = false);

} // namespace FinderMenus
