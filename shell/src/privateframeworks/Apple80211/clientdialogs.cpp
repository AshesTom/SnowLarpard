// SPDX-License-Identifier: LGPL-2.1-or-later
// Apple80211.framework's ClientDialogs.bundle on AquaKit: AirPort's panels from its nibs and strings (the
// DVD's), a program at PrivateFrameworks/Apple80211.framework/Resources/ClientDialogs.bundle:
// the AirPort menu extra (SystemUIServer, airport.cpp) starts this program for a panel and joins the
// network with what it says. Reversed from 10.6.8's ClientDialogs (x86_64):
//
//   AirPortWindowController -showAuthViewForSecurityType: (0xcc2d): the security type's view under the
//   panel's text: none (AirPortAuthViewEmpty), AirPortAuthViewBasic.nib (a password, "Show password")
//   for WEP and WPA/WPA2 Personal (AirPortAuthViewPSK.nib's 64 hex digits with the Option key),
//   AirPortAuthViewLEAP.nib, AirPortAuthViewWPAEnterprise.nib (user name, password, 802.1X profile).
//   -setAuthView: (0xc904) puts it at the origin of the hidden line _wepLimitLine, the window as much
//   taller: the views at and under the line (their autoresizing: the bottom margin kept) go down.
//   AirPortClientDialogPassword -preparePanelToRun (0x7d0c): "The network “%@” requires a WPA / WEP
//   password." in _networkNameText, _networkMessageText and Ask Permission… hidden, Remember this
//   network checked as the interface remembers networks.
//   AirPortClientDialogJoinNetwork -preparePanelToRun (0x4a60): the security pop-up's types (None, WEP
//   Password, WEP 40/128-bit hex and ASCII, LEAP, —, WPA Personal, WPA Enterprise, WPA2 Personal, WPA2
//   Enterprise, 802.1X WEP; tags 0-9), the "Manual" tab (the network's name), _wepTypeView (the
//   pop-up) put at the line as -showSecurityTypeView: (0x518c) puts it.
//   AirPortClientDialogCreateIBSS: the computer's name (32 bytes at most), "Automatic (11)" then the
//   2.4 GHz channels and those of 5 GHz to 48 (-populateIBSSChannelPopup 0x7451), Require Password
//   showing the nib's _authViewBasic (password, verify, 40 or 128-bit WEP: 5 or 13 ASCII characters,
//   10 or 26 hex digits, -refreshUI 0x61dd).
//   OK: the progress wheel and "Connecting to network…", then the panel closes, or says why it failed
//   (kAirPortConnectionFailed*), the warning button beside it.
//
//   ClientDialogs password SSID TYPE    TYPE: the security type's tag (1 WEP, 5/7 WPA/WPA2
//                                              Personal, 4 LEAP, 6/8 Enterprise, 9 802.1X WEP)
//   ClientDialogs join                  Join Other Network…
//   ClientDialogs create                Create Network…
//   --option after them: the Option key was down (the PSK view for WPA Personal)
//
// OK writes "key=value" lines (values with %, newline and = escaped as %XX) and "end" on its standard
// output, then waits for "done" (it quits) or "error <message>" on its standard input; Cancel quits
// with status 1.
#include "controls.h"
#include "coreui.h"
#include "foundation.h"
#include "graphics.h"
#include "nib.h"

#include <fcntl.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace AK;

namespace {

const std::string Bundle = "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/A/Resources/ClientDialogs.bundle";
const std::string Controller = "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/A/Resources/ClientController.bundle";

std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "Localizable", key, fallback); }
std::string trController(const std::string &key, const std::string &fallback) { return Assets::tr(Controller, "Localizable", key, fallback); }

std::string format(std::string pattern, const std::string &arg)
{
    const size_t at = pattern.find("%@");
    if (at != std::string::npos)
        pattern.replace(at, 2, arg);
    return pattern;
}

std::unique_ptr<Nib> loadNib(const std::string &name)
{
    auto nib = std::make_unique<Nib>();
    const std::string res = Bundle + "/Contents/Resources/";
    if (!nib->load(Assets::path(res + "English.lproj/" + name + ".nib/keyedobjects.nib"), Assets::path(res)))
        return nullptr;
    return nib;
}

// -getLocalizedNameOfSecurityType: (0xbde0), ClientController's names
std::string securityName(int tag)
{
    static const char *const keys[][2] = {
        {"kAirPortSecurityTypeNoneKey", "None"},
        {"kAirPortSecurityTypeWEPKey", "WEP Password"},
        {"kAirPortSecurityTypeWEPHexKey", "WEP 40/128-bit hex"},
        {"kAirPortSecurityTypeWEPASCIIKey", "WEP 40/128-bit ASCII"},
        {"kAirPortSecurityTypeLEAPKey", "LEAP"},
        {"kAirPortSecurityTypeWPAPersonalKey", "WPA Personal"},
        {"kAirPortSecurityTypeWPAEnterpriseKey", "WPA Enterprise"},
        {"kAirPortSecurityTypeWPA2PersonalKey", "WPA2 Personal"},
        {"kAirPortSecurityTypeWPA2EnterpriseKey", "WPA2 Enterprise"},
        {"kAirPortSecurityType8021XWEPKey", "802.1X WEP"},
    };
    return tag >= 0 && tag < 10 ? trController(keys[tag][0], keys[tag][1]) : std::string();
}

