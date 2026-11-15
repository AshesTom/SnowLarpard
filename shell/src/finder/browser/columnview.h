// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's column view (TColumnView, an NSBrowser in ColumnView.nib: columns at least 100 wide,
// resized one by one). 10.6's settings (-[TNodeViewSettings refreshColumnViewSettings]): text 12,
// columns 205 wide, icons shown, the preview column shown; a row the text size + 5 high
// (-rowHeightForVariableIconAndText); the preview column at least 208 wide
// (-browser:shouldSizeColumn:forUserResize:toWidth:). A folder shows CoreUI's ChildContainer arrow.
#pragma once

#include "files.h"

#include "appkit.h"

#include <algorithm>
#include <functional>

namespace Finder {

class ColumnView : public AK::View {
public:
    static constexpr double DefaultColumnWidth = 205, MinColumnWidth = 100, PreviewMinWidth = 208;

    void setRoot(const std::string &folder);
    // the deepest folder shown (the window's title), the items of the column with the keyboard and
    // its selection (the status bar)
    std::string currentFolder() const;
    size_t itemCount() const;
    size_t selectedCount() const;
    std::vector<Item> selectedItems() const;   // the keyboard's column's selection

    // the View Options (global, as com.apple.finder's): text size, icons, icon previews, preview column
    int textSize = 12;
    bool showIcons = true, showIconPreview = true, showPreviewColumn = true;

    std::function<void(const Item &)> onOpen;
    std::function<void()> onChanged;   // the selection or the columns

    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    void scrollWheel(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }

private:
    struct Column {
        std::string folder;
        std::vector<Item> items;
        int selected = -1;
        double width = DefaultColumnWidth;
        double offset = 0;   // scrolled down
    };
    double rowHeight() const { return textSize + 5; }
    double contentWidth() const;
    double columnsHeight() const;   // above the horizontal scroller
    bool hasPreview() const;
    double columnX(size_t i) const;
    void select(size_t column, int row);
    void reveal();
    void scrollColumn(size_t column, double offset);
    void drawColumn(cairo_t *cr, size_t i, const AK::Rect &r);
    void drawPreview(cairo_t *cr, const AK::Rect &r);

    std::vector<Column> m_columns;
    size_t m_active = 0;   // the column with the keyboard
    double m_hoffset = 0;
    double previewWidth = std::max(DefaultColumnWidth, PreviewMinWidth);   // a column's, at least 208
    int iconSize = 16;
    enum Drag { None, Resize, HKnob, VKnob } m_drag = None;
    size_t m_dragColumn = 0;
    double m_dragFrom = 0, m_dragStart = 0;
};

} // namespace Finder
