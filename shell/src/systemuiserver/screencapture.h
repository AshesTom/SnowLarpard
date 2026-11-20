// SPDX-License-Identifier: LGPL-2.1-or-later
// SystemUIServer's ScreenCapture (10.6.8, decrypted: -[ScreenCapture init] 0x1000289a8, the hot keys'
// handler 0x10002978b). It registers the window server's symbolic hot keys 28 to 31 as its own 200,
// 201, 300 and 301 and runs /usr/sbin/screencapture for each, with com.apple.screencapture's settings
// (here ~/.config/snowleopard/screencapture.conf: type, location, name, include-date, disable-shadow,
// capture_cursor):
//   ⇧⌘3   200  the screen to a file           screencapture -fd[C] -t<type> <file>
//   ⌃⇧⌘3  201  the screen to the clipboard    screencapture -cd[C] -t<type>
//   ⇧⌘4   300  a selection or a window to a file   screencapture -id[o] -t<type> <file>
//   ⌃⇧⌘4  301  ...to the clipboard              screencapture -idc[o] -t<type>
// C: capture_cursor; o: disable-shadow. The file (0x100028b2e, 0x100028d71, 0x10002907d): in `location`
// when it is a folder, else the Desktop; "<name> <yyyy-MM-dd> at <the time, medium style>" (the
// Localizable "%@ %@ at %@", name "Screen shot" from ScreenCapture.strings), its ':' made '.' and its
// '/' '-'; "<that> (1)", "(2)"... when it is taken; .<type>.
#pragma once

namespace ScreenCapture {

void install();

}
