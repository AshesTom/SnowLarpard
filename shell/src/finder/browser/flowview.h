// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's Cover Flow view (FlowView.nib): a TFlowSplitView with no divider (-dividerThickness 0)
// holding the flow (ImageKit's IKImageFlowView) over the list view (TFlowListView, the list's
// columns); the flow two thirds of the height at first (-[TFlowViewController awakeFromNib]'s
// FlowViewHeight default), at least 180 (+[TFlowSplitView flowViewMinHeight]). The flow and the list
// share the selection. ImageKit draws the flow with OpenGL; here cairo draws the covers in
// perspective strip by strip, their reflections, the title and kind, and the slider.
#pragma once

#include "listview.h"

#include "appkit.h"

namespace Finder {

class CoverFlow;

class FlowView : public AK::View {
public:
    static constexpr double FlowMinHeight = 180;

    FlowView();
    void setItems(std::vector<Item> items);
    ListView *list() const { return m_list; }
    void setFrame(const AK::Rect &r) override;
    void focus();

    std::function<void(const Item &)> onOpen;
    std::function<void()> onSelectionChanged;

private:
    friend class CoverFlow;
    void layout();
    void syncFromList();

    CoverFlow *m_flow = nullptr;
    ListView *m_list = nullptr;
    double m_flowHeight = -1;   // until set: two thirds
};

} // namespace Finder
