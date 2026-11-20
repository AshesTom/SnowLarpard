// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's View Options panel (ViewOptionsWindow.nib, TViewOptionsWindow): one panel following
// the front window, its page the window's view (the tab view's selectedIdentifier bound to
// viewStyleForViewOptions: Icon, List, Column; Cover Flow shows List's). Its controls are found by
// their bindings' key paths (targetedViewOptionsSettings.iconViewIconSize...), as TViewOptionsSettings
// would fill them. At the top "Always open in icon view" (nodeHasCustomViewStyle: the folder's vstl);
// at the bottom "Use as Defaults".
#pragma once

#include "appkit.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AK {
class Nib;
class Window;
}

namespace Finder {

// A folder's view settings: the icon view's (icvp), the list's (lsvp), the column view's (the
// Finder's preferences, not the folder's), the view and whether the folder always opens in it
struct ViewSettings {
    std::string folderName;
    bool desktop = false;       // the desktop's (no "Always open", background or defaults)
    int view = 0;               // 0 icons, 1 list, 2 columns, 3 Cover Flow
    bool alwaysOpen = false;    // vstl
    int iconSize = 64, gridSpacing = 54, iconTextSize = 12;
    bool labelOnBottom = true, showItemInfo = false, iconPreview = true;
    std::string arrangeBy = "none";
    int listTextSize = 12, listIconSize = 16;
    bool relativeDates = true, calculateAllSizes = false, listIconPreview = true;
    std::vector<std::string> listColumns{"dateModified", "size", "kind"};
    int columnTextSize = 12;
    bool columnShowIcons = true, columnIconPreview = true, columnPreview = true;
};

class ViewOptions {
public:
    static ViewOptions &shared();

    bool isOpen() const;
    void toggle(const ViewSettings &settings);   // View > Show View Options (Command-J)
    void update(const ViewSettings &settings);   // the front window's, when it changes

    std::function<void(const ViewSettings &)> onChange;
    std::function<void()> onUseAsDefaults;

private:
    ViewOptions() = default;
    void build();
    void fill();
    void changed();

    std::unique_ptr<AK::Nib> m_nib;
    std::unique_ptr<AK::Window> m_window;
    ViewSettings m_settings;
    bool m_filling = false;
};

} // namespace Finder
