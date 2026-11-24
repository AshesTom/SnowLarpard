// SPDX-License-Identifier: LGPL-2.1-or-later
// Spaces.menu's code (AppleSpacesExtra, SpacesStatusView, SpacesCell) on AquaKit's SystemUIPlugin,
// loaded by SystemUIServer as on the Mac. Reversed from 10.6's Spaces.menu 1.7 (82), x86_64, with REA
// (docs/reverse-status.md, section 29):
//
//   - SpacesStatusView -drawRect:: SpacesBackground.pdf as a template image in a SpacesCell, enabled
//     when Spaces is (CoreDockGetWorkspacesEnabled); SpacesCell -drawInteriorWithFrame:inView: the
//     image, then the current space's number (CGSGetWorkspace, 1 to 16; NSNumberFormatter's decimal
//     style) in Lucida Grande Bold 11: white over the dark picture, black with a shadow of white at
//     0.66 when the menu is down (the picture white then);
//   - -menu: "Spaces is not enabled" when it isn't, else an item a space (rows × columns,
//     CoreDockGetWorkspacesCount), "1"…, the current one checked; a line; "Open Spaces
//     Preferences%C" with an ellipsis (Exposé & Spaces' Spaces tab);
//   - -switchToSpace:: com.apple.switchSpaces posted with the space's index from 0, the Dock switches.
//
// On Linux: EWMH's desktops (_NET_NUMBER_OF_DESKTOPS, _NET_DESKTOP_LAYOUT, _NET_CURRENT_DESKTOP); more
// than one is Spaces enabled; the switch a _NET_CURRENT_DESKTOP message to the window manager.
#include "systemuiplugin.h"

#include "foundation.h"
#include "text.h"
#include "windows.h"

#include <cmath>
#include <cstring>

using namespace AK;

namespace {

struct Desktops {
    int count = 1, current = 0;   // current from 0
};

Desktops desktops()
{
    Desktops d;
    if (!Windows::conn())
        return d;
    const auto n = Windows::cardinals(Windows::root(), Windows::atom("_NET_NUMBER_OF_DESKTOPS"), XCB_ATOM_CARDINAL);
    const auto c = Windows::cardinals(Windows::root(), Windows::atom("_NET_CURRENT_DESKTOP"), XCB_ATOM_CARDINAL);
    if (!n.empty())
        d.count = int(n[0]);
    if (!c.empty())
        d.current = int(c[0]);
    return d;
}

void switchTo(int index)
{
    xcb_connection_t *c = Windows::conn();
    if (!c)
        return;
    xcb_client_message_event_t ev;
    std::memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = Windows::root();
    ev.type = Windows::atom("_NET_CURRENT_DESKTOP");
    ev.data.data32[0] = uint32_t(index);
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(c, 0, Windows::root(), XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&ev));
    xcb_flush(c);
}

} // namespace

class AppleSpacesExtra : public SystemUI::MenuExtra {
public:
    explicit AppleSpacesExtra(const std::string &bundle) : MenuExtra(bundle)
    {
        Windows::open();
        m_background = templateImage("SpacesBackground.pdf");
        m_backgroundWhite = templateImage("SpacesBackground.pdf", true);
        setView(new View(this));
        // CGSRegisterConnectionNotifyProc (the space changed): the properties read again
        m_timer = App::shared().addTimer(0.5, true, [this] {
            const Desktops d = desktops();
            if (d.count != m_last.count || d.current != m_last.current) {
                m_last = d;
                setNeedsDisplay();
            }
        });
        m_last = desktops();
    }
    ~AppleSpacesExtra() override
    {
        if (m_timer)
            App::shared().removeTimer(m_timer);
    }

    std::vector<MenuItem> menu() override
    {
        std::vector<MenuItem> m;
        const Desktops d = desktops();
        if (d.count <= 1) {
            m.push_back(MenuItem{tr("Spaces is not enabled", "Spaces is not enabled")});   // doNothing:
        } else {
            for (int i = 1; i <= d.count; ++i) {
                MenuItem item{std::to_string(i)};
                item.checked = i - 1 == d.current;
                item.action = [i] { switchTo(i - 1); };
                m.push_back(item);
            }
        }
        m.push_back(MenuItem::line());
        std::string open = tr("Open Spaces Preferences%C", "Open Spaces Preferences%C");
        const size_t at = open.find("%C");
        if (at != std::string::npos)
            open.replace(at, 2, "…");
        MenuItem prefs{open};
        prefs.action = [] { openPreferencePane("com.apple.preference.expose"); };
        m.push_back(prefs);
        return m;
    }

private:
    class View : public SystemUI::MenuExtraView {
    public:
        explicit View(AppleSpacesExtra *e) : MenuExtraView(e), extra(e) {}
        AppleSpacesExtra *extra;
        void draw(cairo_t *cr) override
        {
            const Rect r{0, 0, frame().w, frame().h - 1};
            const bool down = extra->isMenuDown();
            if (down)
                drawHighlight(cr, r);
            const Desktops d = desktops();
            const bool enabled = d.count > 1;
            const Image &img = down ? extra->m_backgroundWhite : extra->m_background;
            if (img.isNull())
                return;
            // the cell: its image centred, dimmed when disabled
            const Rect at{std::floor((r.w - img.width()) / 2), std::floor((r.h - img.height()) / 2), double(img.width()), double(img.height())};
            img.draw(cr, at, enabled ? 1 : 0.5);
            if (!enabled || d.current < 0 || d.current >= 16)
                return;
            TextStyle t;
            t.font = Font::boldSystem(11);   // Lucida Grande Bold, the system font here
            t.align = Align::Center;
            t.elide = false;
            const std::string n = std::to_string(d.current + 1);
            if (down) {
                // black over the white picture, a shadow of white at 0.66
                t.color = Color::gray(255, 0.66);
                drawText(cr, n, Rect{at.x, at.y + 1, at.w, at.h}, t);
                t.color = Color::gray(0);
            } else {
                t.color = Color::gray(255);
            }
            drawText(cr, n, at, t);
        }
    };

    std::string tr(const char *key, const char *fallback) const { return localized(key, fallback); }

    Image m_background, m_backgroundWhite;
    Desktops m_last;
    int m_timer = 0;
};

SYSTEMUI_REGISTER_EXTRA("AppleSpacesExtra", AppleSpacesExtra);
