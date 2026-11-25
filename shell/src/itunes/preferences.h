// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's Preferences (iTunes ▸ Preferences…, ⌘,: DLOG / DITL 134 and a pane per toolbar item,
// docs/itunes-reverse.md section 14): the panes built from their DITLs.
#pragma once

#include "appkit.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace AK {
class Label;
}

namespace iTunes {

class PreferencesWindow {
public:
    PreferencesWindow();
    ~PreferencesWindow();
    void show();
    // OK: the preferences kept (Settings "itunes"), for the window to apply them
    std::function<void()> onApply;
    // OK on the Advanced pane with the music folder or Keep organized changed (FUN_002ac6e6): whether
    // the folder changed, and whether Keep organized was on before
    std::function<void(bool folderChanged, bool wasOrganized)> onMusicFolder;

private:
    class Toolbar;
    void select(int pane);
    void save();
    void setFolder(const std::string &dir);

    std::unique_ptr<AK::Window> m_window;
    Toolbar *m_toolbar = nullptr;
    std::vector<AK::View *> m_paneViews;
    std::map<int, AK::View *> m_items;   // the pane's DITL items by number (1 up)
    int m_pane = 0;
    std::string m_folder;            // Advanced: the music folder shown, kept on OK
    AK::Label *m_where = nullptr;
    std::unique_ptr<AK::Window> m_importSettings;   // General's Import Settings…
};

} // namespace iTunes
