// SPDX-License-Identifier: LGPL-2.1-or-later
// NSFontPanel (Format ▸ Font ▸ Show Fonts, ⌘T), as AppKit-1038 (10.6) lays it out in
// NSFontPanel.nib, its main box with the effects bar and the preview hidden:
//   - four columns with their headers (FontManager.strings): Collection (All Fonts, Favorites,
//     Recently Used, then the collections: Fixed Width), Family, Typeface, Size: its field over the
//     list of sizes (NSFontPanelSizeList, by default 9 10 11 12 13 14 18 24 36 48 64 72 96 144 288:
//     -_createFontPanelSizeRep 0x3cf4b8) and the slider beside it (9 to 72)
//   - at the bottom + and − (collections, not here), the action menu (Add to Favorites; the others
//     not here) and the search field (the families whose name has the text)
//   - choosing a family keeps the typeface's name when the new family has it; a family, typeface or
//     size is sent to the text at once (-changeFont:, NSFontManager's convertFont: on each run:
//     _chooseCollection: 0x3d00b6, _chooseSizeFromList: 0x3ce0a7, _chooseSizeFromField: 0x3cdfe5,
//     _chooseSizeFromSlider: 0x3cdce5); the families used go to Recently Used
//   - the panel follows the selection's font (-[NSFontManager setSelectedFont:isMultiple:])
// The families and typefaces are fontconfig's (pango's), sorted as NSFontManager does: families by
// name, typefaces by weight then slant.
#pragma once

#include "appkit.h"
#include "textview.h"

#include <functional>

namespace AK {

namespace FontPanel {
// The text the panel acts on: the key window's (asked each time)
void setTarget(std::function<TextView *()> target);
// The effects bar's document colour: the document's background, asked and set
void setDocumentColor(std::function<Color()> get, std::function<void(const Color &)> set);
// -orderFrontFontPanel:
void show();
bool isVisible();
}

} // namespace AK
