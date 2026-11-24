// SPDX-License-Identifier: LGPL-2.1-or-later
// User.menu's code (AppleUser, fast user switching) on AquaKit's SystemUIPlugin, loaded by SystemUIServer
// as on the Mac. Reversed from 10.6's User.menu 4.0, x86_64, with REA (docs/reverse-status.md,
// section 26):
//
//   - the bar (UserSwitcherView): by userMenuExtraStyle (the global domain's) 0 the current user's full
//     name (the default), 1 the short name, 2 the UserSwitcher.pdf template; the name in the bold
//     system font of 14, centred, cut in the middle (NSLineBreakByTruncatingMiddle), its cell one point
//     down; the view the picture's or the text's width + 4, between 22 and 160 (-updateSize);
//   - the menu (-createMenu:, -updateMenu:withUserList:): a USMenuItem a user, sorted by full name, the
//     name cut to 320 points; "Building user list" until there is one; a line; "Login Window…"
//     (logOut:, CGSCreateLoginSession); a line; "Account Preferences…" (openAccounts:);
//   - USMenuItem (MenuItem.nib, -awakeFromNib): a view 360 x 38, the mark at (4, 11), the user's
//     picture 36 x 36 at (24, 1), the name (menu font 14) at (61, 16) 278 x 18, the status (14 high)
//     at (61, 3), Quartz's y upwards; without a status the name centred down the item (-setStatus:);
//   - -updateMenuItemStatus:forUser:loginAllowed:: the current user's item disabled; the state the
//     user's session elsewhere (isSuspended: greenmarker.tiff) when the user may log in, else mixed
//     (noentry, with "Can’t log in right now" / "Can log in at %@": Parental Controls' curfews);
//   - -switchToUser: on a thread: the user's session, or the login window asking for them.
//
// On Linux: the accounts of /etc/passwd that log in (UIDs 1000 to 59999, a shell), their sessions from
// logind (loginctl), the switch through the display manager (LightDM's dm-tool, GDM, the
// org.freedesktop.DisplayManager seat).
#include "systemuiplugin.h"

#include "foundation.h"
#include "text.h"

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <set>
#include <sstream>

using namespace AK;

namespace {

struct User {
    std::string shortName, longName, home;
    uid_t uid = 0;
};

// CachedUserList: the accounts that log in, by full name (-cachedUserLongNameCompare:)
std::vector<User> users()
{
    std::vector<User> out;
    setpwent();
    while (passwd *p = getpwent()) {
        if (p->pw_uid < 1000 || p->pw_uid >= 60000)
            continue;
        const std::string shell = p->pw_shell ? p->pw_shell : "";
        if (shell.empty() || shell.find("nologin") != std::string::npos || shell.find("/false") != std::string::npos)
            continue;
        User u;
        u.shortName = p->pw_name;
        std::string gecos = p->pw_gecos ? p->pw_gecos : "";
        gecos = gecos.substr(0, gecos.find(','));
        u.longName = gecos.empty() ? u.shortName : gecos;
        u.home = p->pw_dir ? p->pw_dir : "";
        u.uid = p->pw_uid;
        out.push_back(u);
    }
    endpwent();
    std::sort(out.begin(), out.end(), [](const User &a, const User &b) { return finderLess(a.longName, b.longName); });
    return out;
}

// the users with a session (logind's), for isSuspended
std::set<std::string> usersWithSessions()
{
    std::set<std::string> out;
    std::string text;
    // a short synchronous read: the menu is built when it opens
    if (FILE *p = popen("loginctl list-sessions --no-legend 2>/dev/null", "r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, p))
            text += buf;
        pclose(p);
    }
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::istringstream l(line);
        std::string session, uid, user;
        if (l >> session >> uid >> user)
            out.insert(user);
    }
    return out;
}

// "…" in the middle so that the text fits `width`
std::string truncatedMiddle(const std::string &text, double width, const Font &font)
{
    if (textWidth(text, font) <= width)
        return text;
    std::vector<size_t> starts;   // the UTF-8 characters
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
            starts.push_back(i);
    const size_t n = starts.size();
    for (size_t keep = n; keep-- > 1;) {
        const size_t head = (keep + 1) / 2, tail = keep / 2;
        const size_t from = tail ? starts[n - tail] : text.size();
        const std::string s = text.substr(0, starts[head]) + "…" + text.substr(from);
        if (textWidth(s, font) <= width)
            return s;
    }
    return "…";
}

} // namespace

