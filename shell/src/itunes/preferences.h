// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's Preferences (iTunes ▸ Preferences…, ⌘,: DLOG / DITL 134 and a pane per toolbar item,
// docs/itunes-reverse.md section 14): the panes built from their DITLs.
#pragma once

#include "appkit.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace iTunes {

class PreferencesWindow {
public:
    PreferencesWindow();
    ~PreferencesWindow();
    void show();
    // OK: the preferences kept (Settings "itunes"), for the window to apply them
    std::function<void()> onApply;

private:
    class Toolbar;
    void select(int pane);
    void save();

    std::unique_ptr<AK::Window> m_window;
    Toolbar *m_toolbar = nullptr;
    std::vector<AK::View *> m_paneViews;
    std::map<int, AK::View *> m_items;   // the pane's DITL items by number (1 up)
    int m_pane = 0;
};

} // namespace iTunes
