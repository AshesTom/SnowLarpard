// SPDX-License-Identifier: LGPL-2.1-or-later
// CoreUI's ArtFile.bin: the widgets' art (the windows' buttons, the controls), by path. The format was
// read from ArtFileSetupOnce and CUIElement::LoadFromArtFile (CoreUI, 10.6):
//
//   u16 elements, u16 key length (8), u16 tokens, u16 -       little endian
//   u32 token table, u32 token names, u32 path table, u32 data
//   token: u32 name offset (into the names), u8 value, 3 bytes
//   path entry (key length + 4): u32 record offset (from data), key: token values, the widget's first,
//                                then the others ascending, 0-padded
//   record (0x68): u16 rows, u16 columns, margins; +0x20 u32 piece offsets (from the record);
//                  +0x44 u16 piece widths; +0x56 u16 piece heights; row by row
//   pieces: 8 bits a channel, ARGB, premultiplied, as in SArtFile.bin
//
// A path is the widget's token, then the others in any order, e.g. {"titlebarcontrols", "close",
// "colorsryg", "regularwin"}. The window buttons' states: none (normal), "rollover" (the glyphs),
// "pressed", "activenokey" (the window isn't key), "inactive", "disabled".
#pragma once

#include <QImage>
#include <QStringList>
#include <QVector>

namespace ArtFile {

struct Element {
    int rows = 0, columns = 0;
    QVector<QImage> pieces;   // rows x columns, row by row
    bool isNull() const { return pieces.isEmpty(); }
    QImage single() const { return pieces.value(0); }
};

Element element(const QStringList &path);

} // namespace ArtFile
