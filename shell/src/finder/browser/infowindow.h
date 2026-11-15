// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's Info window (Get Info, TInfoWindowController): no nib of its own; it stacks one slice
// per nib, each a TInfoWindowViewController with its disclosure triangle (toggleExpanded:):
//   InfoWindowHeaderView (the icon, name, size, Modified; Spotlight Comments under its own triangle),
//   InfoWindowGeneralView, InfoWindowMoreInfoView, InfoWindowNameView, InfoWindowOpenWithView,
//   InfoWindowPreviewView, InfoWindowPermissionsView.
// 265 wide (+[IInfoWindowControllerBase preferredWindowWidth], 265 to 400); a closed slice 20 high
// (-disclosedHeight; the header's 74), an open one its rows and a bottom margin of 8 (-bottomMargin;
// the header's 5, Open with's 1); General and Preview open at first (-expandedDefaultValue). In a
// slice the rows that don't apply are hidden and the rows under them move up (TLayoutBinder: each
// view bound to the one above it, its "master view").
#pragma once

#include "files.h"

#include "appkit.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Finder {

class InfoWindow {
public:
    // A window for the item, or the one already showing it brought forward
    static void show(const Item &item);
    static void closeAll();

    // an item renamed from Name & Extension: its folder (the windows showing it refresh)
    static inline std::function<void(const std::string &folder)> onRenamed;

private:
    explicit InfoWindow(const Item &item);
    struct Slice;
    void build();
    void fillHeader(Slice &s);
    void fillGeneral(Slice &s);
    void fillMoreInfo(Slice &s);
    void fillName(Slice &s);
    void fillOpenWith(Slice &s);
    void fillPreview(Slice &s);
    void fillPermissions(Slice &s);
    void pack(Slice &s);
    void layout();
    void startSizing();
    static std::string launchServicesAlias();

    Item m_item;
    std::unique_ptr<AK::Window> m_window;
    std::vector<std::unique_ptr<Slice>> m_slices;
    int m_timer = 0;
};

} // namespace Finder
