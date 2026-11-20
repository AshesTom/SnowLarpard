// SPDX-License-Identifier: LGPL-2.1-or-later
// Linux's glue, not a Mac framework: D-Bus (sd-bus) on Foundation's run loop, for what talks to the
// desktop's services: the global menu (com.canonical.AppMenu.Registrar, com.canonical.dbusmenu),
// NetworkManager. The sd-bus API is used as it is; this only gives the buses a run loop.
#pragma once

#include <systemd/sd-bus.h>

namespace DBus {

sd_bus *session();   // null without one
sd_bus *system();
// after a message is sent outside a callback: written now, and what came meanwhile handled
void flush(sd_bus *bus);

} // namespace DBus
