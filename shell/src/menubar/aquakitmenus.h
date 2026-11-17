// SPDX-License-Identifier: LGPL-2.1-or-later
// The menus of the shell's AquaKit applications (Preview...): their MainMenu.nib's, which the
// application publishes on its abstract socket "snowleopard-menus-<pid>" (aquakit/mainmenu.h). Each
// menu is asked for again when it opens, so its items are validated then (-validateMenuItem:); a
// chosen item is sent back as "do <path>". The application menu's Hide, Hide Others and Show All, and
// the Window menu, are the shell's (they act on the windows, as the menu bar's own menus do), unless
// the application's Window menu has actions of its own (Stickies': Collapse Window, Arrange By).
#pragma once

#include "apps.h"

#include <QVector>
#include <functional>

class QMenu;

namespace AquaKitMenus {

// The process publishes its menus
bool available(int pid);
// Its menus, the application menu first; `windowMenu` makes the shell's Window menu. Empty when the
// application doesn't answer.
QVector<QMenu *> build(int pid, const App &app, const std::function<QMenu *()> &windowMenu);

} // namespace AquaKitMenus
