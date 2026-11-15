// SPDX-License-Identifier: LGPL-2.1-or-later
// What the shell's programs share at start: the fonts, Snow Leopard's menus (white, with the blue
// selection gradient) and the X11 platform.
#pragma once

class QApplication;
class QMenu;

namespace Aqua {

// Before QApplication: X11 (XWayland under a Wayland desktop), no Qt scaling of our own.
void preinit();
// After it: fonts, the menus' style.
void init(QApplication &app);
// A popup menu of the shell's: its window type, its translucency when a compositor runs.
void prepareMenu(QMenu *menu);

} // namespace Aqua
