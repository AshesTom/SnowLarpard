// SPDX-License-Identifier: LGPL-2.1-or-later
// Software Update 4.0 (10.6) on AquaKit: its nibs, strings and pictures from the DVD, the updates from
// Apple's own catalog (tools/softwareupdate.py reads it, downloads the packages, and has extract.py
// lay their files over the DVD's). Reversed from Software Update's x86_64 slice:
//
//   -[SUAppWindowController start]: SUAppScanPanel's panel ("Checking for new software…", an
//   indeterminate bar, Cancel) while the catalog is read; then the window: an update to download that
//   needs a restart gives -[SUAppSimplifiedUIController _downloadAndRestartUpdatesAlert], an NSAlert
//   put in the window (_loadSimplifiedView: its content view, the window's buttons hidden): the
//   message UPDATES_AVAILABLE_DOWNLOAD_AND_RESTART_*, Continue (install:), Show Details
//   (toggleDetails:), Not Now (terminate:), and the legal text (_adjustAlert:legalTextType: 1, the
//   update's) from SUAppSimplifiedView.nib as its accessory. Show Details: SUAppDetailedView.nib (the
//   products' table, the description, the restart line, Install, Hide Details, Not Now).
//   Downloading: SUAppInstallWindow.nib, SUAppSessionController's "Downloading 1 item",
//   "Downloading %@…", "%@ of %@ — %@"; then "Expanding and verifying %@…". A restart-required update
//   is installed at log out (SUAppSession's UPDATES_DOWNLOADED_INSTALL_AND_RESTART_*: Restart, Not
//   Now): the session ends, and snowleopard-session runs `Software Update --install-at-logout` before
//   it starts again: the login screen's picture, the install window ("Installing 1 item",
//   "Installing updates…"), then SYSTEM_WILL_RESTART_MESSAGE.
//
//   Software Update                     checks, offers what applies
//   Software Update --install-at-logout installs what was downloaded (snowleopard-session)
#include "controls.h"
#include "foundation.h"
#include "mainmenu.h"
#include "nib.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

extern char **environ;

using namespace AK;

namespace {

const std::string Bundle = "/System/Library/CoreServices/Software Update.app";

std::string res(const std::string &name) { return Assets::path(Bundle + "/Contents/Resources/" + name); }

std::string tr(const char *table, const char *key, const char *fallback) { return Assets::tr(Bundle, table, key, fallback); }

// "%@" and "%d" filled in order
std::string format(std::string f, const std::vector<std::string> &values)
{
    std::string out;
    size_t v = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '%' && i + 1 < f.size() && (f[i + 1] == '@' || f[i + 1] == 'd')) {
            out += v < values.size() ? values[v++] : std::string();
            ++i;
        } else if (f[i] == '%' && i + 1 < f.size() && f[i + 1] == '%') {
            out += '%';
            ++i;
        } else {
            out += f[i];
        }
    }
    return out;
}

// 10.6's sizes are decimal (1 GB = 10^9 bytes): "711 KB", "24.9 MB", "1.07 GB"
std::string sizeString(double bytes)
{
    char buf[32];
    if (bytes < 1e6)
        std::snprintf(buf, sizeof buf, "%.0f KB", std::max(1.0, std::round(bytes / 1e3)));
    else if (bytes < 1e9)
        std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1e6);
    else
        std::snprintf(buf, sizeof buf, "%.2f GB", bytes / 1e9);
    return buf;
}

std::string timeString(double seconds)
{
    if (seconds < 60)
        return "less than a minute";
    const int m = int(std::round(seconds / 60));
    if (m < 60)
        return m == 1 ? "about a minute" : "about " + std::to_string(m) + " minutes";
    const int h = int(std::round(seconds / 3600));
    return h == 1 ? "about an hour" : "about " + std::to_string(h) + " hours";
}

// The description is HTML (WebView's): its text, a bullet for each item
std::string htmlText(const std::string &html)
{
    std::string body = html;
    const size_t b = body.find("<body");
    if (b != std::string::npos)
        body = body.substr(body.find('>', b) + 1);
    std::string out;
    for (size_t i = 0; i < body.size();) {
        if (body[i] == '<') {
            const size_t e = body.find('>', i);
            if (e == std::string::npos)
                break;
            std::string tag = body.substr(i + 1, e - i - 1);
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            if (tag.rfind("li", 0) == 0)
                out += "\n•  ";
            else if (tag == "/p" || tag == "/ul" || tag.rfind("br", 0) == 0)
                out += "\n";
            i = e + 1;
        } else if (body[i] == '\n' || body[i] == '\t') {
            if (!out.empty() && out.back() != ' ' && out.back() != '\n')
                out += ' ';
            ++i;
        } else if (body.compare(i, 5, "&amp;") == 0) {
            out += '&';
            i += 5;
        } else {
            out += body[i++];
        }
    }
    // the paragraphs one blank line apart, no spaces at the lines' starts
    std::string clean;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == ' ' && (clean.empty() || clean.back() == '\n'))
            continue;
        if (out[i] == '\n' && clean.size() >= 2 && clean[clean.size() - 1] == '\n' && clean[clean.size() - 2] == '\n')
            continue;
        clean += out[i];
    }
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == ' '))
        clean.pop_back();
    while (!clean.empty() && clean.front() == '\n')
        clean.erase(clean.begin());
    return clean;
}

