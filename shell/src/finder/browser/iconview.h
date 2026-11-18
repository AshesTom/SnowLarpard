// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's icon view (TIconView), with 10.6's default view options (icon size 64, text size 12,
// grid spacing 54, labels at the bottom, icon previews). Its grid is the Finder's own (docs/reverse-
// status.md 13): TDesktopViewController::calculateGridSize and TLayoutGrid, from the label's
// character width (the width of "0" in the label's font) and line height:
//   pad = icon / 16, icon box = icon + 2 pad
//   label width = floor(m + spacing x ((17 cw + 18) - m) / 100), m = max(4 cw + 18, icon box)
//   grid = (label width + 1, icon box + floor(2 lh + 2) + 6), each rounded up to even
//   an item's anchor: its cell's centre across and 4 right, its icon box's centre 4 down from the
//   cell's top; the label 2 under the icon box. An item with a place in the folder's .DS_Store (Iloc)
//   has its anchor there, and can be dragged to another
#pragma once

#include "files.h"

#include "appkit.h"
#include "text.h"

#include <functional>
#include <map>
#include <set>

namespace Finder {

class IconView : public AK::View {
public:
    IconView();

    void setItems(std::vector<Item> items);
    const std::vector<Item> &items() const { return m_items; }
    std::vector<int> selection() const { return {m_selection.begin(), m_selection.end()}; }
    void selectAll();

    // the height its items need at the view's width (the scroll view's document)
    double contentHeight(double width) const;
    // the icons' places (.DS_Store's Iloc, by name: their anchors); the others on the grid
    void setPositions(std::map<std::string, AK::Point> positions);
    // icons dragged to new places: their names and anchors
    std::function<void(const std::vector<std::pair<std::string, AK::Point>> &)> onMoved;
    // the selection dropped somewhere else than its own place: on a folder of this view, or outside
    // the view (in the window's coordinates, the screen's): the window decides where it goes
    std::function<void(const Item &folder, bool optionDown)> onDropOnFolder;
    std::function<void(AK::Point screen, bool optionDown)> onDropOutside;
    const Item *itemAtPoint(AK::Point inView) const
    {
        const int i = itemAt(inView);
        return i >= 0 ? &m_items[size_t(i)] : nullptr;
    }
    std::vector<Item> selectedItems() const
    {
        std::vector<Item> out;
        for (int i : m_selection)
            out.push_back(m_items[size_t(i)]);
        return out;
    }

    // the items with these paths selected alone (a new folder, pasted items), the first one shown
    void selectPaths(const std::vector<std::string> &paths);
    // where an item's name is, in the window (the rename field's place); empty when it isn't shown
    AK::Rect nameRectInWindow(const std::string &path) const;
    // the right button or a Control-click (the item under it selected first): the contextual menu
    std::function<void(AK::Point inWindow, bool onItem)> onContextMenu;
    // a click on the name of the item already selected alone: renaming after a moment
    std::function<void(const Item &)> onClickName;
    void rightMouseDown(AK::Event &e) override;
    // the folder kept arranged: Clean Up puts every icon on the grid's nearest free place;
    // arrangeOnce sorts them once on the grid (Arrange By), both as places to keep
    std::vector<std::pair<std::string, AK::Point>> cleanUp(bool selectionOnly) const;
    std::vector<std::pair<std::string, AK::Point>> arrangedPlaces(const std::string &by) const;

    std::function<void(const Item &)> onOpen;       // double-click, Command-Down
    std::function<void()> onSelectionChanged;
    std::function<void()> onBack;                   // Command-Up: the enclosing folder

    // the View Options (icvp): iconSize, gridSpacing, textSize, labelOnBottom, showItemInfo,
    // showIconPreview, arrangeBy ("none", "grid", "name", "dateModified", "dateCreated", "size", "kind",
    // "label"); set them, then relayout()
    int iconSize = 64;
    int gridSpacing = 54;
    AK::Font font = AK::Font::system(12);
    bool labelOnBottom = true, showItemInfo = false, showIconPreview = true;
    std::string arrangeBy = "none";
    void relayout()
    {
        m_anchors.clear();
        setNeedsDisplay();
    }

    void setFrame(const AK::Rect &r) override;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }

private:
    struct Metrics {
        double pad, box, labelWidth, labelHeight, lineHeight;
        int lines;
        AK::Size grid;
        double cellWidth = 0;   // labels at the bottom: ImageKit's cell, without the spacing
    };
    Metrics metrics() const;
    int columns() const;
    int columnsFor(double width, const Metrics &m) const;
    AK::Point spotFor(int cell, int cols, const Metrics &m) const;
    std::vector<AK::Point> place(double width) const;
    AK::Point anchor(int index) const;
    AK::Rect iconRect(int index) const;
    AK::Rect labelRect(int index) const;   // the label's whole place (two lines)
    std::vector<std::string> labelLines(const std::string &name, double width, int lines) const;
    std::string itemInfo(const Item &item) const;
    bool arrangedBefore(const Item &a, const Item &b) const;
    int itemAt(AK::Point p) const;
    void select(int index, bool extend);
    void revealItem(int index);

    std::vector<Item> m_items;
    std::set<int> m_selection;
    int m_anchor = -1;   // the last item clicked (shift-arrows)
    bool m_marquee = false;
    AK::Point m_marqueeStart, m_marqueeEnd;
    std::set<int> m_marqueeBase;
    std::map<std::string, AK::Point> m_positions;
    mutable std::vector<AK::Point> m_anchors;   // where each item is, until the items or the width change
    bool m_moving = false;
    AK::Point m_moveFrom, m_moveBy;
    int m_clickedName = -1;
    AK::Point m_pressAt;
    std::vector<AK::Point> gridSpots(size_t count) const;
};

} // namespace Finder
