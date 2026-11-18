// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dbus.h"

#include "runloop.h"

#include <cstdint>

namespace DBus {

namespace {

void process(sd_bus *bus)
{
    while (sd_bus_process(bus, nullptr) > 0) {
    }
    sd_bus_flush(bus);
}

sd_bus *attach(sd_bus *bus)
{
    if (!bus)
        return nullptr;
    AK::RunLoop::main().watch(sd_bus_get_fd(bus), [bus] { process(bus); });
    // sd-bus' own timeouts (a call without an answer) and what a blocking call read ahead
    AK::RunLoop::main().addTimer(0.1, true, [bus] { process(bus); });
    return bus;
}

} // namespace

sd_bus *session()
{
    static sd_bus *bus = [] {
        sd_bus *b = nullptr;
        return sd_bus_open_user(&b) >= 0 ? attach(b) : nullptr;
    }();
    return bus;
}

sd_bus *system()
{
    static sd_bus *bus = [] {
        sd_bus *b = nullptr;
        return sd_bus_open_system(&b) >= 0 ? attach(b) : nullptr;
    }();
    return bus;
}

void flush(sd_bus *bus)
{
    if (bus)
        AK::RunLoop::main().later([bus] { process(bus); });
}

} // namespace DBus