// ---- tools/softwareupdate.py, as SoftwareUpdate.framework's daemon

std::string toolsDir()
{
    const char *env = std::getenv("SNOWLEOPARD_TOOLS");
    if (env && *env)
        return env;
    // .local/build/apps/Software Update.app/Contents/MacOS -> tools; PREFIX/lib/snowleopard/apps/... -> tools
    std::string dir = dirName(executablePath());
    for (int i = 0; i < 7; ++i) {
        if (isFile(dir + "/tools/softwareupdate.py"))
            return dir + "/tools";
        dir = dirName(dir);
    }
    return {};
}

std::string stateDir()
{
    const char *x = std::getenv("XDG_STATE_HOME");
    return (x && *x ? std::string(x) : home() + "/.local/state") + "/snowleopard";
}

// A child whose output is read line by line as it comes, with the display's events
// The tool's command line: through pkexec when the files are the system's (install.sh's, root's)
std::vector<std::string> tool(std::vector<std::string> args, bool writes = false)
{
    std::vector<std::string> cmd;
    if (writes && access(Assets::root().c_str(), W_OK) != 0)
        cmd.push_back("pkexec");
    cmd.push_back("python3");
    cmd.push_back("-I");
    cmd.push_back(toolsDir() + "/softwareupdate.py");
    cmd.insert(cmd.end(), args.begin(), args.end());
    cmd.push_back("--assets");
    cmd.push_back(Assets::root());
    if (writes) {
        // root's cache is not the user's: the packages' folder named
        const char *x = std::getenv("XDG_CACHE_HOME");
        cmd.push_back("--dir");
        cmd.push_back((x && *x ? std::string(x) : home() + "/.cache") + "/snowleopard/Updates/" + args.at(1));
    }
    return cmd;
}

// The few values of the tool's JSON the windows show
struct Product {
    std::string key, title, version, serverComment, description;
    double size = 0;
    bool restart = true, downloaded = false;
};

std::string jsonString(const std::string &json, const std::string &key, size_t from = 0)
{
    const size_t k = json.find("\"" + key + "\":", from);
    if (k == std::string::npos)
        return {};
    size_t i = json.find('"', k + key.size() + 3);
    if (i == std::string::npos)
        return {};
    std::string out;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] != '\\') {
            out += json[i];
            continue;
        }
        const char c = json[++i];
        if (c == 'n')
            out += '\n';
        else if (c == 't')
            out += '\t';
        else if (c == 'u') {
            const unsigned cp = unsigned(std::stoul(json.substr(i + 1, 4), nullptr, 16));
            i += 4;
            if (cp < 0x80) {
                out += char(cp);
            } else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
        } else {
            out += c;
        }
    }
    return out;
}

std::string jsonRaw(const std::string &json, const std::string &key, size_t from = 0)
{
    const size_t k = json.find("\"" + key + "\":", from);
    if (k == std::string::npos)
        return {};
    size_t i = k + key.size() + 3;
    while (i < json.size() && json[i] == ' ')
        ++i;
    size_t e = i;
    while (e < json.size() && json[e] != ',' && json[e] != '}')
        ++e;
    return json.substr(i, e - i);
}

std::vector<Product> parseProducts(const std::string &json)
{
    std::vector<Product> out;
    const size_t list = json.find("\"products\":");
    if (list == std::string::npos)
        return out;
    for (size_t at = json.find("{\"key\":", list); at != std::string::npos; at = json.find("{\"key\":", at + 1)) {
        Product p;
        p.key = jsonString(json, "key", at);
        p.title = jsonString(json, "title", at);
        p.version = jsonString(json, "version", at);
        p.serverComment = jsonString(json, "serverComment", at);
        p.description = htmlText(jsonString(json, "description", at));
        p.size = std::atof(jsonRaw(json, "size", at).c_str());
        p.restart = jsonRaw(json, "restart", at) != "false";
        p.downloaded = jsonRaw(json, "downloaded", at) == "true";
        out.push_back(p);
    }
    return out;
}

// ---- an NSAlert's layout (the icon, the message in bold, the information, an accessory, the
// buttons from the right, the first the default one), in a view of its own