class AppleUser : public SystemUI::MenuExtra {
public:
    explicit AppleUser(const std::string &bundle) : MenuExtra(bundle)
    {
        for (const User &u : users())
            if (u.uid == getuid())
                m_current = u;
        if (m_current.shortName.empty())
            m_current.shortName = m_current.longName = programName();
        setView(new View(this));
        setMenuBarTitleOrPicture();
        // userMenuExtraStyle changed by Accounts' Login Options: read again as the bar redraws
        m_timer = App::shared().addTimer(2, true, [this] { setMenuBarTitleOrPicture(); });
    }
    ~AppleUser() override
    {
        if (m_timer)
            App::shared().removeTimer(m_timer);
    }

    std::vector<MenuItem> menu() override
    {
        // -createMenu:
        std::vector<MenuItem> m;
        const std::vector<User> list = users();
        if (list.empty()) {
            MenuItem building{tr("BUILDING_USER_LIST", "Building user list")};
            building.enabled = false;
            m.push_back(building);
        }
        const std::set<std::string> sessions = usersWithSessions();
        static const Image check = Image::load(resourcePath("greenmarker.tiff"));
        static const Image noEntry = Image::load(resourcePath("noentry.png"));
        for (const User &u : list) {
            // -updateMenu:withUserList:, -updateMenuItemStatus:forUser:loginAllowed: (no curfews here:
            // every user may log in, no status line)
            const bool current = u.uid == getuid();
            const bool suspended = !current && sessions.count(u.shortName);
            const Image picture = userPicture(u);
            const std::string name = truncatedMiddle(u.longName, 320, Font::system(14));
            MenuItem item{name};
            item.enabled = !current;
            item.viewHeight = 38;
            item.viewWidth = 360;
            const bool enabled = item.enabled;
            item.drawItem = [name, picture, suspended, enabled](cairo_t *cr, const Rect &row, bool hi) {
                // MenuItem.nib's view, its frames turned top down (38 high); disabled (-setEnabled:):
                // the pictures' disabledImage (here at half their alpha), the name in
                // disabledControlTextColor
                const double alpha = enabled ? 1 : 0.5;
                if (suspended && !check.isNull())
                    check.draw(cr, Point{row.x + 4, row.y + 38 - 11 - check.height()}, alpha);
                if (!picture.isNull())
                    picture.draw(cr, Rect{row.x + 24, row.y + 1, 36, 36}, alpha);
                TextStyle t;
                t.font = Font::system(14);
                t.color = !enabled ? Color::gray(0, 0.3) : hi ? Color::gray(255) : Color::gray(0);
                // -setStatus: with none: the name centred, (38 - 18) / 2 from the bottom
                drawText(cr, name, Rect{row.x + 61, row.y + 10, std::min(278.0, row.w - 61), 18}, t);
            };
            (void)noEntry;   // the mixed state's picture: Parental Controls only
            const std::string shortName = u.shortName;
            item.action = [shortName] { switchToUser(shortName); };
            m.push_back(item);
        }
        m.push_back(MenuItem::line());
        MenuItem login{tr("LOG_OUT_STRING", "Login Window…")};
        login.action = [] { loginWindow(); };
        m.push_back(login);
        m.push_back(MenuItem::line());
        MenuItem accounts{tr("ACCOUNT_PREFERENCES_STRING", "Account Preferences…")};
        accounts.action = [] { openPreferencePane("com.apple.preferences.users"); };
        m.push_back(accounts);
        return m;
    }

