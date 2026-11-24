// SPDX-License-Identifier: LGPL-2.1-or-later
// PPP.menu's PPPConnectExtra on NetworkMenusCommon's AppleNetworkMenuExtra (networkmenu.h). Reversed
// from 10.6's PPP.menu, x86_64, with REA (docs/reverse-status.md, section 34): PPP0.pdf to PPP4.pdf;
// "No modem configurations were found"; PPPShowTime, PPPShowSpecificTime, PPPShowStatus; the
// services whose interface is PPP over a modem or Bluetooth (-allServices 0x19f0), configured.
// Linux: NetworkManager's mobile broadband (gsm, cdma) and Bluetooth connections.
#include "networkmenu.h"

class PPPConnectExtra : public NetworkMenus::AppleNetworkMenuExtra {
public:
    explicit PPPConnectExtra(const std::string &bundle) : AppleNetworkMenuExtra(bundle) {}
    std::vector<std::string> imageNameArray() const override { return {"PPP0.pdf", "PPP1.pdf", "PPP2.pdf", "PPP3.pdf", "PPP4.pdf"}; }
    std::string noConfigsKey() const override { return "NO_MODEM_CONFIGS"; }
    std::string keyForShowTime() const override { return "PPPShowTime"; }
    std::string keyForShowSpecificTime() const override { return "PPPShowSpecificTime"; }
    std::string keyForShowStatus() const override { return "PPPShowStatus"; }
    bool acceptsType(const std::string &t) const override { return t == "gsm" || t == "cdma" || t == "bluetooth"; }
};

SYSTEMUI_REGISTER_EXTRA("PPPConnectExtra", PPPConnectExtra);