bool allHex(const std::string &s)
{
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string escape(const std::string &s)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '%' || c == '\n' || c == '=' || c == '\r') {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        } else {
            out += char(c);
        }
    }
    return out;
}

// A panel: a nib's window, the views put at its line, the auth view's fields
class Panel {
public:
    enum Kind { Password, Join, Create };

    Panel(Kind kind, const std::string &ssid, int type, bool option) : m_kind(kind), m_ssid(ssid), m_type(type), m_option(option) {}

    bool open()
    {
        static const char *const names[] = {"AirPortClientDialogPassword", "AirPortClientDialogJoinNetwork",
                                            "AirPortClientDialogCreateIBSS"};
        m_nib = loadNib(names[m_kind]);
        if (!m_nib)
            return false;
        const auto windows = m_nib->windows();
        if (windows.empty())
            return false;
        m_root = m_nib->instantiateWindow(windows.front().object, &m_size);
        for (View *v : m_root->subviews())
            m_original[v] = v->frame();
        if (auto *line = m_nib->outlet<View>("_wepLimitLine"))
            m_lineY = line->frame().y;
        else
            m_lineY = m_size.h;

        // the AirPort icon at the 64 points of its view (the nib's images are read at 32)
        if (auto *icon = m_nib->outlet<ImageView>("_airPortIcon")) {
            Image big = Image::load(Assets::path(Bundle + "/Contents/Resources/AirPort.icns"), {64, 64});
            if (!big.isNull())
                icon->image = big;
        }
        hide("_progressIndicator");
        hide("_progressLabel");
        hide("_warningButton");
        if (auto *ok = m_nib->outlet<Button>("_okButton")) {
            ok->isDefault = true;
            ok->action = [this] { accept(); };
        }
        if (auto *cancel = m_nib->outlet<Button>("_cancelButton"))
            cancel->action = [] { App::shared().quit(1); };

        if (m_kind == Password)
            preparePassword();
        else if (m_kind == Join)
            prepareJoin();
        else
            prepareCreate();

        m_window = std::make_unique<Window>(m_size, "");
        m_window->contentView()->addSubview(m_root);
        m_window->onClose = [] { App::shared().quit(1); };
        m_window->onUnhandledKey = [this](Event &e) {
            if (e.keysym == XKB_KEY_Escape)
                App::shared().quit(1);
            else if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter)
                accept();
        };
        // a panel that doesn't activate SystemUIServer (an LSUIElement): above the windows, out of the
        // Dock, the menu bar left to the application in front
        m_window->setFloatingPanel();
        layout();
        m_window->show();
        const Rect screen = App::shared().screenFrame();
        m_window->moveTo({std::round((screen.w - m_size.w) / 2), std::round(screen.h / 3 - m_size.h / 3)});
        if (m_first)
            m_window->makeFirstResponder(m_first);
        refresh();
        App::shared().watch(0, [this] { readCommand(); });
        return true;
    }

