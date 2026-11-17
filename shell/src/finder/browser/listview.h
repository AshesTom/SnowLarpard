// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's list view (TListView, an NSOutlineView in ListView.nib): the columns ListView.nib
// declares (Name 300, Date Modified 181, Size 100, Kind 115 shown by default; the last one takes the
// rest, NSColumnAutoresizingStyle 4; wider than the view, a horizontal scroller), 2 points between
// columns and 3 between rows; 10.6's list settings (TListViewSettings +defaultSettingsForNewUser:
// text 12, icons 16, relative dates, sorted by name); a row as high as TListViewController -rowHeightForIconAndText says (icons of 16: the text
// size, at most 16, plus 4), folders opening in place (an indentation of the icon size + 5).
#pragma once

#include "files.h"

#include "appkit.h"
#include "controls.h"

#include <functional>
#include <set>

namespace Finder {

class ListView : public AK::View {
public:
    ListView();

    void setItems(std::vector<Item> items);   // the folder's: the top level
    std::vector<Item> selectedItems() const;
    std::vector<Item> rowItems() const;       // the rows, in their order (Cover Flow's)
    void selectPath(const std::string &path);   // that row alone, shown
    size_t itemCount() const;                  // the top level's
    size_t selectedCount() const { return m_selection.size(); }
    void selectAll();

    // the sort (a column's id: "name", "dateModified", "size", "kind"), as .DS_Store's lsvp keeps it
    void setSort(const std::string &column, bool ascending);
    const std::string &sortColumn() const { return m_sortColumn; }
    bool ascending() const { return m_ascending; }
    std::function<void()> onSortChanged;   // a header clicked
    // the View Options (lsvp): the columns shown (Name always), the text size, small or large icons
    // (16, 32), relative dates, icon previews
    void setColumnsShown(const std::vector<std::string> &ids);
    std::vector<std::string> columnsShown() const;
    void setOptions(int textSize, int iconSize, bool relativeDates, bool iconPreviews);

    std::function<void(const Item &)> onOpen;
    std::function<void()> onSelectionChanged;
    std::function<void()> onBack;
    // the rows with these paths selected (a new folder, pasted items), the first shown
    void selectPaths(const std::vector<std::string> &paths);
    AK::Rect nameRectInWindow(const std::string &path) const;   // the rename field's place
    std::function<void(AK::Point inWindow, bool onItem)> onContextMenu;
    std::function<void(const Item &)> onClickName;   // a click on the name of the row selected alone

    void setFrame(const AK::Rect &r) override;
    void focus();

    struct Column {
        std::string id, title;
        double width, minWidth;
        AK::Align align;
    };

private:
    friend class ListHeader;
    friend class ListBody;
    friend class HScroller;
    double contentWidth() const;
    void scrollHorizontally(double x);
    struct Row {
        Item item;
        int level = 0;
        bool expanded = false;
    };
    void sort();
    void sortRows(std::vector<Row> &rows) const;
    void layout();
    void toggle(int row);
    void select(int row, bool extend);
    void reveal(int row);
    double pitch() const { return rowHeight + 3; }

    std::vector<Column> m_columns;
    std::vector<Row> m_rows;
    std::set<int> m_selection;
    int m_anchor = -1;
    std::string m_sortColumn = "name";
    bool m_ascending = true;
    double rowHeight = 16;
    int textSize = 12, iconSize = 16;
    bool relativeDates = true, showIconPreview = true;
    std::vector<Column> m_allColumns;   // ListView.nib's
    View *m_header = nullptr;
    AK::ScrollView *m_scroll = nullptr;
    View *m_body = nullptr;
    View *m_hscroller = nullptr;
    double m_hoffset = 0;
};

} // namespace Finder
