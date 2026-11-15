// SPDX-License-Identifier: LGPL-2.1-or-later
// NSToolbar under the title bar, in one gradient with it (Window::setUnifiedToolbarHeight): the items'
// controls 25 high, 7 down, each in a viewer 4 wider on both sides (none between viewers: the Aqua
// metrics' inter-item spacing is 0; the spaces have no padding), 4 in from the ends; their labels (Lucida Grande 11) under them in icon-and-label
// mode (Preview's), none in icon-only mode (the Finder's). Template images (NSImage isTemplate) drawn
// as AppKit draws them on a textured control.
#pragma once

#include "controls.h"

namespace AK {

class Toolbar : public View {
public:
    // icon and label: 54 points of gradient; icon only: the same less the label and its gap (17)
    static constexpr double Gradient = 54, Height = Gradient + 1, IconOnlyGradient = 37,
                            ControlTop = 7, ControlHeight = 25, Margin = 4, Padding = 4,
                            SpaceWidth = 32, FlexibleMinimum = 1;   // -[NSToolbarSpaceItem minSize], ...FlexibleSpaceItem

    explicit Toolbar(bool labels = true) : m_labels(labels) {}
    double gradient() const { return m_labels ? Gradient : IconOnlyGradient; }
    double height() const { return gradient() + 1; }

    struct Item {
        std::string id, label;
        View *view = nullptr;
        double width = 0;   // the control's (its minimum when it grows)
        double maxWidth = 0;   // more than width: the control grows with the flexible spaces (a search field)
        bool flexible = false;
        // a flexible space tracking a split view's divider (-[NSToolbarFlexibleSpaceItem
        // setTrackedSplitView:]): it reaches this x, so that the next item starts by the divider; < 0 none
        double alignTo = -1;
        bool shown = true;
        Rect frame;         // the item's, the label's width included
    };
    std::vector<Item> items;

    // NSToolbarFlexibleSpaceItem is a flexible space, NSToolbarSpaceItem a 32 point space. The room left is
    // shared as -[NSToolbarView _distributeAvailableSpaceIfNecessary:...] shares it (AppKit 10.6): by
    // weight, 0.25 a flexible space (+[NSToolbarFlexibleSpaceItem _resizeWeight]), 1 an item that grows
    // (+[NSToolbarItem _resizeWeight]), an item no wider than its maximum, what it can't take going to
    // the others; a flexible space tracking a divider sized first
    void addItem(const std::string &id, const std::string &label, View *view, double width, double maxWidth = 0);
    void layout();
    void setFrame(const Rect &r) override;
    void draw(cairo_t *cr) override;

    // WindowServer's unified title bar and toolbar (as System Preferences' toolbar draws it): row 0 the
    // title bar's top
    static Color rowColor(int row, int total, bool active);

private:
    double itemWidth(const Item &it) const;   // its viewer's, at its minimum
    bool m_labels;
};

// A template image (black and clear) on a textured control: dark grey with a white emboss one point
// under it, fainter when disabled or in an inactive window; on a selected or pressed (dark) segment,
// white with a dark shadow under it
void drawTemplate(cairo_t *cr, const Image &image, const Rect &r, bool enabled, bool active, bool highlighted = false);

// NSSegmentedControl of the toolbar showing images (template or not), centred in their segments
SegmentedControl *toolbarSegments(const std::vector<Image> &images, const std::vector<double> &widths, bool momentary,
                                  bool templates = false);

} // namespace AK
