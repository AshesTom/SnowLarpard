// SPDX-License-Identifier: LGPL-2.1-or-later
// loginwindow's Log Out: the panel LogOutAlert shows (LogOut.nib, the DVD's) with its 60 seconds'
// countdown, and the log out itself, ApplicationManager's 'rlgo': the applications asked to quit,
// then the session ended. Reversed from loginwindow 6.0's x86_64 slice, decrypted
// (tools/deprotect.py): -[LogOutAlert showLogoutPanel] (0x100018d73), sizeWindowToFit (0x100019df0),
// displayCurrentCountdownTime (0x10001993e), updateTimeFormatString (0x100019cff),
// countDownToLogout: (0x100036934), logout: (0x10001a43f), cancel: (0x1000369d5).
//   loginwindow --logout        the panel (Apple menu > Log Out <name>…, ⇧⌘Q)
//   loginwindow --logout-now    no panel (⌥⇧⌘Q)
#pragma once

namespace LoginWindow {

int logOut(bool askFirst);
// The Apple menu's Restart… and Shut Down…: the same panel (LogOutAlert), then the applications asked
// to quit, the computer restarted or shut down (SNOWLEOPARD_POWER=1), the session ended
//   loginwindow --restart | --shutdown
int restart(bool shutDown);
// Force Quit Applications (ProcessPanel.nib)
//   loginwindow --force-quit
int forceQuit();

} // namespace LoginWindow
