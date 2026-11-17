// SPDX-License-Identifier: LGPL-2.1-or-later
// Quick Look (File > Quick Look, the toolbar's eye, the space bar): QuickLookUI's QLPreviewPanel, the
// dark HUD panel the Finder opens with -[TBrowserViewController togglePreviewPanelWithFullScreen:]
// over the items -getQuickLookNodes: gives (the selection). Its title the item's name, its close
// button at the top left; the item shown as its generators show it: pictures, PDFs (the first page),
// plain text on a white page, else the item's icon at 128 with its kind, size and dates. The panel
// follows the selection (-updatePreviewPanel); the arrows go through several items. Escape, the space
// bar or Command-Y close it.
#pragma once

#include "files.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Finder {

class QuickLook {
public:
    static bool isOpen();
    // open on the items (the first one shown), or close when it shows them already
    static void toggle(const std::vector<Item> &items);
    static void close();
    // the Finder window's selection changed: the panel shows it, if it is open
    static void follow(const std::vector<Item> &items);
    // the arrows in the panel: the window's selection moves too
    static inline std::function<void(const Item &)> onShowItem;
};

} // namespace Finder
