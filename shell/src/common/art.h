// SPDX-License-Identifier: LGPL-2.1-or-later
// CoreUI's shared art (SArtFile.bin on the DVD): the images CoreUI's renderer draws the menu bar and
// other widgets with, by image ID. The format was read from CUISharedArtReader (CoreUI, 10.6):
//
//   u16 version, u16 count, u32 data       little endian
//   u32 entry[count]                       at 8: entry offsets from the file's start
//   image ID n is entry n - 20000 (n - 20000 > 0)
//   entry + 12 * r: u16 width (+0xC), u16 height (+0xE), u32 length (+0x10), u32 offset (+0x14)
//                                          representation r; the pixels at data + offset
//   pixels: 8 bits a channel, ARGB, premultiplied, rows of width * 4 bytes
//
// docs/reverse-status.md lists where each ID used by the shell comes from.
#pragma once

#include <QImage>

namespace Art {

enum : int {
    MenuBar = 20102,         // CUIRenderer::DrawMenuBar, the usual bar (1 x 21, stretched)
    MenuBarOpaque = 20116,   // ...for the descriptor value 0xC5, with black at 0.55 alpha over it
    MenuTitleSelected = 20103,           // CUIRenderer::DrawMenuTitle, state 0xA8 (Graphite: 20104)
    MenuTitleSelectedGraphite = 20104,
    MenuItemSelected = 20114,            // CUIRenderer::DrawMenuItem, state 0xA8 (Graphite: 20115)
    MenuItemSelectedGraphite = 20115,
    AppleMenuTitle = 20061,              // CUIRenderer::DrawMenuTitle, title kind 0xCF: the Apple menu's
    AppleMenuTitleSelected = 20062,      // ...and when open (state 0xA8)
    // CUIRenderer::DrawDockMenu: nine pieces (as stored, top down), and the callout for each Dock edge
    DockMenuTopLeft = 20074, DockMenuTop = 20075, DockMenuTopRight = 20076,
    DockMenuLeft = 20070, DockMenuCentre = 20069, DockMenuRight = 20072,
    DockMenuBottomLeft = 20065, DockMenuBottom = 20066, DockMenuBottomRight = 20067,
    DockMenuCalloutDown = 20068,         // the Dock at the bottom: the arrow points down at the icon
    DockMenuCalloutUp = 20077, DockMenuCalloutLeft = 20071, DockMenuCalloutRight = 20073,
};

QImage shared(int id, int representation = 0);

// _CUIGetTheme: Graphite when AppleAquaColorVariant (the Appearance pane's, in appearance.conf) is 6;
// read again when the file changes. tint(): the art's token for a tinted state, CUIPath::AppendTint's
// "graphite" or "blue"
bool graphite();
const char *tint();
// The same image as a PNG file in the cache (~/.cache/snowleopard/art), for Qt style sheets.
QString sharedFile(int id);

} // namespace Art
