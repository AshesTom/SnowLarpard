// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder window's sidebar (TSidebarView, a source list): DEVICES, PLACES and SEARCH FOR with their
// headers (LocalizableCore SD5-SD8), 10.6's default places (Desktop, the home folder, Applications,
// Documents) and searches (Today, Yesterday, Past Week, All Images, All Movies, All Documents).
// CoreUI draws a source list (CUIRenderer::DrawGradient): its background kCUIVariantGradientSideBar,
// the selection kCUIVariantGradientSideBarSelection (_kPairs: the colours, by the window's state and
// the list's focus).
#pragma once

#include "appkit.h"
#include "graphics.h"

#include <functional>
#include <string>
#include <vector>

namespace Finder {

class Sidebar : public AK::View {
public:
    struct Row {
        std::string title, path;   // path: a folder, or "search:<name>" for SEARCH FOR's
        AK::Image icon;
        bool header = false;
        bool ejectable = false;   // a removable or external disk, a server: the eject button at its right
        bool added = false;       // put there by File > Add to Sidebar (Remove from Sidebar takes it out)
    };
    std::vector<Row> rows;
    // 20 a row; a section's header after another section 7 lower (a 10.6 Finder: an item's icon 31
    // above the next header's capitals, those 15 above its first icon)
    static constexpr double RowHeight = 20, SectionGap = 7;
    double rowTop(size_t i) const;
    int rowAt(double y) const;

    void reload();                         // the rows, from the volumes and the user's folders
    void selectPath(const std::string &path);   // the row showing that folder, if one does
    std::function<void(const Row &)> onSelect;
    std::string pathAt(AK::Point inView) const;   // a folder's row under the point (a drop's target)
    std::function<void(const Row &)> onEject;
    std::function<void(const Row &, AK::Point inWindow)> onContextMenu;
    void rightMouseDown(AK::Event &e) override;
    // File > Add to Sidebar (⌘T): the folder or file under PLACES, kept in finder.conf's SidebarPlaces
    static void add(const std::string &path);
    static void remove(const std::string &path);
    // Preferences > Sidebar: an item shown ("Computer", "HardDisks", "ExternalDisks", "Removable",
    // "Servers", "Home", "Desktop", "Applications", "Documents", "Today"... the canned searches)
    static bool shows(const std::string &item);
    double contentHeight() const { return rows.empty() ? 4 : rowTop(rows.size() - 1) + RowHeight + 2; }

    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;

private:
    int m_selected = -1;
};

} // namespace Finder
