// SPDX-License-Identifier: LGPL-2.1-or-later
// What the Finder's windows show of the file system (DesktopServicesPriv's TNode for the Mac): a
// folder's items in the Finder's order, their icons from CoreTypes (the folders the Mac names, generic
// documents, icon previews of pictures), the mounted volumes, and the free space.
#pragma once

#include "graphics.h"

#include <string>
#include <vector>

namespace Finder {

struct Item {
    std::string name, path;
    bool folder = false;
    bool application = false;   // a .desktop entry, shown by its Name (the Mac's application bundles)
};

// A folder's visible items (no dot files: the Finder hides them), by name as the Finder sorts them
std::vector<Item> list(const std::string &folder);

// An item's icon at `size` points: the special folders' own (Desktop, Documents...), the generic
// folder and document, applications, and a preview of pictures (the icon view's "Show icon preview")
AK::Image icon(const Item &item, int size, bool preview = true);
AK::Image coreTypesIcon(const std::string &name, int size);   // CoreTypes.bundle's <name>.icns

// The window's title for a folder: its name, the home folder's the user's short name, "/" the
// startup disk's name
std::string displayName(const std::string &path);

// The mounted volumes (the Mac's /Volumes): the startup disk first ("/" by its label), then the
// removable and external disks the desktop mounted
struct Volume {
    std::string name, path;
    enum Kind { Internal, External, Removable } kind = Internal;
};
std::vector<Volume> volumes();
AK::Image volumeIcon(const Volume &v, int size);

// The list view's columns: the Kind (LaunchServices': the default application's name for the type,
// else CoreTypes' description, else "%@ Document"), the size on disk, the modification time
std::string kind(const Item &item);
long long sizeOnDisk(const Item &item);   // -1 when unknown
long long modificationTime(const Item &item);
long long creationTime(const Item &item);   // statx's birth time, else the modification time
long long accessTime(const Item &item);     // Last Opened
// TDateFormatter's styles; relative: "Today, 4:20 PM", "Yesterday, ..."
enum DateStyle { DateFull, DateLong, DateMedium, DateShort };
std::string formatDate(long long time, DateStyle style, bool relative);

// The free space of the volume holding `path`, in bytes
long long freeSpace(const std::string &path);
// The Finder's sizes (NSLocalizedFileSizeDescription as TFileSizeFormatter asks): "Zero KB", "12 KB",
// "1.4 MB", "45.67 GB"
std::string formatBytes(long long bytes);

// The folders of the user the sidebar's PLACES lists (the xdg ones, as the Mac's)
std::string userDir(const std::string &xdgName, const std::string &fallback);

} // namespace Finder