struct AlertButton {
    std::string title;
    std::function<void()> action;
    bool left = false;   // apart, at the left (Show Details)
};

struct Alert {
    View *view = nullptr;
    Size size;
    std::vector<Button *> buttons;
};

Alert makeAlert(const Image &icon, const std::string &message, const std::string &info, const std::string &accessory,
                const std::vector<AlertButton> &buttons)
{
    Alert a;
    const double width = 440, textX = 104, textW = width - textX - 20;
    const double mh = std::max(17.0, wrappedHeight(message, textW, Font::boldSystem(13)));
    const double ih = info.empty() ? 0 : wrappedHeight(info, textW, Font::system(11)) + 8;
    const double ah = accessory.empty() ? 0 : wrappedHeight(accessory, textW, Font::system(11)) + 12;
    a.size = {width, std::max(126.0, 15 + mh + ih + ah + 20 + 32 + 16)};
    a.view = new View;
    a.view->setFrame({0, 0, a.size.w, a.size.h});
    auto *iconView = new ImageView;
    iconView->image = icon;
    iconView->setFrame({24, 15, 64, 64});
    a.view->addSubview(iconView);
    auto *m = new Label(message, Font::boldSystem(13));
    m->wrap = true;
    m->setFrame({textX, 15, textW, mh});
    a.view->addSubview(m);
    double y = 15 + mh + 8;
    if (!info.empty()) {
        auto *i = new Label(info, Font::system(11));
        i->wrap = true;
        i->setFrame({textX, y, textW, ih - 8});
        a.view->addSubview(i);
        y += ih;
    }
    if (!accessory.empty()) {
        auto *l = new Label(accessory, Font::system(11));
        l->wrap = true;
        l->setFrame({textX, y + 4, textW, ah - 12});
        a.view->addSubview(l);
    }
    double right = a.size.w - 20, left = 20;
    for (size_t b = 0; b < buttons.size(); ++b) {
        const double w = std::max(82.0, std::ceil(textWidth(buttons[b].title, Font::system(13))) + 32);
        auto *button = new Button(buttons[b].title);
        button->isDefault = b == 0;
        if (buttons[b].left) {
            button->setFrame({left, a.size.h - 16 - 32, w, 32});
            left += w + 6;
        } else {
            button->setFrame({right - w, a.size.h - 16 - 32, w, 32});
            right -= w + 6;
        }
        button->action = buttons[b].action;
        a.view->addSubview(button);
        a.buttons.push_back(button);
    }
    return a;
}

// ---- the application

class SoftwareUpdate {
public:
    explicit SoftwareUpdate(bool atLogout) : m_atLogout(atLogout)
    {
        m_icon = Image::load(res("Software Update.icns"), {64, 64});
        if (!atLogout)
            loadMenus();
    }

    void start()
    {
        if (m_atLogout)
            installAtLogout();
        else
            scan();
    }

private:
    // ---- the menus: MainMenu.nib's
    void loadMenus()
    {
        m_menu.load(res("English.lproj/MainMenu.nib"));
        m_menu.validate = [this](const MainMenuItem &it, MenuValidation &) {
            const std::string &a = it.action;
            if (a == "terminate:")
                return !m_busy;
            return a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
                   a == "performMiniaturize:" || a == "miniaturize:";
        };
        m_menu.perform = [this](const MainMenuItem &it) {
            if (it.action == "terminate:")
                App::shared().quit();
            else if ((it.action == "performMiniaturize:" || it.action == "miniaturize:") && m_window)
                m_window->miniaturize();
        };
        m_menu.publish();
    }

    // ---- SUAppScanPanel: the catalog read
    void scan()
    {
        m_scanNib = std::make_unique<Nib>();
        View *content = nullptr;
        if (m_scanNib->load(res("English.lproj/SUAppScanPanel.nib/keyedobjects.nib"), res("")))
            content = m_scanNib->instantiate("window");
        Size size = content ? m_scanNib->size() : Size{400, 138};
        if (!content) {
            content = new View;
            content->setFrame({0, 0, size.w, size.h});
        }
        m_scanPanel = std::make_unique<Window>(size, "Software Update");
        m_scanPanel->contentView()->addSubview(content);
        if (auto *bar = m_scanNib->outlet<ProgressIndicator>("progressIndicator")) {
            bar->indeterminate = true;
            bar->start();
        }
        if (auto *cancel = m_scanNib->outlet<Button>("cancelButton"))
            cancel->action = [] { App::shared().quit(); };   // -[SUAppScanController cancel:]
        m_scanPanel->onClose = [] { App::shared().quit(); };
        m_scanPanel->onUnhandledKey = [](Event &e) {
            if (e.keysym == 0xff1b)
                App::shared().quit();
        };
        m_scanPanel->show();
        std::string out;
        m_helper = std::make_unique<Task>();
        m_helper->errors = Task::Errors::Inherit;
        auto buffer = std::make_shared<std::string>();
        m_helper->onLine = [buffer](const std::string &l) { *buffer += l; };
        m_helper->onExit = [this, buffer](int code, const std::string &) {
            m_scanPanel->onClose = nullptr;
            m_scanPanel->close();
            if (code != 0 || buffer->find("\"products\"") == std::string::npos) {
                failed();
                return;
            }
            m_products = parseProducts(*buffer);
            if (m_products.empty())
                upToDate();
            else
                simplified();
        };
        if (toolsDir().empty() || !m_helper->launch(tool({"scan"})))
            App::shared().later([this] {
                m_scanPanel->close();
                failed();
            });
    }

