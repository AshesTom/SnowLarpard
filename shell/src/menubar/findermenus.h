// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's menus in the menu bar, as its MenuBar.nib has them (their titles, key equivalents and
// actions: cmdNewFileViewer:, cmdNewFolder:...). A command goes to the Finder's windows' process
// (FinderWindows --command <selector>: the running one, over its socket); what the front Finder
// window allows, its titles ("Compress “a.txt”", "Show Path Bar") and checks (the view) come from the
// state file it writes ($XDG_RUNTIME_DIR/snowleopard-finder-<uid>-<display>.state). With the desktop
// in front (or no Finder window), the commands that need a window are disabled.
#pragma once

#include <QVector>

class QMenu;

namespace FinderMenus {

// File, Edit, View, Go; `desktop`: the desktop is the active window
QVector<QMenu *> build(bool desktop);
void send(const QString &command);
bool finderRunning();

} // namespace FinderMenus
