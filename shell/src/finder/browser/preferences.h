// SPDX-License-Identifier: LGPL-2.1-or-later
// Finder > Preferences… (⌘,): TPreferencesWindowController and PreferencesWindow.nib, its toolbar's
// four panes (general:, labels:, sidebar:, advanced:), each pane controller's _view:
//   General   Show these items on the desktop (ShowHardDrivesOnDesktop, ShowExternalHardDrivesOnDesktop,
//             ShowRemovableMediaOnDesktop, ShowMountedServersOnDesktop), New Finder windows open
//             (NewWindowTarget: PfCm the computer, PfVo the startup disk, PfHm home, PfDe desktop,
//             PfDo documents, PfLo another folder in NewWindowTargetPath), Always open folders in a new
//             window, Spring-loaded folders and windows (com.apple.springing.enabled, .delay)
//   Labels    the seven names (Label_Name_N), their swatches
//   Sidebar   the items the sidebar shows
//   Advanced  AppleShowAllExtensions, FXEnableExtensionChangeWarning, WarnOnEmptyTrash,
//             EmptyTrashSecurely, FXDefaultSearchScope (SCev This Mac, SCcf the current folder, SCsp
//             the previous scope)
// The keys are com.apple.finder's, kept in ~/.config/snowleopard/finder.conf.
#pragma once

#include <functional>

namespace Finder {

class Preferences {
public:
    static void show(int pane = -1);   // 0 General, 1 Labels, 2 Sidebar, 3 Advanced; -1 the last one
    static inline std::function<void()> onChange;   // the sidebar, the labels, the extensions: the windows redraw
};

} // namespace Finder