    // the window (SUAppWindow.nib's: "Software Update", its content one of the views below)
    void show(View *content, Size size, bool buttons)
    {
        if (!m_window) {
            m_window = std::make_unique<Window>(size, "Software Update");
            m_window->onClose = [this] {
                if (!m_busy)
                    App::shared().quit();
            };
            m_window->onUnhandledKey = [this](Event &e) { key(e); };
            m_window->setIcon(m_icon);
            m_window->contentView()->addSubview(content);
            m_window->show();
        } else {
            m_window->contentView()->removeAllSubviews();
            m_window->setContentSize(size);
            m_window->contentView()->addSubview(content);
        }
        (void)buttons;
        m_content = content;
    }

    // Return: the default button; Escape: Not Now / Quit
    void key(Event &e)
    {
        if (e.keysym != 0xff0d && e.keysym != 0xff8d && e.keysym != 0xff1b)
            return;
        std::function<void(View *)> walk;
        Button *found = nullptr;
        const bool escape = e.keysym == 0xff1b;
        walk = [&](View *v) {
            for (View *s : v->subviews()) {
                if (auto *b = dynamic_cast<Button *>(s)) {
                    if (b->isEnabled() && !b->isHidden() &&
                        (escape ? (b->title == m_escapeTitle) : b->isDefault))
                        found = b;
                }
                walk(s);
            }
        };
        if (m_content)
            walk(m_content);
        if (found && found->action)
            found->action();
    }

    std::string legalUpdateText()
    {
        // _adjustAlert:legalTextType: 1, SUAppSimplifiedView.nib's legalUpdateTextView
        // (an NSTextView: its NSTextContainer's NSLayoutManager's NSTextStorage holds the text)
        const std::string file = res("English.lproj/SUAppSimplifiedView.nib/keyedobjects.nib");
        Nib nib;
        Archive archive;
        if (nib.load(file, res("")) && archive.load(file)) {
            const int view = nib.outletObject("legalUpdateTextView");
            const int storage = archive.object(archive.object(archive.object(view, "NSTextContainer"), "NSLayoutManager"), "NSTextStorage");
            std::string text = archive.string(storage, "NSString");
            for (size_t at; (at = text.find("\xC2\xA0")) != std::string::npos;)
                text.replace(at, 2, " ");
            if (!text.empty())
                return text;
        }
        return "Note: Use of this software is subject to the original Software License Agreement(s) that "
               "accompanied the software being updated. A list of Apple SLAs may be found here: http://www.apple.com/legal/sla/";
    }

    // -[SUAppSimplifiedUIController alertForProducts:]: to download, with a restart (the combo)
    void simplified()
    {
        const bool restart = std::any_of(m_products.begin(), m_products.end(), [](const Product &p) { return p.restart; });
        const bool downloaded = std::all_of(m_products.begin(), m_products.end(), [](const Product &p) { return p.downloaded; });
        std::string message, info, ok;
        if (downloaded) {
            message = tr("SUAppNotif", "UPDATES_AVAILABLE_INSTALL_MESSAGE",
                         "Software updates for your computer are ready to be installed. Do you want to install them now?");
            info = tr("SUAppNotif", "UPDATES_AVAILABLE_INSTALL_AND_RESTART_INFORMATIVE",
                      "You must restart your computer after the updates are installed.");
            ok = tr("SUAppNotif", "LOG_OUT_AND_INSTALL_AND_RESTART_TITLE", "Install and Restart");
        } else if (restart) {
            message = tr("SUAppNotif", "UPDATES_AVAILABLE_DOWNLOAD_AND_RESTART_MESSAGE",
                         "Software updates are available for your computer. Do you want to download and install them?");
            info = tr("SUAppNotif", "UPDATES_AVAILABLE_DOWNLOAD_AND_RESTART_INFORMATIVE",
                      "You must restart your computer after the updates are installed.");
            ok = tr("SUAppNotif", "CONTINUE_TITLE", "Continue");
        } else {
            message = tr("SUAppNotif", "UPDATES_AVAILABLE_NO_RESTART_MESSAGE",
                         "Software updates are available for your computer. Do you want to install them?");
            info = tr("SUAppNotif", "UPDATES_AVAILABLE_NO_RESTART_INFORMATIVE", "");
            ok = tr("SUAppNotif", "INSTALL_TITLE", "Install");
        }
        m_escapeTitle = tr("SUAppNotif", "NOT_NOW_TITLE", "Not Now");
        Alert a = makeAlert(m_icon, message, info, legalUpdateText(),
                            {{ok, [this] { install(); }},
                             {m_escapeTitle, [] { App::shared().quit(); }},
                             {tr("SUAppNotif", "SHOW_DETAILS_TITLE", "Show Details"), [this] { detailed(); }, true}});
        show(a.view, a.size, false);
    }

