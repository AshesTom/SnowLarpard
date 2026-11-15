// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's Copy window ("Copy", PW32): one view per running operation stacked in it,
// ProgressStatusView2Line.nib's (the item's icon, "Copying “a” to “b”" (CP4, MV4, DU3, MT8, ET2...),
// the bar, "12 MB of 40 MB - About 5 seconds" (PW3, U20-U26), the stop button); an operation waiting
// on a name already taken shows ProgressReplaceView.nib's instead (PE1, PE32, PE61-PE64: Stop, Don't
// Replace, Replace, Apply to All). The window shows when an operation takes more than a second, or at
// once to ask.
#pragma once

#include "fileops.h"

#include <functional>
#include <memory>
#include <string>

namespace Finder {

class Progress {
public:
    // the operation followed until it ends; done(op) then (the windows showing its folders refresh)
    static void follow(std::shared_ptr<Operation> op);
    static inline std::function<void(const Operation &)> onFinished;
};

} // namespace Finder
