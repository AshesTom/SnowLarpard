// SPDX-License-Identifier: LGPL-2.1-or-later
// Stickies' database, ~/Library/StickiesDatabase (docs/reverse-status.md, section 16): what
// -[NoteController saveAllDocumentsMenuAction:] archives with NSArchiver, an NSMutableArray of the
// notes front to back, each a `Document` (class version 1) encoded by -[Document encodeWithCoder:]:
//   @ the RTFD (NSData) | i the flags | {?={?=ff}{?=ff}} the window's frame | i the colour | @ @ the
//   creation and modification NSDates
// (at class version 0 a `c` before them, -[Document initWithCoder:]).
//
// NSArchiver's typedstream: 04 0b "streamtyped" (little endian, as an Intel Mac writes it) or
// "typedstream" (big endian), the system's version (1000), then groups, each its type string (shared)
// then its values. Integers: a byte, 0x81 + 2 bytes, 0x82 + 4; floats and doubles 0x83 + 4 or 8 bytes,
// or an integer when whole; 0x84 a new entry, 0x85 nil, 0x86 an object's end; anything else a
// reference (value + 110) to an earlier string, or object or class (one table for both). An object:
// 0x84, its class (0x84, the name, the version, its superclass... down to nil), its groups, 0x86.
// NSArray: i the count, then each object a group of its own; NSData: i the length, [Nc] the bytes;
// NSDate: d the seconds since 2001.
#pragma once

#include <string>
#include <vector>

namespace Stickies {

struct Note {
    std::string rtfd;           // the flattened RTFD (rtfd.h)
    int flags = 0;              // 1 collapsed, 2 floating, 4 translucent
    double x = 100, y = 100;    // the window's frame on the screen, from its bottom left (Cocoa's)
    double width = 300, height = 200;
    int colour = 0;             // 0 Yellow ... 5 Gray
    double created = 0, modified = 0;   // seconds since 1 January 2001 (NSDate's reference date)
};

enum Flags { Collapsed = 1, Floating = 2, Translucent = 4 };

// The notes of a database file, front to back; false when it isn't one
bool readDatabase(const std::string &path, std::vector<Note> &notes);
bool readDatabase(const std::string &bytes, std::vector<Note> &notes, bool fromBytes);
// As NSArchiver archives them (written to a temporary file, then renamed over the old one)
std::string writeDatabase(const std::vector<Note> &notes);
bool saveDatabase(const std::string &path, const std::vector<Note> &notes);

double nowSinceReferenceDate();

} // namespace Stickies
