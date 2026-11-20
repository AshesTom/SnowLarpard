// SPDX-License-Identifier: LGPL-2.1-or-later
// AirPort.menu's code (AirPortExtra, AirPortExtraImplementation) on AquaKit's SystemUIPlugin, loaded by
// SystemUIServer as on the Mac; on NetworkManager (nmcli, and D-Bus for joining): the bars of the
// network joined, the networks found, Turn AirPort Off, Join Other Network…, Create Network…. Its words
// are the bundle's Localizable.strings. What 10.6.8's AirPort.menu does is in docs/reverse-status.md
// (section 17).
#pragma once

#include "systemuiplugin.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct sd_bus_slot;

class AirPortExtra : public SystemUI::MenuExtra {
public:
    explicit AirPortExtra(const std::string &bundle);
    ~AirPortExtra() override;
    std::vector<AK::MenuItem> menu() override;
    void menuDidClose() override;
    void willUnload() override;

    struct Network {
        std::string ssid, security, bssid;
        int signal = 0;   // NetworkManager's 0-100
        int rssi = 0;     // dBm, from the driver (iw) for the network joined
        int channel = 0, frequency = 0, rate = 0;
        bool active = false, adhoc = false;
    };
    // AME_AirPortNetworksOrganizer +barsForSignal: (RSSI in dBm) and +barsForMappedTransmitRate: (Mbit/s)
    static int barsForSignal(int rssi);
    static int barsForMappedTransmitRate(int rate);
    static int rssiForQuality(int quality);

    AK::Image statusImage(bool white) const;   // -updateStatusImage

    // NetworkManager's answers (the bus' callbacks)
    void joinStateChanged(uint32_t state, uint32_t reason);
    void joinAdded(const std::string &settings, const std::string &active);
    void panelFailed(const std::string &error);

private:
    void poll(bool rescan);
    void parse(const std::string &out);
    std::vector<AK::MenuItem> items();
    void networks(std::vector<AK::MenuItem> &out);
    AK::MenuItem networkItem(const Network &n, bool current, int bars);
    void join(const Network &n);
    void runPanel(const std::vector<std::string> &args);
    void panelAccepted(const std::map<std::string, std::string> &fields);
    void finishJoin(const std::string &error);
    std::string tr(const char *key, const char *fallback) const { return localized(key, fallback); }

    std::unique_ptr<AK::Task> m_poll, m_panel;
    std::string m_panelOutput;
    bool m_panelHidden = false;
    int m_pollTimer = 0, m_animation = 0, m_spinner = 0, m_scanTimer = 0, m_joinTimeout = 0;
    double m_animationStart = 0;
    int m_frame = 0, m_spin = 0;
    struct Link {
        int rssi = 0, rate = 0, mcs = -1;
        std::string phy;
    } m_link;
    bool m_hardware = false, m_powerOn = false, m_connecting = false, m_scanning = false, m_showScanning = false;
    std::string m_device, m_connectivity;
    std::vector<Network> m_networks;
    std::vector<std::string> m_profiles;
    std::string m_joinSettings, m_joinActive;
    sd_bus_slot *m_joinSlot = nullptr;
    int m_txSamples[3] = {0, 0, 0};
    int m_bars = 0;
};
