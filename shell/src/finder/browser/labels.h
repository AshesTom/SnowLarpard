// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's labels (File > Label:, the contextual menu's swatches, Preferences > Labels): an item's
// label is bits 1-3 of its FinderInfo's fdFlags (big-endian, bytes 8-9 of the 32 bytes of
// com.apple.FinderInfo), kept here in the extended attribute user.com.apple.FinderInfo. The colours
// and names are HIServices' (CopyLabelColorAndName: _sCachedColors at 0x57a60, RGBColor's 16 bits
// a channel; the names Labels.strings' Label_Name_0-7), the user's own from com.apple.Labels
// (Label_Color_N, Label_Name_N), here finder.conf's. The index: 0 none, 1 gray, 2 green, 3 purple,
// 4 blue, 5 yellow, 6 red, 7 orange; the menus' order Red, Orange, Yellow, Green, Blue, Purple, Gray
// (TLabelsPaneController -labelIndexForOrder:).
#pragma once

#include "graphics.h"

#include <string>

namespace Finder {

namespace Labels {

constexpr int Order[7] = {6, 7, 5, 2, 4, 3, 1};
int get(const std::string &path);              // 0 none
bool set(const std::string &path, int index);  // false when the file system won't keep it
AK::Color color(int index);
std::string name(int index);
void setName(int index, const std::string &name);
void forget(const std::string &path);          // the cache's entry (the item renamed, moved)

} // namespace Labels

} // namespace Finder