    void upToDate()
    {
        m_escapeTitle = tr("SUAppWindow", "QUIT_TITLE", "Quit");
        Alert a = makeAlert(m_icon, tr("SUAppWindow", "UP_TO_DATE_TITLE", "Your software is up to date."),
                            tr("SUAppWindow", "UP_TO_DATE_MESSAGE", "Software Update doesn’t have any new software for your computer at this time."),
                            {}, {{m_escapeTitle, [] { App::shared().quit(); }}});
        show(a.view, a.size, false);
    }

    void failed()
    {
        m_escapeTitle = tr("SUAppWindow", "QUIT_TITLE", "Quit");
        const bool noTool = toolsDir().empty();
        Alert a = makeAlert(m_icon,
                            noTool ? tr("SUAppWindow", "FAILED_SCAN_OTHER_ERROR_TITLE", "Failed to check for updates.")
                                   : tr("SUAppWindow", "FAILED_SCAN_NETWORK_ERROR_TITLE",
                                        "Software Update can't check for updates because of a network problem."),
                            tr("SUAppSession", "STANDARD_NO_SUS_MESSAGE",
                               "Check to make sure your network connection is operating normally."),
                            {}, {{m_escapeTitle, [] { App::shared().quit(); }}});
        show(a.view, a.size, false);
    }

    // ---- SUAppDetailedView.nib: the products, laid out at its frames (512 x 598, from the top)
    void detailed()
    {
        const Size size{512, 598};
        auto *v = new View;
        v->setFrame({0, 0, size.w, size.h});
        auto *iconView = new ImageView;
        iconView->image = m_icon;
        iconView->setFrame({24, 598 - 514 - 64, 64, 64});
        v->addSubview(iconView);
        auto *title = new Label(tr("SUAppWindow", "NEW_SOFTWARE_TITLE", "New software is available for your computer."), Font::boldSystem(13));
        title->setFrame({104, 598 - 557 - 17, 383, 17});
        v->addSubview(title);
        auto *subtitle = new Label(tr("SUAppWindow", "NEW_SOFTWARE_MESSAGE",
                                      "If you don’t want to install now, choose Apple menu > Software Update when you’re ready to install."),
                                   Font::system(11));
        subtitle->wrap = true;
        subtitle->setFrame({104, 598 - 521 - 28, 391, 28});
        v->addSubview(subtitle);
        // the split view: the table (129 high), the description (204) under it
        auto *table = new TableView;
        table->columns = {{"", 20, true}, {"Name", 254}, {"Version", 100}, {"Size", 80}};
        for (const Product &p : m_products)
            table->rows.push_back({m_checked ? "1" : "0", p.title, p.version, sizeString(p.size)});
        table->selected = m_products.empty() ? -1 : 0;
        table->setFrame({20, 598 - 155 - 342, 472, 129});
        table->onCheck = [this](int, int, bool on) {
            m_checked = on;
            if (m_installButton)
                m_installButton->setEnabled(on);
        };
        table->reload();
        v->addSubview(table);
        auto *description = new Label(m_products.empty() ? std::string() : m_products.front().description, Font::system(11));
        description->wrap = true;
        description->background = Color::gray(255);
        auto *box = new Box;
        box->setFrame({20, 598 - 155 - 342 + 138, 472, 204});
        description->setFrame({8, 6, 456, 192});
        box->addSubview(description);
        v->addSubview(box);
        // the restart line
        auto *restartIcon = new ImageView;
        restartIcon->image = Image::load(res("RestartReq.tif"));
        restartIcon->setFrame({20, 598 - 60 - 13, 13, 13});
        v->addSubview(restartIcon);
        auto *restart = new Label(m_products.size() == 1
                                      ? tr("SUAppWindow", "RestartRequired_Single", "You must restart your computer after the update is installed.")
                                      : tr("SUAppWindow", "RestartRequired", "You must restart your computer after the updates are installed."),
                                  Font::system(11));
        restart->setFrame({39, 598 - 59 - 14, 456, 14});
        v->addSubview(restart);
        auto *legal = new Label(legalUpdateText(), Font::system(11));
        legal->wrap = true;
        legal->setFrame({20, 598 - 106 - 41, 472, 41});
        v->addSubview(legal);
        // the buttons
        auto *hide = new Button(tr("SUAppNotif", "HIDE_DETAILS_TITLE", "Hide Details"));
        hide->setFrame({18, 598 - 12 - 32, 116, 32});
        hide->action = [this] { simplified(); };
        v->addSubview(hide);
        m_escapeTitle = tr("SUAppWindow", "NOT_NOW_TITLE", "Not Now");
        auto *notNow = new Button(m_escapeTitle);
        notNow->setFrame({264, 598 - 12 - 32, 96, 32});
        notNow->action = [] { App::shared().quit(); };
        v->addSubview(notNow);
        m_installButton = new Button(m_products.size() == 1 ? tr("SUAppWindow", "Install1Item", "Install 1 Item")
                                                            : format(tr("SUAppWindow", "InstallNItems", "Install %d Items"), {std::to_string(m_products.size())}));
        m_installButton->isDefault = true;
        m_installButton->setFrame({360, 598 - 12 - 32, 138, 32});
        m_installButton->action = [this] { install(); };
        m_installButton->setEnabled(m_checked);
        v->addSubview(m_installButton);
        show(v, size, true);
    }

