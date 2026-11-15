// SPDX-License-Identifier: LGPL-2.1-or-later
// What the Apple menu, the application menu and the Dock's menus do to the session and to the
// applications: hide, quit, force quit, sleep, restart, shut down, log out.
#pragma once

#include "apps.h"

#include <QString>
#include <QVector>
#include <xcb/xcb.h>

namespace Session {

enum Power { Sleep, Restart, ShutDown, LogOut };

QString userFullName();
// The application's windows, bottom to top.
QVector<xcb_window_t> windowsOf(const App &app);

void hideApp(const App &app);
void hideOthers(const App &app);
void showAll();
void bringToFront(const App &app);
void quitApp(const App &app);

void aboutThisMac();
void forceQuit();
void emptyTrash();
// Asks first, as the Mac does (Sleep doesn't). Restart and Shut Down only act when
// $SNOWLEOPARD_POWER is 1 (a whole session, not a window of another desktop).
void power(Power what);

} // namespace Session
