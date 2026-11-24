// SPDX-License-Identifier: LGPL-2.1-or-later
// SystemUIServer's NSMenuToolbar: the menu extras at the menu bar's right, in the system status window
// (10.6.8: -relayout 0x1000075bf, -menuBarFrame 0x1000151e9). The items from 4 points, each the next at
// its length + 8; each item's place (its subwindow on the Mac, CGSSetSystemStatusBarWindowSubitemOffsets
// at x - 4) 4 points wider on each side, where its menu's highlight goes. Here a view of the bar,
// as wide as its items, at the bar's right edge.
#pragma once

#include "systemuiplugin.h"

#include <functional>
#include <vector>

class MenuToolbar : public AK::View, public SystemUI::MenuExtra::Host {
public:
    ~MenuToolbar() override;
    void addItem(SystemUI::MenuExtra *extra);   // takes it, at the right
    void insertItem(SystemUI::MenuExtra *extra, size_t index);   // -addItem:atIndex:
    void removeItem(SystemUI::MenuExtra *extra);   // -removeItem: (willUnload, then deleted)
    const std::vector<SystemUI::MenuExtra *> &items() const { return m_items; }
    double width() const { return m_width; }
    // the item's place under a point of the toolbar, and its extra
    SystemUI::MenuExtra *extraAt(AK::Point p, AK::Rect *place = nullptr) const;
    AK::Rect placeOf(SystemUI::MenuExtra *extra) const;

    void extraLengthChanged(SystemUI::MenuExtra *) override { relayout(); }
    void extraViewChanged(SystemUI::MenuExtra *extra, SystemUI::MenuExtraView *old) override;
    void popUpMenu(SystemUI::MenuExtra *extra, const std::vector<AK::MenuItem> &items, std::function<void()> closed) override;
    void commandDrag(SystemUI::MenuExtra *extra, Drag phase, AK::Point screen) override;
    void unloadExtra(SystemUI::MenuExtra *extra) override;   // -[NSMenuExtra unload]: removed on the next turn
    // the extras' new order, or one removed (dragged off the bar): the list to write
    std::function<void(const std::vector<std::string> &order, const std::string &removed)> listChanged;
    SystemUI::MenuExtra *fixed = nullptr;   // Spotlight: at the edge, not moved

    // the menu bar opens the menus (its tracking slides from one to the next); told of a new width
    std::function<void(SystemUI::MenuExtra *, const std::vector<AK::MenuItem> &, std::function<void()>)> openMenu;
    std::function<void()> resized;
    void unloadAll();

private:
    class ItemView;
    void relayout();
    std::vector<SystemUI::MenuExtra *> m_items;
    std::vector<ItemView *> m_views;
    double m_width = 0;
    SystemUI::MenuExtra *m_dragged = nullptr;
};
