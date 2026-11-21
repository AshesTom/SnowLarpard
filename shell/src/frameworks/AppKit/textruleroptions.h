// SPDX-License-Identifier: LGPL-2.1-or-later
// NSTextRulerOptions (AppKit-1038, the same on 10.6 and 10.6.8): the sheets NSTextView opens from the
// ruler's pull-downs and the Format menu, from AppKit's NSTextRulerOptions.nib. The spacing sheet
// (Format ▸ Text ▸ Spacing…, the ruler's Spacing ▸ Other…):
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
// The link sheet (Format ▸ Text ▸ Link…, ⌘K): -[NSTextView orderFrontLinkPanel:] (0x573db2) gives
// -setLinkInWindow:string:delegate: (0x3bc066) the selection's link (inside one with nothing
// selected: the link's whole run selected), else the selected text trimmed; OK links the selection
// to +[NSTextView _URLForString:] (0x5b5e50) of what was typed: "http://" before a name with no
// scheme that starts www., web., ftp. or ends .com, .net, .org, .gov, .edu, .mil; an empty field
// or Remove Link (-removeLink: 0x3ba216) takes the link away
void setLink(Window *window, TextView *text);
// The list sheet (Format ▸ List…, the ruler's Lists ▸ Other…): -setMarkerFormatInWindow:textList:
// delegate: (0x3bc18e) shows the selection's list: its marker kind among None and the 26 of
// -loadUI (titled by -_unaffixedMarkerTitle: the bullet, else "1 2 3"), the text before and after
// it, the starting number (shown as its marker, -setStartFieldAndStepper 0x3ba375, disabled for
// bullets) and Prepend enclosing list marker; OK sets the selected paragraphs' list
// (-_doUserSetListMarkerFormat:options:startingItemNumber:), None takes them out of it
void setList(Window *window, TextView *text, std::function<void()> changed = {});
// +[NSTextView _URLForString:]
std::string urlForString(const std::string &s);
}

} // namespace AK
