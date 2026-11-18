// SPDX-License-Identifier: LGPL-2.1-or-later
#include "systemuiplugin.h"

#include "coreui.h"
#include "foundation.h"

#include <cmath>
#include <cstdlib>
#include <unistd.h>

namespace SystemUI {

MenuExtra::MenuExtra(std::string bundle) : m_bundle(std::move(bundle))
{
    // -defaultLength: NSMenuExtraWidth, else 22
    const std::string w = infoValue("NSMenuExtraWidth");
    m_length = w.empty() ? 22 : std::atof(w.c_str());
    if (m_length <= 0)
        m_length = 22;
}

MenuExtra::~MenuExtra() = default;   // the view is the bar's (a subview), deleted with it

std::string MenuExtra::infoValue(const std::string &key) const
{
    const auto info = AK::Assets::plistStrings(m_bundle + "/Contents/Info.plist");
    auto it = info.find(key);
    return it == info.end() ? std::string() : it->second;
}

std::string MenuExtra::localized(const std::string &key, const std::string &fallback, const std::string &table) const
{
    return AK::Assets::tr(m_bundle, table, key, fallback);
}

std::string MenuExtra::resourcePath(const std::string &name) const
{
    return AK::Assets::path(m_bundle + "/Contents/Resources/" + name);
}

Image MenuExtra::templateImage(const std::string &name, bool white) const
{
    Image i = Image::load(resourcePath(name));
    return white && !i.isNull() ? i.tinted(AK::Color::gray(255)) : i;
}

void MenuExtra::setLength(double length)
{
    if (length == m_length)
        return;
    m_length = length;
    if (host)
        host->extraLengthChanged(this);
}

void MenuExtra::setImage(const Image &image)
{
    m_image = image;
    setNeedsDisplay();
}

void MenuExtra::setAlternateImage(const Image &image)
{
    m_alternate = image;
    setNeedsDisplay();
}

void MenuExtra::setView(MenuExtraView *view)
{
    MenuExtraView *old = m_view;
    m_view = view;
    if (old && old != view) {
        if (host)
            host->extraViewChanged(this, old);
        else
            delete old;
    }
}

MenuExtraView *MenuExtra::view()
{
    if (!m_view)
        m_view = new MenuExtraView(this);
    return m_view;
}

void MenuExtra::setNeedsDisplay()
{
    if (m_view)
        m_view->setNeedsDisplay();
}

void MenuExtra::popUpMenu()
{
    if (!host || m_menuDown)
        return;
    const std::vector<MenuItem> items = menu();
    if (items.empty())
        return;
    m_menuDown = true;
    setNeedsDisplay();
    menuWillOpen();
    host->popUpMenu(this, items, [this] {
        m_menuDown = false;
        setNeedsDisplay();
        menuDidClose();
    });
}

// ---- NSMenuExtraView

void MenuExtraView::drawHighlight(cairo_t *cr, const Rect &r)
{
    static const Image tab = CoreUI::shared(20103);
    if (!tab.isNull()) {
        tab.draw(cr, r);
        return;
    }
    cairo_pattern_t *g = cairo_pattern_create_linear(0, r.y, 0, r.bottom());
    cairo_pattern_add_color_stop_rgb(g, 0, 0x6a / 255.0, 0x8e / 255.0, 0xf8 / 255.0);
    cairo_pattern_add_color_stop_rgb(g, 1, 0x21 / 255.0, 0x55 / 255.0, 0xef / 255.0);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

void MenuExtraView::draw(cairo_t *cr)
{
    // the menu's background (the bar's own under it: nothing to draw unless highlighted; the bottom
    // line is the bar's)
    const Rect r{0, 0, frame().w, frame().h - 1};
    if (m_extra->isMenuDown())
        drawHighlight(cr, r);
    const Image &img = m_extra->isMenuDown() && !m_extra->alternateImage().isNull() ? m_extra->alternateImage() : m_extra->image();
    if (img.isNull())
        return;
    // centred, at its size (an image cell, NSImageScaleNone)
    const double x = std::floor((r.w - img.width()) / 2), y = std::floor((r.h - img.height()) / 2);
    img.draw(cr, AK::Point{x, y});
}

void MenuExtraView::mouseDown(AK::Event &e)
{
    if ((e.modifiers & AK::Command) && m_extra->host) {
        m_commandDrag = true;
        m_extra->host->commandDrag(m_extra, MenuExtra::Host::Drag::Begin, e.screen);
        return;
    }
    m_extra->setModifierFlags(e.modifiers);
    m_extra->popUpMenu();
}

void MenuExtraView::mouseDragged(AK::Event &e)
{
    if (m_commandDrag && m_extra->host)
        m_extra->host->commandDrag(m_extra, MenuExtra::Host::Drag::Move, e.screen);
}

void MenuExtraView::mouseUp(AK::Event &e)
{
    if (!m_commandDrag)
        return;
    m_commandDrag = false;
    if (m_extra->host)
        // may delete the extra (and this view): nothing after it
        m_extra->host->commandDrag(m_extra, MenuExtra::Host::Drag::End, e.screen);
}

void MenuExtra::openPreferencePane(const std::string &pane)
{
    // System Preferences.app beside SystemUIServer.app (apps/SystemUIServer.app/Contents/MacOS/...)
    const std::string apps = AK::dirName(AK::dirName(AK::dirName(AK::dirName(AK::executablePath()))));
    AK::Task::launchDetached({apps + "/System Preferences.app/Contents/MacOS/System Preferences", "--pane", pane});
}

bool MenuExtra::onPath(const std::string &program)
{
    const char *path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin";
    size_t from = 0;
    while (from <= p.size()) {
        const size_t colon = p.find(':', from);
        const std::string dir = p.substr(from, colon == std::string::npos ? std::string::npos : colon - from);
        if (!dir.empty() && access((dir + "/" + program).c_str(), X_OK) == 0)
            return true;
        if (colon == std::string::npos)
            break;
        from = colon + 1;
    }
    return false;
}

bool MenuExtra::startFirst(const std::vector<std::string> &programs, const std::vector<std::string> &args)
{
    for (const std::string &program : programs)
        if (onPath(program)) {
            std::vector<std::string> a{program};
            a.insert(a.end(), args.begin(), args.end());
            return AK::Task::launchDetached(a);
        }
    return false;
}

AK::Font menuBarFont()
{
    return AK::Font::system(14);
}

// ---- the classes

static std::map<std::string, Factory> &classes()
{
    static std::map<std::string, Factory> c;
    return c;
}

void registerClass(const std::string &name, Factory make)
{
    classes()[name] = std::move(make);
}

Factory classNamed(const std::string &name)
{
    auto it = classes().find(name);
    return it == classes().end() ? Factory() : it->second;
}

} // namespace SystemUI
