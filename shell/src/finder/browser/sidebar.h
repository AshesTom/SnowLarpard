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
    };
    std::vector<Row> rows;
    static constexpr double RowHeight = 20;

    void reload();                         // the rows, from the volumes and the user's folders
    void selectPath(const std::string &path);   // the row showing that folder, if one does
    std::function<void(const Row &)> onSelect;
    std::string pathAt(AK::Point inView) const;   // a folder's row under the point (a drop's target)
    double contentHeight() const { return rows.size() * RowHeight + 4; }

    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;

private:
    int m_selected = -1;
};

} // namespace Finder
