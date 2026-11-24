// SPDX-License-Identifier: LGPL-2.1-or-later
// VPN.menu's AppleVPNExtra on NetworkMenusCommon's AppleNetworkMenuExtra (networkmenu.h). Reversed
// from 10.6's VPN.menu, x86_64, with REA (docs/reverse-status.md, section 34): VPN0.pdf to VPN4.pdf;
// "VPN is not configured"; VPNShowTime, VPNShowSpecificTime, VPNShowStatus; the services whose
// interface is IPSec, or PPP over L2TP or PPTP (-allServices 0x196c, the IPSec ones first). Linux:
// NetworkManager's vpn and wireguard connections.
#include "networkmenu.h"

class AppleVPNExtra : public NetworkMenus::AppleNetworkMenuExtra {
public:
    explicit AppleVPNExtra(const std::string &bundle) : AppleNetworkMenuExtra(bundle) {}
    std::vector<std::string> imageNameArray() const override { return {"VPN0.pdf", "VPN1.pdf", "VPN2.pdf", "VPN3.pdf", "VPN4.pdf"}; }
    std::string noConfigsKey() const override { return "NO_VPN_CONFIGS"; }
    std::string keyForShowTime() const override { return "VPNShowTime"; }
    std::string keyForShowSpecificTime() const override { return "VPNShowSpecificTime"; }
    std::string keyForShowStatus() const override { return "VPNShowStatus"; }
    bool acceptsType(const std::string &t) const override { return t == "vpn" || t == "wireguard"; }
};

SYSTEMUI_REGISTER_EXTRA("AppleVPNExtra", AppleVPNExtra);
