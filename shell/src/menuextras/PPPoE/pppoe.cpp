// SPDX-License-Identifier: LGPL-2.1-or-later
// PPPoE.menu's PPPoEConnectExtra on NetworkMenusCommon's AppleNetworkMenuExtra (networkmenu.h).
// Reversed from 10.6's PPPoE.menu, x86_64, with REA (docs/reverse-status.md, section 34):
// PPPOEDisconnected.pdf and PPPOE.pdf; "No PPPoE configurations were found"; PPPoEShowTime,
// PPPoEShowSpecificTime, PPPoEShowStatus; the services whose interface is PPP over Ethernet
// (-allServices 0x9f4). Linux: NetworkManager's pppoe connections.
#include "networkmenu.h"

class PPPoEConnectExtra : public NetworkMenus::AppleNetworkMenuExtra {
public:
    explicit PPPoEConnectExtra(const std::string &bundle) : AppleNetworkMenuExtra(bundle) {}
    std::vector<std::string> imageNameArray() const override { return {"PPPOEDisconnected.pdf", "PPPOE.pdf"}; }
    std::string noConfigsKey() const override { return "NO_PPPOE_CONFIGS"; }
    std::string keyForShowTime() const override { return "PPPoEShowTime"; }
    std::string keyForShowSpecificTime() const override { return "PPPoEShowSpecificTime"; }
    std::string keyForShowStatus() const override { return "PPPoEShowStatus"; }
    bool acceptsType(const std::string &t) const override { return t == "pppoe"; }
};

SYSTEMUI_REGISTER_EXTRA("PPPoEConnectExtra", PPPoEConnectExtra);
