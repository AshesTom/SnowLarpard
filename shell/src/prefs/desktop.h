// SPDX-License-Identifier: LGPL-2.1-or-later
// The desktop picture and the screen saver as System Preferences sets them, without Qt: the same files
// and keys as the shell's Qt programs (common/desktopsettings.h, which the Finder, the Dock and the
// menu bar read): ~/.config/snowleopard/desktop.conf and screensaver.conf. From 10.6's
// DesktopPictures.prefPane (docs/reverse-status.md, section 7).
#pragma once

#include "graphics.h"

#include <string>
#include <vector>

namespace Desktop {

enum Placement { FillScreen = 1, FitToScreen = 2, StretchToFillScreen = 3, Center = 4, Tile = 5 };

struct Settings {
    std::string picture;
    int placement = FillScreen;
    AK::Color color = AK::Color::rgb(0x3D, 0x5B, 0x8C);
    int change = 0;   // 0 never, else an index into changeIntervals()
    bool random = false;
    std::string changeFolder;
    std::vector<std::string> folders;
};

Settings load();
void save(const Settings &s);
std::string picturePath(const Settings &s);
AK::Image render(const Settings &s, AK::Size size);   // the desktop on a screen of that size

struct Source {
    std::string name, path;
};
std::vector<Source> appleSources();   // DSKAppleRootSource::init's folders, in its order
std::vector<std::string> pictures(const std::string &folder);

struct Interval {
    std::string label;
    int seconds;
};
const std::vector<Interval> &changeIntervals();

// A picture's thumbnail (~/.cache/snowleopard/thumbnails), at least `size`
AK::Image thumbnail(const std::string &path, AK::Size size);

} // namespace Desktop

namespace ScreenSaver {
std::string folder();
int idleSeconds();
void setFolder(const std::string &f);
void setIdleSeconds(int s);
} // namespace ScreenSaver