private:
    void hide(const char *outlet)
    {
        if (auto *v = m_nib->outlet<View>(outlet))
            v->setHidden(true);
    }

    // ---- the views at the line (-setAuthView:, -showSecurityTypeView:)

    struct Inserted {
        View *view;
        double height;
    };

    // the views at the line, in order, the window as much taller; the nib's views under it lower
    void layout()
    {
        double extra = m_tabExtra;
        for (const Inserted &i : m_inserted)
            extra += i.height;
        for (auto &[view, frame] : m_original) {
            Rect f = frame;
            if (f.y >= m_lineY - 0.5)
                f.y += extra;
            else if (view == m_tabs)
                f.h += m_tabExtra;   // -showNetworkTypeTable: the table's 100 points, the name's 23
            view->setFrame(f);
        }
        double y = m_lineY + m_tabExtra;
        for (const Inserted &i : m_inserted) {
            Rect f = i.view->frame();
            i.view->setFrame({f.x, y, f.w, i.height});
            y += i.height;
        }
        const Size size{m_size.w, m_size.h + extra};
        m_root->setFrame({0, 0, size.w, size.h});
        if (m_window && (size.w != m_window->size().w || size.h != m_window->size().h)) {
            m_window->setContentSize(size);
            m_window->setFloatingPanel();   // its fixed size, the new one
        }
    }

    void insert(View *v, size_t at)
    {
        m_root->addSubview(v);
        m_inserted.insert(m_inserted.begin() + long(std::min(at, m_inserted.size())), {v, v->frame().h});
    }

    void removeInserted(View *v)
    {
        auto it = std::find_if(m_inserted.begin(), m_inserted.end(), [v](const Inserted &i) { return i.view == v; });
        if (it == m_inserted.end())
            return;
        m_inserted.erase(it);
        m_root->removeSubview(v);
    }

    // -showAuthViewForSecurityType: (0xcc2d): the auth view for the type, after the views before it
    void showAuthView(int type)
    {
        if (m_auth) {
            removeInserted(m_auth);
            m_auth = nullptr;
        }
        m_authNib.reset();
        m_password = m_username = m_verify = nullptr;
        m_reveal = nullptr;
        m_authType = type;
        const bool option = m_option;   // the PSK view (64 hex digits) with the Option key down
        std::string name;
        if (type >= 1 && type <= 3)
            name = "AirPortAuthViewBasic";
        else if (type == 5 || type == 7)
            name = option ? "AirPortAuthViewPSK" : "AirPortAuthViewBasic";
        else if (type == 4)
            name = "AirPortAuthViewLEAP";
        else if (type == 6 || type == 8 || type == 9)
            name = "AirPortAuthViewWPAEnterprise";
        if (!name.empty()) {
            m_authNib = loadNib(name);
            if (m_authNib) {
                m_auth = m_authNib->instantiateWindow(m_authNib->outletObject("_view"), nullptr);
                if (m_auth) {
                    m_auth->setFrame({0, 0, m_size.w, m_auth->frame().h});
                    insert(m_auth, m_inserted.size());
                    m_password = m_authNib->outlet<TextField>("passwordTextField");
                    if (!m_password)
                        m_password = m_authNib->outlet<TextField>("_preSharedKeyField");
                    if (!m_password)
                        m_password = m_authNib->outlet<TextField>("_leapPasswordField");
                    if (!m_password)
                        m_password = m_authNib->outlet<TextField>("_passwordField");
                    m_username = m_authNib->outlet<TextField>("_leapUsernameField");
                    if (!m_username)
                        m_username = m_authNib->outlet<TextField>("_usernameField");
                    if (auto *profile = m_authNib->outlet<PopUpButton>("_8021XProfilePopup")) {
                        profile->items = {MenuItem("Automatic")};
                        profile->setSelected(0);
                    }
                    m_reveal = m_authNib->outlet<CheckBox>("revealCheckbox");
                    if (m_password)
                        m_password->secure = true;
                    if (m_reveal)
                        m_reveal->setChecked(false);   // -reset
                    if (m_reveal && m_password) {
                        // -revealCheckboxPressed: the password in a field that shows it
                        TextField *field = m_password;
                        m_reveal->onToggle = [field](CheckBox *c) {
                            field->secure = !c->checked();
                            field->setNeedsDisplay();
                        };
                    }
                    for (TextField *f : {m_username, m_password})
                        if (f) {
                            f->onChange = [this](const std::string &) { refresh(); };
                            f->onEnter = [this] { accept(); };
                        }
                }
            }
        }
        layout();
        if (m_window) {
            if (m_username)
                m_window->makeFirstResponder(m_username);
            else if (m_password)
                m_window->makeFirstResponder(m_password);
        }
        refresh();
    }

    // ---- the three panels

    void preparePassword()
    {
        // kAirPortPasswordPanelWPAText / WEPText, else the plain one
        std::string text = tr("kAirPortPasswordPanelText", "The network “%@” requires a password.");
        if (m_type >= 1 && m_type <= 3)
            text = tr("kAirPortPasswordPanelWEPText", "The network “%@” requires a WEP password.");
        else if (m_type >= 4)
            text = tr("kAirPortPasswordPanelWPAText", "The network “%@” requires a WPA password.");
        if (auto *l = m_nib->outlet<Label>("_networkNameText"))
            l->setText(format(text, m_ssid));
        hide("_networkMessageText");
        hide("_askForPermission");
        if (auto *c = m_nib->outlet<CheckBox>("_keychainBox"))
            c->setChecked(true);
        showAuthView(m_type);
        m_first = m_username ? static_cast<View *>(m_username) : m_password;
    }

    void prepareJoin()
    {
        // the manual panel, as Join Other Network… opens it (display state 1)
        if (auto *l = m_nib->outlet<Label>("_windowTitle"))
            l->setText(tr("kAirPortSelectorManualNetworkTitleKey", "Enter the name of the network."));
        if (auto *l = m_nib->outlet<Label>("_windowDescription"))
            l->setText(tr("kAirPortSelectorManualNetworkDescriptionKey",
                          "Enter the name of the network you want to join, and then enter the password if necessary."));
        m_tabs = m_nib->outlet<TabView>("_tabView");
        if (m_tabs)
            m_tabs->select(0);   // "Manual"
        m_table = m_nib->outlet<TableView>("_networkTable");
        if (m_table) {
            m_tableFrame = m_table->frame();
            m_table->rows.clear();
            m_table->onSelect = [this](int row) { networkSelected(row); };
            m_table->onDoubleClick = [this](int) { accept(); };   // -handleNetworkTableDoubleClick:
            m_table->drawCell = [this](cairo_t *cr, int row, int column, const Rect &cell, bool highlighted) {
                return drawNetworkCell(cr, row, column, cell, highlighted);
            };
        }
        m_showHide = m_nib->outlet<Button>("_showHideButton");
        if (m_showHide) {
            m_showHide->title = tr("kAirPortSelectorShowNetworksButtonKey", "Show Networks");
            m_showHide->action = [this] { showHidePressed(); };
            m_showHide->setHidden(!m_table);
        }
        if (auto *c = m_nib->outlet<CheckBox>("_rememberButton"))
            c->setChecked(true);
        m_name = m_nib->outlet<TextField>("_networkNameField");
        if (m_name) {
            m_name->onChange = [this](const std::string &) { refresh(); };
            m_name->onEnter = [this] { accept(); };
        }
        showSecurityTypeView(true);
        showAuthView(0);
        m_first = m_name;
    }

    // -showSecurityTypeView: _wepTypeView ("Security:" and its pop-up) at the line, or not
    void showSecurityTypeView(bool show)
    {
        if (!show) {
            if (m_security)
                removeInserted(m_security);
            m_security = nullptr;
            layout();
            return;
        }
        if (m_security)
            return;
        if (View *security = m_nib->instantiateWindow(m_nib->outletObject("_wepTypeView"), nullptr)) {
            m_security = security;
            security->setFrame({0, 0, m_size.w, security->frame().h});
            insert(security, 0);
            if (auto *popup = m_nib->outlet<PopUpButton>("_wepKeyType")) {
                popup->items.clear();
                m_tags.clear();
                for (int tag : {0, 1, 2, 3, 4, -1, 5, 6, 7, 8, 9}) {
                    popup->items.push_back(tag < 0 ? MenuItem({}, true) : MenuItem(securityName(tag)));
                    m_tags.push_back(tag);
                }
                popup->setSelected(0);
                popup->onSelect = [this](int i) {
                    if (i >= 0 && size_t(i) < m_tags.size() && m_tags[size_t(i)] >= 0)
                        showAuthView(m_tags[size_t(i)]);
                };
            }
        }
        layout();
    }

    // ---- Show Networks: the table of the networks found (display state 0)

    // -handleShowHidePressed: (0x1c1c) the table, or back to the name
    void showHidePressed()
    {
        if (m_busy)
            return;
        m_tableMode = !m_tableMode;
        auto *title = m_nib->outlet<Label>("_windowTitle");
        auto *description = m_nib->outlet<Label>("_windowDescription");
        if (m_tableMode) {
            // -showNetworkTypeTable:YES: the tabled title and description, "Other…", the table 100 points
            if (title)
                title->setText(tr("kAirPortSelectorTabledNetworkTitleKey", "Select the network you want to join."));
            if (description)
                description->setText(tr("kAirPortSelectorTabledNetworkDescriptionKey",
                                        "Select the network you want to join from the list, and then enter the network password if necessary."));
            m_showHide->title = tr("kAirPortSelectorHideNetworksButtonKey", "Other…");
            showSecurityTypeView(false);
            m_tabExtra = 100 - 23;
            if (m_tabs) {
                m_tabs->select(1);
                if (View *page = m_tabs->page(1))
                    page->setFrame({page->frame().x, page->frame().y, page->frame().w, 100});
            }
            if (m_table) {
                m_table->setFrame({m_tableFrame.x, m_tableFrame.y, m_tableFrame.w, m_tableFrame.h + m_tabExtra});
                m_table->selected = -1;
                m_table->rows.clear();
                m_table->reload();
            }
            m_found.clear();
            showAuthView(0);
            if (auto *c = m_nib->outlet<CheckBox>("_rememberButton"))
                c->setEnabled(false);
            scan();
            // -startRescanTimer (0x21fc): every 20 seconds while no network is chosen
            m_rescan = App::shared().addTimer(20, true, [this] {
                if (m_table && m_table->selected < 0)
                    scan();
            });
            if (m_table)
                m_window->makeFirstResponder(m_table);
        } else {
            if (title)
                title->setText(tr("kAirPortSelectorManualNetworkTitleKey", "Enter the name of the network."));
            if (description)
                description->setText(tr("kAirPortSelectorManualNetworkDescriptionKey",
                                        "Enter the name of the network you want to join, and then enter the password if necessary."));
            m_showHide->title = tr("kAirPortSelectorShowNetworksButtonKey", "Show Networks");
            stopRescan();
            m_tabExtra = 0;
            if (m_tabs)
                m_tabs->select(0);
            if (auto *c = m_nib->outlet<CheckBox>("_rememberButton"))
                c->setEnabled(true);
            showAuthView(0);
            showSecurityTypeView(true);
            if (auto *popup = m_nib->outlet<PopUpButton>("_wepKeyType"))
                popup->setSelected(0);   // -setSecurityType: Open
            if (m_name)
                m_window->makeFirstResponder(m_name);
        }
        m_showHide->setNeedsDisplay();
        layout();
        refresh();
    }

    void stopRescan()
    {
        if (m_rescan)
            App::shared().removeTimer(m_rescan);
        m_rescan = 0;
    }

    // -handleSearchForNetworks: the wheel, OK and Show/Hide off while NetworkManager scans
    void scan()
    {
        if (m_scanFd >= 0)
            return;
        int fds[2];
        if (pipe(fds) != 0)
            return;
        const pid_t pid = fork();
        if (pid == 0) {
            dup2(fds[1], 1);
            close(fds[0]);
            close(fds[1]);
            const int null = ::open("/dev/null", O_WRONLY);
            if (null >= 0)
                dup2(null, 2);
            execlp("nmcli", "nmcli", "-t", "-e", "yes", "-f", "SSID,SECURITY,SIGNAL,MODE", "device", "wifi", "list", "--rescan", "yes",
                   static_cast<char *>(nullptr));
            _exit(127);
        }
        close(fds[1]);
        if (pid < 0) {
            close(fds[0]);
            return;
        }
        m_scanPid = pid;
        m_scanFd = fds[0];
        m_scanOutput.clear();
        if (auto *wheel = m_nib->outlet<ProgressIndicator>("_progressIndicator")) {
            wheel->spinning = wheel->indeterminate = true;
            wheel->setHidden(false);
            wheel->start();
        }
        if (m_showHide)
            m_showHide->setEnabled(false);
        refresh();
        App::shared().watch(m_scanFd, [this] {
            char buf[4096];
            const ssize_t n = read(m_scanFd, buf, sizeof buf);
            if (n > 0) {
                m_scanOutput.append(buf, size_t(n));
                return;
            }
            App::shared().unwatch(m_scanFd);
            close(m_scanFd);
            m_scanFd = -1;
            waitpid(m_scanPid, nullptr, 0);
            scanComplete();
        });
    }

    // nmcli's terse fields, ':' escaped as "\:"
    static std::vector<std::string> fields(const std::string &line)
    {
        std::vector<std::string> out(1);
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size())
                out.back() += line[++i];
            else if (line[i] == ':')
                out.emplace_back();
            else
                out.back() += line[i];
        }
        return out;
    }

    // -clientController:scanComplete: (0x41df): the evil twins out (a name and security once, the
    // strongest), sorted by name; the table reloaded unless a network is chosen
    void scanComplete()
    {
        if (auto *wheel = m_nib->outlet<ProgressIndicator>("_progressIndicator")) {
            wheel->stop();
            wheel->setHidden(true);
        }
        if (m_showHide)
            m_showHide->setEnabled(true);
        std::vector<Found> found;
        size_t start = 0;
        while (start < m_scanOutput.size()) {
            size_t end = m_scanOutput.find('\n', start);
            if (end == std::string::npos)
                end = m_scanOutput.size();
            const auto f = fields(m_scanOutput.substr(start, end - start));
            start = end + 1;
            if (f.size() < 4 || f[0].empty())
                continue;
            Found n{f[0], f[1] == "--" ? std::string() : f[1], std::atoi(f[2].c_str()), f[3] == "Ad-Hoc"};
            auto same = std::find_if(found.begin(), found.end(), [&](const Found &o) { return o.ssid == n.ssid && o.security == n.security; });
            if (same == found.end())
                found.push_back(n);
            else if (n.signal > same->signal)
                *same = n;
        }
        std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return strcasecmp(a.ssid.c_str(), b.ssid.c_str()) < 0; });
        if (m_table && m_table->selected < 0) {
            m_found = found;
            m_table->rows.clear();
            for (const Found &n : m_found)
                m_table->rows.push_back({n.ssid, {}, {}});
            m_table->reload();
        }
        refresh();
    }

    // -tableViewSelectionDidChange: (0x319b): the network's security type, its auth view
    void networkSelected(int row)
    {
        const bool valid = row >= 0 && size_t(row) < m_found.size();
        if (auto *c = m_nib->outlet<CheckBox>("_rememberButton"))
            c->setEnabled(valid);
        if (!valid)
            return;
        const std::string &sec = m_found[size_t(row)].security;
        int tag = 0;
        if (sec.find("802.1X") != std::string::npos)
            tag = sec.find("WPA2") != std::string::npos || sec.find("WPA3") != std::string::npos ? 8
                  : sec.find("WPA") != std::string::npos                                         ? 6
                                                                                                 : 9;
        else if (sec.find("WPA2") != std::string::npos || sec.find("WPA3") != std::string::npos)
            tag = 7;
        else if (sec.find("WPA") != std::string::npos)
            tag = 5;
        else if (sec.find("WEP") != std::string::npos)
            tag = 1;
        if (auto *wheel = m_nib->outlet<ProgressIndicator>("_progressIndicator")) {
            wheel->stop();
            wheel->setHidden(true);
        }
        showAuthView(tag);
        m_window->makeFirstResponder(m_password ? static_cast<View *>(m_password) : m_table);
        refresh();
    }

    // -tableView:objectValueForTableColumn:row: (0x43db): the name, the lock (not for Open), the bars
    // (+barsForSignal:), the selected row's white
    bool drawNetworkCell(cairo_t *cr, int row, int column, const Rect &cell, bool highlighted)
    {
        if (column == 0 || row < 0 || size_t(row) >= m_found.size())
            return false;
        const Found &n = m_found[size_t(row)];
        Image img;
        if (column == 1) {
            if (n.security.empty())
                return true;
            img = art(highlighted ? "lock-white" : "lock");
        } else {
            // NetworkManager's quality is the RSSI from -100 to -40 dBm
            const int rssi = n.signal <= 0 ? 0 : n.signal >= 100 ? -40 : int(std::lround(n.signal * 0.6 - 100));
            static const int table[5] = {0, 20, 40, 60, 80};
            const int s = rssi < 1 ? rssi + 100 : rssi;
            int bars = 0;
            while (table[bars] < s && bars != 4)
                ++bars;
            img = art(std::string("AirportInMenu") + std::to_string(bars) + (highlighted ? "-white" : ""));
        }
        if (img.isNull())
            return true;
        // the pictures are made at twice their size
        const Size size{img.width() / 2.0, img.height() / 2.0};
        const Size fit = size.w <= cell.w && size.h <= cell.h ? size : scaledToFit(size, {cell.w, cell.h});   // never larger
        img.draw(cr, Rect{cell.x + std::floor((cell.w - fit.w) / 2), cell.y + std::floor((cell.h - fit.h) / 2), fit.w, fit.h},
                 highlighted ? 1.0 : 0.63);
        return true;
    }

    // the bundle's pictures (PDFs: AirportInMenu0-4, AlertTriangleSmall) at twice their size, the
    // lock (NSImageNameLockLockedTemplate: CoreUI's LockLocked); "-white": a template's highlighted
    Image art(const std::string &name)
    {
        auto it = m_art.find(name);
        if (it != m_art.end())
            return it->second;
        const bool white = name.size() > 6 && name.compare(name.size() - 6, 6, "-white") == 0;
        const std::string base = white ? name.substr(0, name.size() - 6) : name;
        Image img;
        if (base == "lock") {
            Image lock = CoreUI::named("LockLocked");
            img = lock.isNull() ? lock : lock.scaled(lock.width() * 2, lock.height() * 2);
        } else {
            const std::string path = Assets::path(Bundle + "/Contents/Resources/" + base + ".pdf");
            const Size page = Image::sizeOf(path);
            img = Image::load(path, {page.w * 2, page.h * 2});
        }
        if (white || base == "lock" || base.rfind("AirportInMenu", 0) == 0)
            img = img.tinted(white ? Color::gray(255) : Color::gray(0));   // templates
        m_art[name] = img;
        return img;
    }

    void prepareCreate()
    {
        m_name = m_nib->outlet<TextField>("_networkNameField");
        if (m_name) {
            char host[256] = {};
            gethostname(host, sizeof host - 1);
            std::string name = host;
            if (name.size() > 32)
                name.resize(32);
            m_name->setText(name);
            m_name->onChange = [this](const std::string &) { refresh(); };
            m_name->onEnter = [this] { accept(); };
        }
        if (auto *channels = m_nib->outlet<PopUpButton>("_channelPopup")) {
            // -populateIBSSChannelPopup (0x7451): the card's channels (Apple80211's supported channels;
            // here iw's, those it may start a network on), 2.4 GHz then 5 GHz to 48; "Automatic (11)",
            // or the last 2.4 GHz channel without 11
            std::vector<int> low, high;
            cardChannels(low, high);
            const int automatic = std::find(low.begin(), low.end(), 11) != low.end() ? 11 : low.empty() ? 11 : low.back();
            channels->items = {MenuItem(format(tr("kAirPortIBSSDefaultChannelKey", "Automatic (%@)"), std::to_string(automatic))),
                               MenuItem({}, true)};
            m_channels = {0, -1};
            for (int c : low) {
                channels->items.push_back(MenuItem(std::to_string(c)));
                m_channels.push_back(c);
            }
            if (!high.empty()) {
                channels->items.push_back(MenuItem({}, true));
                m_channels.push_back(-1);
            }
            for (int c : high) {
                channels->items.push_back(MenuItem(std::to_string(c)));
                m_channels.push_back(c);
            }
            channels->setSelected(0);
        }
        if (auto *box = m_nib->outlet<CheckBox>("_enableEncryptionCheckBox")) {
            box->setChecked(false);
            box->onToggle = [this](CheckBox *c) { requirePassword(c->checked()); };
        }
        hide("_helpButton");
        m_first = m_name;
    }

    // `iw list`'s frequencies: "* 2412.0 MHz [1] (20.0 dBm)", without "disabled" or "no IR" (no network
    // may be started there); 1-11 and 36-48 when iw can't say
    static void cardChannels(std::vector<int> &low, std::vector<int> &high)
    {
        if (FILE *f = popen("iw list 2>/dev/null", "r")) {
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                const char *bracket = std::strstr(line, " MHz [");
                if (!bracket || std::strstr(line, "disabled") || std::strstr(line, "no IR"))
                    continue;
                const int c = std::atoi(bracket + 6);
                if (c >= 1 && c <= 14) {
                    if (std::find(low.begin(), low.end(), c) == low.end())
                        low.push_back(c);
                } else if (c > 14 && c <= 48 && std::find(high.begin(), high.end(), c) == high.end()) {
                    high.push_back(c);
                }
            }
            pclose(f);
        }
        std::sort(low.begin(), low.end());
        std::sort(high.begin(), high.end());
        if (low.empty() && high.empty()) {
            for (int c = 1; c <= 11; ++c)
                low.push_back(c);
            high = {36, 40, 44, 48};
        }
    }

    // -enableEncryptionChecked: the nib's _authViewBasic at the line, or not
    void requirePassword(bool on)
    {
        if (!on) {
            if (m_auth)
                removeInserted(m_auth);
            m_auth = nullptr;
            m_password = m_verify = nullptr;
            layout();
            refresh();
            return;
        }
        m_auth = m_nib->instantiateWindow(m_nib->outletObject("_authViewBasic"), nullptr);
        if (!m_auth)
            return;
        m_auth->setFrame({0, 0, m_size.w, m_auth->frame().h});
        insert(m_auth, 0);
        m_password = m_nib->outlet<TextField>("_passwordField");
        m_verify = m_nib->outlet<TextField>("_verifyPasswordField");
        for (TextField *f : {m_password, m_verify})
            if (f) {
                f->secure = true;
                f->onChange = [this](const std::string &) { refresh(); };
                f->onEnter = [this] { accept(); };
            }
        if (auto *keys = m_nib->outlet<PopUpButton>("_wepKeyType")) {
            if (keys->items.empty())
                keys->items = {MenuItem("40-bit WEP (more compatible)"), MenuItem("128-bit WEP")};
            keys->setSelected(1);
            keys->onSelect = [this](int) { refresh(); };
        }
        layout();
        if (m_password)
            m_window->makeFirstResponder(m_password);
        refresh();
    }

    // -validateEntries / -refreshUI: OK as what's typed allows
    bool valid()
    {
        if (m_kind == Create) {
            if (!m_name || m_name->text.empty())
                return false;
            if (!m_password)
                return true;
            const std::string &p = m_password->text;
            if (!m_verify || p != m_verify->text || p.empty())
                return false;
            auto *keys = m_nib->outlet<PopUpButton>("_wepKeyType");
            const bool wide = keys && keys->selected() == 1;
            return wide ? (p.size() == 13 || (p.size() == 26 && allHex(p))) : (p.size() == 5 || (p.size() == 10 && allHex(p)));
        }
        if (m_kind == Join && m_tableMode && (!m_table || m_table->selected < 0 || size_t(m_table->selected) >= m_found.size()))
            return false;
        if (m_kind == Join && !m_tableMode && (!m_name || m_name->text.empty()))
            return false;
        const std::string pass = m_password ? m_password->text : std::string();
        switch (m_authType) {
        case 0:
            return true;
        case 1:
            return !pass.empty();
        case 2:
            return (pass.size() == 10 || pass.size() == 26) && allHex(pass);
        case 3:
            return pass.size() == 5 || pass.size() == 13;
        case 5:
        case 7:
            return (pass.size() >= 8 && pass.size() <= 63) || (pass.size() == 64 && allHex(pass));
        default:
            return !pass.empty() && (!m_username || !m_username->text.empty());
        }
    }

    void refresh()
    {
        if (m_kind == Create)
            if (auto *hint = m_nib->outlet<Label>("_wepKeyHint")) {
                auto *keys = m_nib->outlet<PopUpButton>("_wepKeyType");
                hint->setText(keys && keys->selected() == 1
                                  ? tr("kAirPortWepKey128Hint", "The password must be entered as exactly 13 ASCII characters or 26 hex digits.")
                                  : tr("kAirPortWepKey40Hint", "The password must be entered as exactly 5 ASCII characters or 10 hex digits."));
            }
        if (auto *ok = m_nib->outlet<Button>("_okButton"))
            ok->setEnabled(!m_busy && m_scanFd < 0 && valid());
    }

    // ---- OK: what was typed to SystemUIServer, the wheel until it says how it went

    void accept()
    {
        if (m_busy || !valid())
            return;
        std::map<std::string, std::string> out;
        if (m_kind == Password) {
            out["ssid"] = m_ssid;
            out["security"] = std::to_string(m_authType);
        } else if (m_kind == Join) {
            out["ssid"] = m_tableMode ? m_found[size_t(m_table->selected)].ssid : (m_name ? m_name->text : std::string());
            out["security"] = std::to_string(m_authType);
            out["hidden"] = m_tableMode ? "0" : "1";
            stopRescan();
        } else {
            out["ssid"] = m_name ? m_name->text : std::string();
            auto *channels = m_nib->outlet<PopUpButton>("_channelPopup");
            const int i = channels ? channels->selected() : 0;
            out["channel"] = std::to_string(i >= 0 && size_t(i) < m_channels.size() ? std::max(0, m_channels[size_t(i)]) : 0);
            out["security"] = m_password ? "2" : "0";   // a WEP key, as typed (hex or ASCII)
        }
        if (m_password)
            out["password"] = m_password->text;
        if (m_username)
            out["username"] = m_username->text;
        auto *remember = m_nib->outlet<CheckBox>(m_kind == Password ? "_keychainBox" : "_rememberButton");
        out["remember"] = remember && !remember->isHidden() ? (remember->checked() ? "1" : "0") : "1";
        for (const auto &[k, v] : out)
            std::printf("%s=%s\n", k.c_str(), escape(v).c_str());
        std::printf("end\n");
        std::fflush(stdout);

        m_busy = true;
        if (m_warning)
            m_warning->setHidden(true);
        if (auto *label = m_nib->outlet<Label>("_progressLabel")) {
            if (m_kind != Create)
                label->setText(tr("kAirPortConnectingKey", "Connecting to network…"));
            label->setHidden(false);
        }
        if (auto *wheel = m_nib->outlet<ProgressIndicator>("_progressIndicator")) {
            wheel->spinning = true;
            wheel->indeterminate = true;
            wheel->setHidden(false);
            wheel->start();
        }
        refresh();
    }

    // _warningButton: AlertTriangleSmall, in its 16 points
    void showWarning()
    {
        auto *button = m_nib->outlet<View>("_warningButton");
        if (!button)
            return;
        if (!m_warning) {
            m_warning = new ImageView;
            // AlertTriangleSmall, else CoreTypes' caution triangle
            m_warning->image = art("AlertTriangleSmall");
            if (m_warning->image.isNull())
                m_warning->image = Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/AlertCautionIcon.icns"), {16, 16});
            button->superview()->addSubview(m_warning);
        }
        m_warning->setFrame(button->frame());
        m_warning->setHidden(false);
    }

    void readCommand()
    {
        char buf[512];
        const ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0) {
            App::shared().unwatch(0);
            App::shared().quit(m_busy ? 0 : 1);   // SystemUIServer gone
            return;
        }
        m_input.append(buf, size_t(n));
        size_t nl;
        while ((nl = m_input.find('\n')) != std::string::npos) {
            const std::string line = m_input.substr(0, nl);
            m_input.erase(0, nl + 1);
            if (line == "done") {
                App::shared().quit(0);
            } else if (line.rfind("error ", 0) == 0) {
                // the failure in the progress line, the warning button beside it
                m_busy = false;
                if (auto *wheel = m_nib->outlet<ProgressIndicator>("_progressIndicator")) {
                    wheel->stop();
                    wheel->setHidden(true);
                }
                if (auto *label = m_nib->outlet<Label>("_progressLabel"))
                    label->setText(line.substr(6));
                showWarning();
                refresh();
            }
        }
    }

    Kind m_kind;
    std::string m_ssid;
    int m_type = 0, m_authType = 0;
    std::unique_ptr<Nib> m_nib, m_authNib;
    View *m_root = nullptr, *m_auth = nullptr, *m_first = nullptr;
    Size m_size;
    double m_lineY = 0;
    std::map<View *, Rect> m_original;
    std::vector<Inserted> m_inserted;
    std::unique_ptr<Window> m_window;
    TextField *m_name = nullptr, *m_password = nullptr, *m_username = nullptr, *m_verify = nullptr;
    CheckBox *m_reveal = nullptr;
    ImageView *m_warning = nullptr;
    std::vector<int> m_tags, m_channels;
    struct Found {
        std::string ssid, security;
        int signal = 0;
        bool adhoc = false;
    };
    bool m_busy = false, m_option = false, m_tableMode = false;
    TabView *m_tabs = nullptr;
    TableView *m_table = nullptr;
    Button *m_showHide = nullptr;
    View *m_security = nullptr;
    Rect m_tableFrame;
    double m_tabExtra = 0;
    std::vector<Found> m_found;
    int m_rescan = 0, m_scanFd = -1;
    pid_t m_scanPid = -1;
    std::string m_scanOutput;
    std::map<std::string, Image> m_art;

    std::string m_input;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: ClientDialogs password SSID TYPE | join | create\n");
        return 2;
    }
    const std::string what = argv[1];
    Panel::Kind kind = what == "password" ? Panel::Password : what == "join" ? Panel::Join : Panel::Create;
    if (what != "password" && what != "join" && what != "create")
        return 2;
    App &app = App::shared();
    setProgramName("SystemUIServer");
    if (!app.open()) {
        std::fprintf(stderr, "ClientDialogs: no display\n");
        return 2;
    }
    FontManager::registerSystemFonts();
    bool option = false;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--option") == 0)
            option = true;
        else
            positional.push_back(argv[i]);
    }
    Panel panel(kind, positional.size() > 0 ? positional[0] : "", positional.size() > 1 ? std::atoi(positional[1].c_str()) : 7, option);
    if (!panel.open()) {
        std::fprintf(stderr, "ClientDialogs: ClientDialogs.bundle is not in the assets (tools/extract.py takes it from the DVD)\n");
        return 2;
    }
    return app.run();
}