    // what UserSwitcherView shows: 0 an image, 1 text (-setDisplayType:)
    int displayType = 1;
    std::string title;
    Image picture;

private:
    class View : public SystemUI::MenuExtraView {
    public:
        explicit View(AppleUser *u) : MenuExtraView(u), user(u) {}
        AppleUser *user;
        void draw(cairo_t *cr) override
        {
            const Rect r{0, 0, frame().w, frame().h - 1};
            if (user->isMenuDown())
                drawHighlight(cr, r);
            if (user->displayType == 0) {
                const Image &img = user->isMenuDown() ? user->alternateImage() : user->image();
                if (!img.isNull())
                    img.draw(cr, Point{std::floor((r.w - img.width()) / 2), std::floor((r.h - img.height()) / 2)});
                return;
            }
            TextStyle t;
            t.font = Font::boldSystem(14);
            t.color = user->isMenuDown() ? Color::gray(255) : Color::gray(0);
            t.align = Align::Center;
            t.elide = false;
            // -drawRect:: the text cell's frame one point down
            drawText(cr, truncatedMiddle(user->title, r.w, t.font), Rect{0, 1, r.w, r.h - 1}, t);
        }
    };

    std::string tr(const char *key, const char *fallback) const { return localized(key, fallback); }

    // -setMenuBarTitleOrPicture, -titleDisplayType, -updateSize
    void setMenuBarTitleOrPicture()
    {
        int style = Settings("appearance").integer("userMenuExtraStyle", 0);
        if (style < 0 || style > 2)
            style = 2;
        const int type = style == 2 ? 0 : 1;
        const std::string t = style == 0 ? m_current.longName : style == 1 ? m_current.shortName : std::string();
        if (type == displayType && t == title && (type == 1 || !image().isNull()))
            return;
        displayType = type;
        title = t;
        double width = 0;
        if (type == 0) {
            setImage(templateImage("UserSwitcher.pdf"));
            setAlternateImage(templateImage("UserSwitcher.pdf", true));
            width = image().width();
        } else {
            width = std::ceil(textWidth(title, Font::boldSystem(14)));
        }
        setLength(std::clamp(width + 2 + 2, 22.0, 160.0));
        setNeedsDisplay();
    }

    // _UserPicture: the account's picture (Accounts'; AccountsService's or ~/.face here), else the
    // generic user's
    static Image userPicture(const User &u)
    {
        for (const std::string &p : {"/var/lib/AccountsService/icons/" + u.shortName, u.home + "/.face.icon", u.home + "/.face"})
            if (isFile(p)) {
                Image img = Image::load(p, {36, 36});
                if (!img.isNull())
                    return img;
            }
        return Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/UserIcon.icns"), {36, 36});
    }

    // -switchToUserThread: the user's session if there is one, else the login window for them
    static void switchToUser(const std::string &name)
    {
        Task::launchDetached({"sh", "-c",
                              "dm-tool switch-to-user \"$0\" 2>/dev/null || gdmflexiserver 2>/dev/null || "
                              "busctl call org.freedesktop.DisplayManager /org/freedesktop/DisplayManager/Seat0 "
                              "org.freedesktop.DisplayManager.Seat SwitchToUser ss \"$0\" '' 2>/dev/null",
                              name});
    }

    // -logOut: (CGSCreateLoginSession): the login window over this session, which goes on
    static void loginWindow()
    {
        Task::launchDetached({"sh", "-c",
                              "dm-tool switch-to-greeter 2>/dev/null || gdmflexiserver 2>/dev/null || "
                              "busctl call org.freedesktop.DisplayManager /org/freedesktop/DisplayManager/Seat0 "
                              "org.freedesktop.DisplayManager.Seat SwitchToGreeter 2>/dev/null || loginctl lock-session"});
    }

    User m_current;
    int m_timer = 0;
};

SYSTEMUI_REGISTER_EXTRA("AppleUser", AppleUser);
