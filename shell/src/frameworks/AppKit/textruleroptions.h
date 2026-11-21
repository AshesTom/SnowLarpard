// SPDX-License-Identifier: LGPL-2.1-or-later
// NSTextRulerOptions (AppKit-1038, the same on 10.6 and 10.6.8): the sheets NSTextView opens from the
// ruler's pull-downs and the Format menu, from AppKit's NSTextRulerOptions.nib. Here the spacing
// sheet (Format ▸ Text ▸ Spacing…, the ruler's Spacing ▸ Other…):
//   - -setSpacing:inWindow:delegate: (0x3bc511): the selection's paragraph style shown
//     (-updateLineSpacingUI 0x3bc6a5): the line height multiple (0: 1), the minimum and maximum line
//     heights, the inter-line spacing, the spacing before and after; a minimum equal to the maximum
//     is Exactly (At most off and disabled), else At least, At most on when there is a maximum
//   - -modifyOptionsViaPanel: (0x3cb148): each field and its stepper kept together; Exactly takes
//     the maximum away and gives the minimum the font's default line height when it had none; At
//     most on gives the maximum at least the minimum; the minimum never over the maximum, nor the
//     maximum under the minimum; a multiple of 0 or less is 1
//   - OK (-confirmSheet:) applies them to the selected paragraphs (Exactly: the maximum the
//     minimum), Cancel (-cancelSheet:) leaves them
#pragma once

#include "appkit.h"
#include "textview.h"

namespace AK {

namespace TextRulerOptions {
// The spacing sheet over `window` for `text`'s selected paragraphs; `changed` after OK
void setSpacing(Window *window, TextView *text, std::function<void()> changed = {});
}

} // namespace AK
