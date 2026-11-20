// SPDX-License-Identifier: LGPL-2.1-or-later
// SystemUIPlugin.framework on AquaKit: NSMenuExtra and NSMenuExtraView, what SystemUIServer's extras
// are made of, reversed from SystemUIPlugin 1.6 (32) of Mac OS X 10.6.8 (docs/reverse-status.md,
// section 19):
//
//   - -[NSMenuExtra initWithBundle:]: the extra knows its bundle (its resources, its strings, its
//     Info.plist); its length the Info.plist's NSMenuExtraWidth, else 22 (-defaultLength);
//   - NSMenuExtraView -drawRect: the menu's background, highlighted while the extra's menu is down,
//     then the extra's image, its alternate image while the menu is down, centred;
//   - -mouseDown: pops the extra's menu up under it (-popUpMenu:, through SystemUIServer's
//     NSMenuToolbarItem): the extra is "menu down" until it closes.
//
// The classes SystemUIServer finds by name (the bundle's NSPrincipalClass, looked up in SystemUIServer
// first, NSClassFromString) register here: the built-in ones (AppleClockExtra) with SystemUIServer, a
// bundle's when SystemUIServer loads its library.
#pragma once

#include "appkit.h"
#include "controls.h"
#include "text.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace SystemUI {

using AK::Image;
using AK::MenuItem;
using AK::Rect;

class MenuExtraView;

class MenuExtra {
public:
    // -initWithBundle: `bundle` is its Mac path (/System/Library/CoreServices/Menu Extras/Clock.menu)
    explicit MenuExtra(std::string bundle);
    virtual ~MenuExtra();
    MenuExtra(const MenuExtra &) = delete;
    MenuExtra &operator=(const MenuExtra &) = delete;

    const std::string &bundlePath() const { return m_bundle; }
    // the bundle's Info.plist value (strings, numbers as text)
    std::string infoValue(const std::string &key) const;
    // -[NSBundle localizedStringForKey:value:table:] in the bundle's English.lproj
    std::string localized(const std::string &key, const std::string &fallback, const std::string &table = "Localizable") const;
    // a resource's file (the DVD's, at its Mac path in the assets)
    std::string resourcePath(const std::string &name) const;
    // a template picture of the bundle (its PDFs), at the menu bar's scale
    Image templateImage(const std::string &name, bool white = false) const;

    // NSStatusItem's length: what the bar gives it
    double length() const { return m_length; }
    void setLength(double length);
    void setImage(const Image &image);
    void setAlternateImage(const Image &image);
    const Image &image() const { return m_image; }
    const Image &alternateImage() const { return m_alternate; }
    // -setView: (takes it; a default NSMenuExtraView otherwise)
    void setView(MenuExtraView *view);
    MenuExtraView *view();

    // -menu: the items to drop down (none: a click does nothing more)
    virtual std::vector<MenuItem> menu() { return {}; }
    virtual void menuWillOpen() {}
    virtual void menuDidClose() {}
    virtual void willUnload() {}
    // the menu bar's tracking opens it when the pointer slides onto it with another menu open
    virtual bool slidesFromMenus() const { return true; }
    bool isMenuDown() const { return m_menuDown; }
    // the modifier keys of the click that opened the menu ([NSEvent modifierFlags]: Option's details)
    unsigned modifierFlags() const { return m_modifiers; }
    void setModifierFlags(unsigned m) { m_modifiers = m; }
    // System Preferences on a pane ("network", "bluetooth", or an identifier)
    static void openPreferencePane(const std::string &pane);
    // the first of these programs on the PATH, started; false if none is
    static bool startFirst(const std::vector<std::string> &programs, const std::vector<std::string> &args = {});
    static bool onPath(const std::string &program);
    void popUpMenu();   // -popUpMenu:
    void setNeedsDisplay();

    // SystemUIServer's side (NSMenuToolbar): told of a new length; pops the menus up
    struct Host {
        virtual ~Host() = default;
        virtual void extraLengthChanged(MenuExtra *) = 0;
        virtual void extraViewChanged(MenuExtra *, MenuExtraView *old) = 0;   // -setView: (old deleted by it)
        virtual void popUpMenu(MenuExtra *, const std::vector<MenuItem> &items, std::function<void()> closed) = 0;
        // Command-drag (NSMenuToolbarView): the extra moved along the bar, or off it to remove it
        enum class Drag { Begin, Move, End };
        virtual void commandDrag(MenuExtra *, Drag, AK::Point /*screen*/) {}
    };
    Host *host = nullptr;

private:
    std::string m_bundle;
    double m_length;
    Image m_image, m_alternate;
    MenuExtraView *m_view = nullptr;
    bool m_menuDown = false;
    unsigned m_modifiers = 0;
};

class MenuExtraView : public AK::View {
public:
    explicit MenuExtraView(MenuExtra *extra) : m_extra(extra) {}
    MenuExtra *menuExtra() const { return m_extra; }
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    // the menu bar's highlight (CUIRenderer::DrawMenuTitle's selected title, SArtFile 20103)
    static void drawHighlight(cairo_t *cr, const Rect &r);

protected:
    MenuExtra *m_extra;
    bool m_commandDrag = false;
};

// The menu bar's text: 14 points (CLKDigitalView's systemFontOfSize:14), black, white while highlighted
AK::Font menuBarFont();
constexpr double MenuBarHeight = 22;

// The principal classes by name (NSClassFromString)
using Factory = std::function<MenuExtra *(const std::string &bundle)>;
void registerClass(const std::string &name, Factory make);
Factory classNamed(const std::string &name);

} // namespace SystemUI

// A bundle's principal class, registered when its library loads
#define SYSTEMUI_REGISTER_EXTRA(ClassName, Type)                                                       \
    static const bool registered_##Type = [] {                                                         \
        SystemUI::registerClass(ClassName, [](const std::string &b) { return new Type(b); });          \
        return true;                                                                                   \
    }()