    // ---- SUAppInstallWindow.nib: the download, then the restart
    void install()
    {
        if (m_products.empty())
            return;
        const Product p = m_products.front();
        if (p.downloaded) {
            restartAlert();
            return;
        }
        m_busy = true;
        if (m_window)
            m_window->close();
        m_window.reset();
        m_content = nullptr;
        openInstallWindow();
        const std::string items = tr("SUAppSessionController", "1_ITEM", "1 item");
        setInstall(format(tr("SUAppSessionController", "DOWNLOADING_X_TITLE", "Downloading %@"), {items}),
                   format(tr("SUAppSessionController", "DOWNLOADING_ITEM_STATUS", "Downloading %@…"), {p.title}), "");
        if (m_stop) {
            m_stop->title = tr("SUAppSessionController", "STOP_TITLE", "Stop");
            m_stop->action = [this] {   // stopDownload: the packages kept, Software Update gone
                m_helper->terminate();
                App::shared().quit();
            };
        }
        m_helper = std::make_unique<Task>();
        m_helper->errors = Task::Errors::Inherit;
        m_started = now();
        m_startBytes = -1;
        m_helper->onLine = [this, p](const std::string &l) {
            double done = 0, total = 0;
            if (std::sscanf(l.c_str(), "PROGRESS %lf %lf", &done, &total) == 2 && total > 0) {
                if (m_startBytes < 0)
                    m_startBytes = done;
                if (m_bar) {
                    m_bar->indeterminate = false;
                    m_bar->maxValue = total;
                    m_bar->value = done;
                    m_bar->setNeedsDisplay();
                }
                const double elapsed = now() - m_started, rate = (done - m_startBytes) / std::max(elapsed, 0.001);
                std::string sizes = format(tr("SUAppSessionController", "FILE_SIZES_STATUS", "%@ of %@"), {sizeString(done), sizeString(total)});
                if (elapsed > 3 && rate > 0)
                    sizes = format(tr("SUAppSessionController", "FILE_SIZES_AND_TIME_ESTIMATE_STATUS", "%@ of %@ — %@"),
                                   {sizeString(done), sizeString(total), timeString((total - done) / rate)});
                if (m_time) {
                    m_time->setText(sizes);
                    m_time->setHidden(false);
                }
            }
        };
        m_helper->onExit = [this, p](int code, const std::string &) {
            if (code != 0) {
                m_busy = false;
                setInstall(tr("SUAppSessionController", "DOWNLOAD_ONLY_FAILED", "An error occurred during download."),
                           format(tr("SUAppSession", "UPDATE_DOWNLOAD_ERROR", "The update “%@” can’t be saved."), {p.title}), "");
                if (m_stop) {
                    m_stop->title = tr("SUAppSessionController", "OK_TITLE", "OK");
                    m_stop->action = [] { App::shared().quit(); };
                }
                return;
            }
            // _sessionWillBeginVerifyingProduct: the packages' sizes were checked as they came
            setInstall(format(tr("SUAppSessionController", "DOWNLOADING_X_TITLE", "Downloading %@"), {tr("SUAppSessionController", "1_ITEM", "1 item")}),
                       format(tr("SUAppSessionController", "EXPANDING_ITEM_STATUS", "Expanding and verifying %@…"), {p.title}), "");
            if (m_bar) {
                m_bar->indeterminate = true;
                m_bar->start();
            }
            App::shared().addTimer(1.5, false, [this] {
                m_busy = false;
                if (m_installWindow)
                    m_installWindow->close();
                m_installWindow.reset();
                m_products.front().downloaded = true;
                restartAlert();
            });
        };
        if (!m_helper->launch(tool({"download", p.key})))
            m_helper->onExit(1, {});
    }

