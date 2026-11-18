// SPDX-License-Identifier: LGPL-2.1-or-later
// Linux's glue: the Wayland window server's private calls (sl_window_server_v1, the shell's CGS),
// for the programs that on the Mac ask the window server: the Dock (the windows, minimizing into it),
// SystemUIServer (the active application, the screen shots' hot keys), the Finder, loginwindow,
// screencapture. Without Qt or AquaKit: a connection of its own to $WAYLAND_DISPLAY (or one of the
// program's, attach()), its fd watched by the program's event loop (dispatch() when readable).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct wl_display;

namespace WindowServerClient {

enum Flags : uint32_t {
    Minimized = 1, Hidden = 2, Utility = 4, Dialog = 8, SkipTaskbar = 16, Fullscreen = 32, Zoomed = 64, X11Window = 128, Above = 256
};

struct WindowInfo {
    uint32_t id = 0, pid = 0, flags = 0;
    std::string appId, title, menuService, menuPath;
    int x = 0, y = 0, width = 0, height = 0;
};

struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
};

// The window server's connection: true when there is one (the Wayland session's WindowServer).
// connect() makes one of its own, once; attach() binds on the program's own display instead (its
// events then come with the program's)
bool connect();
bool attach(wl_display *display);
bool connected();
int fd();
void dispatch();   // read what came and handle it (the fd readable)
void flush();

// what it says, as of its last done event
const std::vector<WindowInfo> &windows();   // bottom to top
const WindowInfo *window(uint32_t id);
uint32_t active();
uint32_t frontPid();
uint32_t frontSerial();
Rect workArea();
Rect screen();

void activate(uint32_t window);
void close(uint32_t window);
void minimize(uint32_t window);
void hide(uint32_t window);
void setIconGeometry(uint32_t window, int x, int y, int width, int height);
void setFrontApp(uint32_t pid);
void setKeyEquivalents(const std::vector<uint32_t> &keysyms);
void setMagicMirror(const std::string &layer, int direction, const std::vector<int> &quad);
void registerHotKeys(const std::vector<int> &hotKeys, std::function<void(int)> pressed);

// Captures, waited for: a PNG's bytes, empty when it failed; *x, *y where the window's frame is in
// it (its shadow's room)
std::string captureWindow(uint32_t window, bool shadow, int *x = nullptr, int *y = nullptr);
std::string captureScreen(int x, int y, int width, int height);
uint32_t pickWindow(int x, int y);

// Told after each whole change (the done event)
void onChange(std::function<void()> changed);
// The keys for this application while it is in front without a window (set_front_app)
void onKey(std::function<void(uint32_t keysym, const std::string &text, uint32_t modifiers)> key);

} // namespace WindowServerClient
