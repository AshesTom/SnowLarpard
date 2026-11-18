// SPDX-License-Identifier: LGPL-2.1-or-later
// What the Apple menu and the application menu do to the applications and the session, without Qt:
// hide, show, quit (the Process Manager's, on the windows through Windows), and loginwindow's panels
// (About This Mac, Force Quit, Restart, Shut Down, Log Out), as the Mac's menu bar asks loginwindow.
#pragma once

#include "launchservices.h"

#include <string>
#include <vector>
#include <xcb/xcb.h>

namespace Session {

using LaunchServices::App;

App appForWindow(xcb_window_t w);
std::vector<xcb_window_t> windowsOf(const App &app);   // bottom to top
void hideApp(const App &app);
void hideOthers(const App &app);
void showAll();
void bringToFront(const App &app);
void quitApp(const App &app);

std::string userFullName();
void aboutThisMac();
void forceQuit();
void sleep();
void restart();
void shutDown();
void logOut(bool ask = true);

} // namespace Session
