// SPDX-License-Identifier: LGPL-2.1-or-later
// Linux's glue, not a Mac framework: the session's windows as the window manager lists them (EWMH,
// WindowServer's or another's), without Qt, for what on the Mac asks the window server
// (CGWindowList, the Process Manager's hide and show): SystemUIServer's menu bar, later the Dock.
// A connection of its own to the X display, watched on Foundation's run loop.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <xcb/xcb.h>

namespace Windows {

bool open();   // the display ($DISPLAY); false without one
xcb_connection_t *conn();
xcb_window_t root();
xcb_atom_t atom(const char *name);

std::vector<uint32_t> cardinals(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type);
std::string bytes(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type);
bool hasType(xcb_window_t w, const char *type);   // "_NET_WM_WINDOW_TYPE_DESKTOP"
bool hasState(xcb_window_t w, const char *state);  // "_NET_WM_STATE_SKIP_TASKBAR"

bool hasWindowManager();
// The applications' windows, bottom to top (not the shell's docks, desktops and menus)
std::vector<xcb_window_t> clientWindows();
xcb_window_t activeWindow();
std::string wmClass(xcb_window_t w);      // the class part ("firefox")
std::string wmInstance(xcb_window_t w);   // the instance part
std::string title(xcb_window_t w);
uint32_t pid(xcb_window_t w);
bool minimized(xcb_window_t w);
bool hidden(xcb_window_t w);   // hidden by ⌘H (_SNOWLEOPARD_HIDDEN)

void activate(xcb_window_t w);
void close(xcb_window_t w);
void minimize(xcb_window_t w);
void hide(xcb_window_t w);

// Told (on the run loop, coalesced) when the root's windows, the active one, their stacking or one of
// `atoms` on the root changed, or a window watched with watchDestroy() went
void onChange(std::function<void()> changed, const std::vector<std::string> &rootAtoms = {});
void watchDestroy(xcb_window_t w);

} // namespace Windows
