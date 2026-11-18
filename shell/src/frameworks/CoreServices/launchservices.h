// SPDX-License-Identifier: LGPL-2.1-or-later
// LaunchServices (CoreServices.framework) without Qt: the applications and how to open them. On Linux
// the applications are the .desktop entries, and Snow Leopard's stand for the Linux programs that do
// their job (Safari for the web browser, Mail for the mail client...), with the Mac's name and icon; the
// shell's own (System Preferences, Preview...) stand for themselves. The same table as aquashell's
// apps.cpp (the Qt programs'), so both name an application alike.
#pragma once

#include "graphics.h"

#include <cstdint>
#include <string>
#include <vector>

namespace LaunchServices {

struct App {
    std::string id;          // the .desktop file's name without .desktop, or "finder", "mac-<Name>"
    std::string name;        // as shown: the Mac's when it stands for a Mac application
    std::string macBundle;   // "/Applications/Safari.app" when it stands for one
    std::vector<std::string> exec;
    std::string wmClass;     // StartupWMClass, or the guess from the command
    std::string iconName;    // the .desktop entry's Icon
    bool shown = true;       // not NoDisplay
    bool valid() const { return !id.empty(); }
};

const std::vector<App> &all();
App byId(const std::string &id);
App forMacBundle(const std::string &bundle);
// a window's application: by WM_CLASS, instance name, or its process' executable
App forWindow(const std::string &wmClass, const std::string &instance, uint32_t pid);
App forPid(uint32_t pid);
App finder();

AK::Image icon(const App &app, int size);
bool launch(const App &app, const std::vector<std::string> &files = {});
// LSOpenCFURLRef for a file or a URL: PDFs and pictures in Preview, the rest as the desktop opens them
bool open(const std::string &pathOrURL);

// The shell's programs (apps/<Name>.app/Contents/MacOS/<Name>) from any of them: the folder of
// the .app bundles
std::string appsDir();

} // namespace LaunchServices
