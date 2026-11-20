// SPDX-License-Identifier: LGPL-2.1-or-later
// AppKit's standard panels and what NSApplication does with a second launch:
//   NSOpenPanel, NSSavePanel   the desktop's own file chooser (kdialog, else zenity), run without
//                              blocking the windows; its answer later
//   NSAlert                    the application's icon, the message in bold, the informative text, the
//                              buttons from the right (the first the default one, a third at the left),
//                              as a window or as a sheet over a document's window
//   -[NSWorkspace openFile:]   xdg-open
//   one instance               a program by user and display: a second launch gives the running one
//                              its files (-[NSApplication application:openFiles:]) and quits
#pragma once

#include "appkit.h"

#include <functional>
#include <string>
#include <vector>

namespace AK {

// A file type for the panels: its name and its patterns ("*.rtf")
struct FileFilter {
    std::string name;
    std::vector<std::string> patterns;
};

namespace OpenPanel {
// The files chosen (none: cancelled)
void run(const std::string &title, const std::string &directory, const std::vector<FileFilter> &filters, bool multiple,
         std::function<void(std::vector<std::string>)> done);
}

namespace SavePanel {
// The path chosen (empty: cancelled); `start` is the folder and the proposed name
void run(const std::string &title, const std::string &start, const std::vector<FileFilter> &filters,
         std::function<void(std::string)> done);
}

namespace Alert {
// NSRunAlertPanel: a window of its own
void run(const Image &icon, const std::string &message, const std::string &informative, std::vector<std::string> buttons,
         std::function<void(int)> done);
// NSBeginAlertSheet: a sheet over `window` (a window already showing one: a window of its own)
void beginSheet(Window *window, const Image &icon, const std::string &message, const std::string &informative,
                std::vector<std::string> buttons, std::function<void(int)> done);
} // namespace Alert

namespace Workspace {
void openFile(const std::string &path);
bool inPath(const char *program);
} // namespace Workspace

namespace Instance {
// The running instance (of `name`, for this user and display) took the files: true
bool handToRunning(const std::string &name, const std::vector<std::string> &files);
// This one listens for the next launches' files (none: just come forward)
void listen(const std::string &name, std::function<void(std::vector<std::string>)> open);
} // namespace Instance

} // namespace AK
