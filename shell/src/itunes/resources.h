// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's own resources, read from the DVD's files at run time (docs/itunes-reverse.md):
//   iTunes.rsrc                 the art: PNG resources by ID
//   English.lproj/Localized.rsrc the menus (MBAR 129 and its xMNU resources), the text styles (Txtr)
//   English.lproj/Localizable.strings the menus' and windows' text, by key
// and the pictures' grid of states, as iTunes' own table at 0xa622e0 has them (columns, rows, fixed
// left and right sizes, the state mapping kind), for the pictures the window draws.
#pragma once

#include "graphics.h"
#include "mainmenu.h"
#include "text.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace iTunes {

const char *const Bundle = "/Applications/iTunes.app";

// A data-fork resource file (the Resource Manager's format): resources by type and ID
class ResourceFile {
public:
    bool open(const std::string &path);
    const std::string *get(const std::string &type, int id) const;
    std::vector<int> ids(const std::string &type) const;
    bool isOpen() const { return !m_res.empty(); }

private:
    std::map<std::pair<std::string, int>, std::string> m_res;
};

ResourceFile &art();         // iTunes.rsrc
ResourceFile &localized();   // English.lproj/Localized.rsrc
bool resourcesPresent();

// Localizable.strings
std::string tr(const std::string &key);

// A PNG resource as an image (cached); null when missing
AK::Image picture(int id);

// The picture table's entry (FUN_0000ebe4): its grid of states and its fixed sides
struct PictureInfo {
    int columns = 1, rows = 1;
    int fixedLeft = 0, fixedRight = 0;
    int kind = 0;   // FUN_0002b9f0's state to column mapping
};
PictureInfo pictureInfo(int id);

// A button's state as FUN_0002c5f8 and its callers give it: 0 disabled, 1 normal, 2 pressed, 4 disabled
// in an inactive window, 5 enabled in an inactive window
int stateColumn(int kind, int state);

// One cell of a picture (column, row) into dst: the fixed left and right sides kept, the middle
// stretched across (FUN_0002a3d6, FUN_0002a75a)
void drawCell(cairo_t *cr, int id, int column, int row, const AK::Rect &dst, double alpha = 1);
// A picture's cell size
AK::Size cellSize(int id);

// A Txtr resource: size, style, justification, colour, font name ("LCD" is 302)
struct TextStyle {
    AK::Font font;
    AK::Align align = AK::Align::Left;
    AK::Color color;
};
TextStyle textStyle(int id);

// The menu bar from MBAR 129 and its xMNU resources (FUN_0026c16c, FUN_0026be3e): each item's text
// (Localizable.strings), key, modifiers and command (its 4-char code as the action); submenus by the
// classic hMenuCmd convention; the hidden "Commands" menu (160) for its key equivalents
std::vector<AK::MainMenuItem> menuBar();
// One xMNU's items (a contextual menu, the Dock menu)
std::vector<AK::MainMenuItem> menuItems(int id);

} // namespace iTunes
