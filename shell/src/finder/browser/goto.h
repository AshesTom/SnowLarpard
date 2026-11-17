// SPDX-License-Identifier: LGPL-2.1-or-later
// Go > Go to Folder… (⇧⌘G, TGotoWindowController) and Go > Connect to Server… (⌘K,
// TConnectToWindowController), from their nibs:
//  - GotoWindow.nib: a sheet on the front window ("Go to the folder:", the path field TGotoPathField,
//    Cancel, Go); Tab completes the path (-autocompletePath:), "~" is the home folder, the last path
//    is offered again; a path that isn't a folder: "The folder can’t be found." (A14.1) under the field
//  - ConnectToWindow.nib: the server address, the + that adds it to Favorite Servers, the recent
//    servers' menu, Remove, Browse, Connect. Linux's GIO mounts it (smb://, afp://, ftp://, sftp://,
//    dav://...), and the Finder opens the mounted volume.
#pragma once

#include <functional>
#include <string>

namespace AK {
class Window;
}

namespace Finder {

class GoTo {
public:
    // the sheet on `window` (a window of its own without one); go(folder) when a folder is chosen
    static void show(AK::Window *window, double sheetTop, std::function<void(const std::string &)> go);
    // the path's completion: the folder part kept, the first name in it that starts with the rest
    static std::string complete(const std::string &typed);
    // "~/Documents" -> "/home/me/Documents"; a trailing "/" gone
    static std::string expand(const std::string &typed);
};

class ConnectToServer {
public:
    // the window (brought forward when open); opened(folder) once a server is mounted
    static void show(std::function<void(const std::string &)> opened);
};

} // namespace Finder
