// SPDX-License-Identifier: LGPL-2.1-or-later
// Show All: the panes by group, a band each (its name, then its icons eight a row with their names),
// and Spotlight's search over them: the window dimmed, a spotlight on each pane that matches.
#pragma once

#include "catalog.h"
#include "controls.h"

#include <functional>

class ShowAll : public AK::View {
public:
    ShowAll();
    static constexpr double Width = 668;
    double height() const;
    void setSearch(const std::string &text);
    std::function<void(const std::string &id)> onOpen;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;

private:
    struct Cell {
        const PaneInfo *pane;
        AK::Rect rect;   // the icon and its name
    };
    std::vector<Cell> m_cells;
    std::vector<std::pair<std::string, AK::Rect>> m_bands;   // a group's name and band
    std::vector<std::string> m_matches;
    bool m_searching = false;
    int m_pressed = -1, m_down = -1;
    int cellAt(AK::Point p) const;
};
