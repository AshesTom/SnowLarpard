// SPDX-License-Identifier: LGPL-2.1-or-later
// NetworkMenusCommon.framework on AquaKit: AppleNetworkMenuExtra, its service controllers and its menu
// bar view, the base of the PPP, PPPoE and VPN menu extras. Reversed from 10.6's NetworkMenusCommon,
// x86_64, with REA (docs/reverse-status.md, section 34). Compiled into each of the three bundles (the
// framework's one copy on the Mac).
//
// On Linux the services are NetworkManager's connections (nmcli): a subclass says which types it
// shows, as its -allServices picks the SCNetworkServices of its interface types.
#pragma once

#include "systemuiplugin.h"

#include <ctime>
#include <string>
#include <vector>

namespace NetworkMenus {

// SCNetworkConnectionGetStatus
enum Status { Invalid = -1, Disconnected = 0, Connecting = 1, Connected = 2, Disconnecting = 3 };

// AppleNetworkMenuExtraServiceController: a service and its connection
struct Service {
    std::string uuid, name, type;
    Status status = Disconnected;
    std::time_t start = 0;            // -connectionStartTime
    bool shouldShowTimer = false;    // the header shows the time (the bar doesn't)
};

class AppleNetworkMenuExtra : public SystemUI::MenuExtra {
public:
    explicit AppleNetworkMenuExtra(const std::string &bundle);
    ~AppleNetworkMenuExtra() override;

    std::vector<AK::MenuItem> menu() override;   // -createMenu

    // the subclasses' (-imageNameArray, -localizedNoConfigsString, the preference keys,
    // -networkPrefSectionName, -allServices' interface types)
    virtual std::vector<std::string> imageNameArray() const = 0;
    virtual std::string noConfigsKey() const = 0;
    virtual std::string keyForShowTime() const = 0;
    virtual std::string keyForShowSpecificTime() const = 0;
    virtual std::string keyForShowStatus() const = 0;
    virtual bool acceptsType(const std::string &nmType) const = 0;

    // the view's
    const std::vector<Service> &services() const { return m_services; }
    const Service *barService() const;   // the menu bar view's service controller
    bool showTimer() const { return m_showTimer; }
    bool showAnimatedStatus() const { return m_showStatus; }
    const std::vector<AK::Image> &images(bool white) const { return white ? m_white : m_images; }
    std::string text(const std::string &key) const { return localized(key, key); }

private:
    void refresh();   // the dynamic store's notifications: the connections read again
    void prefsChanged();
    static void storeValue(const std::string &key, const std::string &value);

    std::vector<Service> m_services;
    std::vector<AK::Image> m_images, m_white;
    std::string m_barUuid;
    bool m_showTimer = false, m_showStatus = true;
    int m_poll = 0;
};

} // namespace NetworkMenus