    void openInstallWindow()
    {
        m_installNib = std::make_unique<Nib>();
        View *content = nullptr;
        if (m_installNib->load(res("English.lproj/SUAppInstallWindow.nib/keyedobjects.nib"), res("")))
            content = m_installNib->instantiate("window");
        const Size size = content ? m_installNib->size() : Size{508, 123};
        if (!content) {
            content = new View;
            content->setFrame({0, 0, size.w, size.h});
        }
        m_installWindow = std::make_unique<Window>(size, "Software Update");
        m_installWindow->contentView()->addSubview(content);
        m_installWindow->setIcon(m_icon);
        m_title = m_installNib->outlet<Label>("titleField");
        m_message = m_installNib->outlet<Label>("messageField");
        m_time = m_installNib->outlet<Label>("timeRemainingField");
        m_bar = m_installNib->outlet<ProgressIndicator>("progressIndicator");
        m_stop = m_installNib->outlet<Button>("stopButton");
        if (auto *image = m_installNib->outlet<ImageView>("imageView")) {
            image->image = m_icon;
            image->setNeedsDisplay();
        }
        if (m_time)
            m_time->setHidden(true);
        if (m_bar) {
            m_bar->indeterminate = true;
            m_bar->start();
        }
        m_installWindow->onClose = [this] {
            if (!m_busy)
                App::shared().quit();
        };
        m_installWindow->show();
    }

    void setInstall(const std::string &title, const std::string &message, const std::string &time)
    {
        if (m_title)
            m_title->setText(title);
        if (m_message)
            m_message->setText(message);
        if (m_time) {
            m_time->setText(time);
            m_time->setHidden(time.empty());
        }
    }

    // SUAppSession's: to install a restart-required update, log out and restart
    void restartAlert()
    {
        m_escapeTitle = tr("SUAppSession", "NOT_NOW_TITLE", "Not Now");
        Alert a = makeAlert(m_icon,
                            tr("SUAppSession", "UPDATES_DOWNLOADED_INSTALL_AND_RESTART_MESSAGE", "To install the updates, you must restart your computer."),
                            tr("SUAppSession", "UPDATES_DOWNLOADED_INSTALL_AND_RESTART_INFORMATIVE",
                               "To finish the installation and restart your computer, click Restart."),
                            {}, {{tr("SUAppSession", "LOG_OUT_AND_INSTALL_AND_RESTART_TITLE", "Restart"), [this] { restartAndInstall(); }},
                                 {m_escapeTitle, [] { App::shared().quit(); }}});
        show(a.view, a.size, false);
    }

    // The session (snowleopard-session) ends with SystemUIServer, installs what install-updates
    // names, and starts again
    void restartAndInstall()
    {
        makePath(stateDir());
        std::ofstream(stateDir() + "/install-updates") << m_products.front().key << "\n";
        endSession();
        App::shared().quit();
    }

    // This display's SystemUIServer (not the user's other sessions'): the one whose DISPLAY (or
    // WAYLAND_DISPLAY) is ours
    static void endSession()
    {
        auto ours = [](const char *name) {
            const char *v = std::getenv(name);
            return std::string(name) + "=" + (v ? v : "");
        };
        const std::string display = ours("DISPLAY"), wayland = ours("WAYLAND_DISPLAY");
        DIR *proc = opendir("/proc");
        if (!proc)
            return;
        while (dirent *e = readdir(proc)) {
            const pid_t pid = pid_t(std::atoi(e->d_name));
            if (pid <= 0)
                continue;
            std::string comm;
            std::getline(std::ifstream("/proc/" + std::string(e->d_name) + "/comm"), comm);
            if (comm != "SystemUIServer")
                continue;
            std::ifstream env("/proc/" + std::string(e->d_name) + "/environ");
            std::string var;
            bool sameDisplay = false, sameWayland = true;
            while (std::getline(env, var, '\0')) {
                if (var.rfind("DISPLAY=", 0) == 0)
                    sameDisplay = var == display;
                else if (var.rfind("WAYLAND_DISPLAY=", 0) == 0)
                    sameWayland = var == wayland;
            }
            if (sameDisplay && sameWayland)
                kill(pid, SIGTERM);
        }
        closedir(proc);
    }

