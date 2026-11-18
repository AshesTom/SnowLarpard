// SPDX-License-Identifier: LGPL-2.1-or-later
// Bluetooth.menu's code (AppleBluetoothExtra and its DeviceMenuCreator) on AquaKit's SystemUIPlugin,
// loaded by SystemUIServer as on the Mac; on BlueZ (bluetoothctl): the controller's state,
// Discoverable, the devices with their submenus. Its words are the bundle's Localizable.strings. What
// 10.6.8's Bluetooth.menu does is in docs/reverse-status.md (section 17).
#pragma once

#include "systemuiplugin.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class AppleBluetoothExtra : public SystemUI::MenuExtra {
public:
    explicit AppleBluetoothExtra(const std::string &bundle);
    ~AppleBluetoothExtra() override;
    std::vector<AK::MenuItem> menu() override;
    void willUnload() override;

    struct Device {
        std::string address, name, icon;
        std::vector<std::string> uuids;
        bool connected = false;
        int battery = -1;
        int rssi = 0;
        bool has(const char *uuid16) const;
        bool hid() const { return has("1124") || has("1812"); }
        bool stereo() const { return has("110b"); }
        bool headset() const { return has("1108") || has("111e"); }
    };
    AK::Image statusImage(bool white) const;   // -checkAndSetPicture

private:
    void poll();
    void parse(const std::string &out);
    std::vector<AK::MenuItem> items();
    bool deviceItem(const Device &d, AK::MenuItem &out);
    void report();
    std::string tr(const char *key, const char *fallback) const { return localized(key, fallback); }

    std::unique_ptr<AK::Task> m_poll;
    int m_timer = 0;
    bool m_controller = false, m_powered = false, m_discoverable = false;
    std::string m_name, m_address, m_version;
    std::vector<Device> m_devices;
    std::map<std::string, std::string> m_pan;   // NetworkManager's bt devices' states, by address
};
