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
// ShutDownAlert (ShutDownAlert.nib): the Apple menu's Shut Down… (type 1) and Restart… (2), the power
// key's (0: Restart, Sleep, Cancel, Shut Down), the scheduled ones (3 shut down, 4 sleep, 5 restart);
// then the applications asked to quit, the computer restarted or shut down (SNOWLEOPARD_POWER=1), the
// session ended. Reversed from loginwindow 6.0's x86_64 slice, decrypted: -[ShutDownAlert showPanel]
// (0x1000354a3), -renameDefaultButtonAndResetUI: (0x100035fe4), -sizeWindowToFit (0x100035dee),
// -loadBaseFormatString (0x100035038), -doDefaultAction: (0x1000353b6)
//   loginwindow --restart | --shutdown | --power
int restart(int type);
// Force Quit Applications (ProcessPanel.nib)
//   loginwindow --force-quit
int forceQuit();

} // namespace LoginWindow