    // ---- --install-at-logout: the login screen's picture, the install window over it
    void installAtLogout()
    {
        std::string key;
        std::ifstream(stateDir() + "/install-updates") >> key;
        unlink((stateDir() + "/install-updates").c_str());
        if (key.empty()) {
            App::shared().quit(0);
            return;
        }
        const Rect screen = App::shared().screenFrame();
        m_backdrop = std::make_unique<Window>(Size{screen.w, screen.h}, "", Window::FullScreen);
        auto *picture = new ImageView;
        picture->image = Image::load(Assets::path("/System/Library/CoreServices/DefaultDesktop.jpg"), {screen.w, screen.h});
        picture->setFrame({0, 0, screen.w, screen.h});
        m_backdrop->contentView()->addSubview(picture);
        m_backdrop->show();
        m_busy = true;
        openInstallWindow();   // after the picture: above it, centred
        const Size size = m_installWindow->size();
        m_installWindow->moveTo({std::round((screen.w - size.w) / 2), std::round((screen.h - size.h) / 2)});
        if (m_stop)
            m_stop->setHidden(true);
        const std::string items = tr("SUAppSessionController", "1_ITEM", "1 item");
        setInstall(format(tr("SUAppSessionController", "INSTALLING_X_TITLE", "Installing %@"), {items}),
                   tr("SUAppSessionController", "INSTALLING_ITEM_STATUS", "Installing updates…"), "");
        m_helper = std::make_unique<Task>();
        m_helper->errors = Task::Errors::Inherit;
        m_started = now();
        m_helper->onLine = [this](const std::string &l) {
            double done = 0, total = 0;
            if (std::sscanf(l.c_str(), "PROGRESS %lf %lf", &done, &total) == 2 && total > 0 && m_bar) {
                m_bar->indeterminate = false;
                m_bar->maxValue = total;
                m_bar->value = done;
                m_bar->setNeedsDisplay();
                const double elapsed = now() - m_started;
                if (m_time && elapsed > 5 && done > 0) {
                    m_time->setText("Time Remaining: " + timeString(elapsed * (total - done) / done));
                    m_time->setHidden(false);
                }
            }
        };
        m_helper->onExit = [this](int code, const std::string &) {
            m_busy = false;
            if (m_time)
                m_time->setHidden(true);
            if (code != 0) {
                setInstall(tr("SUAppSessionController", "INSTALLATION_FAILED", "An error occurred during installation."),
                           "", "");
                if (m_stop) {
                    m_stop->title = tr("SUAppSessionController", "OK_TITLE", "OK");
                    m_stop->setHidden(false);
                    m_stop->action = [] { App::shared().quit(1); };
                }
                return;
            }
            if (m_bar) {
                m_bar->indeterminate = false;
                m_bar->value = m_bar->maxValue;
                m_bar->setNeedsDisplay();
            }
            setInstall(tr("SUAppSessionController", "INSTALLATION_DONE_SINGLE", "The update was installed successfully."),
                       tr("SUAppRootInstall", "SYSTEM_WILL_RESTART_MESSAGE", "The computer will restart automatically."), "");
            App::shared().addTimer(3, false, [] { App::shared().quit(0); });
        };
        if (toolsDir().empty() || !m_helper->launch(tool({"install", key}, true)))
            m_helper->onExit(1, {});
    }

    bool m_atLogout;
    Image m_icon;
    MainMenu m_menu;
    std::unique_ptr<Window> m_scanPanel, m_window, m_installWindow, m_backdrop;
    std::unique_ptr<Nib> m_scanNib, m_installNib;
    std::unique_ptr<Task> m_helper;
    View *m_content = nullptr;
    std::vector<Product> m_products;
    bool m_checked = true, m_busy = false;
    std::string m_escapeTitle;
    Button *m_installButton = nullptr, *m_stop = nullptr;
    Label *m_title = nullptr, *m_message = nullptr, *m_time = nullptr;
    ProgressIndicator *m_bar = nullptr;
    double m_started = 0, m_startBytes = -1;
};

} // namespace

int main(int argc, char **argv)
{
    const bool atLogout = argc > 1 && std::strcmp(argv[1], "--install-at-logout") == 0;
    App &app = App::shared();
    setProgramName("Software Update");
    if (!app.open()) {
        std::fprintf(stderr, "Software Update: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    if (!Assets::exists(Bundle + "/Contents/Resources/English.lproj/SUAppInstallWindow.nib/keyedobjects.nib")) {
        std::fprintf(stderr, "Software Update: its files are not in the assets (tools/extract.py takes them from the DVD)\n");
        if (atLogout)
            return 1;
    }
    SoftwareUpdate su(atLogout);
    app.later([&su] { su.start(); });
    return app.run();
}
