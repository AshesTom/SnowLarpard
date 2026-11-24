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
        m_view = m_supportsAnimation ? new MenuExtraAnimationView(this, m_length) : new MenuExtraView(this);
    return m_view;
}

// -setAnimationText: and the others: the animation view's, when the extra supports animation
void MenuExtra::setAnimationText(const std::string &text)
{
    if (auto *v = dynamic_cast<MenuExtraAnimationView *>(view()))
        v->setAnimationText(text);
}

void MenuExtra::setStaticText(const std::string &text)
{
    if (auto *v = dynamic_cast<MenuExtraAnimationView *>(view()))
        v->setStaticText(text);
}

void MenuExtra::startTextAnimation()
{
    if (auto *v = dynamic_cast<MenuExtraAnimationView *>(view()))
        v->startTextAnimation();
}

void MenuExtra::stopTextAnimation()
{
    if (auto *v = dynamic_cast<MenuExtraAnimationView *>(view()))
        v->stopTextAnimation();
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
    // DrawMenuTitle, state 0xA8: 20103, Graphite 20104
    const Image tab = CoreUI::shared(CoreUI::graphite() ? 20104 : 20103);
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

// ---- NSMenuExtraAnimationView

namespace {
AK::Font animationFont() { return AK::Font::system(12); }   // -_textAttributes: systemFontOfSize:12
} // namespace

MenuExtraAnimationView::MenuExtraAnimationView(MenuExtra *extra, double minWidth)
    : MenuExtraView(extra), m_minWidth(minWidth)
{
}

MenuExtraAnimationView::~MenuExtraAnimationView()
{
    if (m_timer)
        AK::App::shared().removeTimer(m_timer);
}

void MenuExtraAnimationView::setAnimationText(const std::string &text)
{
    m_text = text;
    m_textFrame = 0;
    m_textImageWidth = text.empty() ? 0 : std::floor(AK::textWidth(text, animationFont()) + 10);
}

void MenuExtraAnimationView::setStaticText(const std::string &text)
{
    setAnimationText(text);
    setMaxWidth(m_minWidth + m_textImageWidth);
    if (m_timer)
        AK::App::shared().removeTimer(m_timer);
    m_timer = 0;
    m_stage = text.empty() ? 0 : 2;
    m_extra->setLength(text.empty() ? m_minWidth : m_maxWidth);
    setNeedsDisplay();
}

void MenuExtraAnimationView::startTextAnimation()
{
    if (m_stage == 0) {
        m_stage = 1;
        if (!m_timer)
            m_timer = AK::App::shared().addTimer(1.0 / 30, true, [this] { tick(); });   // 0.0333 s
    } else if (m_stage == 3) {
        m_stage = 1;
    }
}

void MenuExtraAnimationView::stopTextAnimation()
{
    if (m_stage == 1 || m_stage == 2) {
        m_stage = 3;
    } else if (m_stage == 0 && m_timer) {
        AK::App::shared().removeTimer(m_timer);
        m_timer = 0;
        setNeedsDisplay();
    }
}

void MenuExtraAnimationView::tick()
{
    double w = m_extra->length();
    switch (m_stage) {
    case 1:
        w = std::floor(m_widthIncrement + w);
        if (w >= m_maxWidth) {
            m_stage = 2;
            w = m_maxWidth;
        }
        m_extra->setLength(w);
        break;
    case 2:
        if (++m_textFrame > m_textImageWidth)
            m_textFrame = 0;
        break;
    case 3:
        w = std::floor(w - m_widthIncrement);
        if (w <= m_minWidth) {
            m_stage = 0;
            w = m_minWidth;
        }
        m_extra->setLength(w);
        break;
    default:
        if (m_timer) {
            const int t = m_timer;
            m_timer = 0;
            AK::App::shared().removeTimer(t);   // may be this callback's own: nothing after it
            setNeedsDisplay();
            return;
        }
    }
    setNeedsDisplay();
}

void MenuExtraAnimationView::draw(cairo_t *cr)
{
    const double w = frame().w, h = frame().h;
    const bool down = m_extra->isMenuDown();
    if (down)
        drawHighlight(cr, Rect{0, 0, w, h - 1});
    // the image in the first frame, centred
    const Image &img = down && !m_extra->alternateImage().isNull() ? m_extra->alternateImage() : m_extra->image();
    if (!img.isNull()) {
        const double x = std::floor(std::max(0.0, (m_minWidth - img.width()) / 2));
        const double y = std::floor(std::max(0.0, (h - 1 - img.height()) / 2));
        img.draw(cr, AK::Point{x, y});
    }
    if ((m_stage != 1 && m_stage != 2) || m_text.empty())
        return;
    const AK::Font font = animationFont();
    const double th = AK::lineHeight(font);
    const double x0 = m_minWidth + 5;
    // a point over the middle (Cocoa's y up: floor((h - th) / 2) + 1 from the bottom)
    const double top = h - (std::floor((h - th) / 2) + 1) - th;
    AK::TextStyle style;
    style.font = font;
    style.color = AK::Color::gray(down ? 255 : 0);
    style.elide = false;
    cairo_save(cr);
    cairo_rectangle(cr, x0, 0, std::max(0.0, w - x0), h);
    cairo_clip(cr);
    cairo_push_group(cr);
    // the picture from the scroll's offset, then its start again after its end
    const double first = m_textImageWidth - m_textFrame;
    AK::drawText(cr, m_text, Rect{x0 - m_textFrame, top, m_textImageWidth, th}, style);
    if (first < w)
        AK::drawText(cr, m_text, Rect{x0 + first, top, m_textImageWidth, th}, style);
    if (!down) {
        // the ends faded (NSRectFillListUsingOperation, NSCompositeDestinationIn): white at 0.4, 0.6, 0.8
        static const double alpha[] = {0.4, 0.6, 0.8};
        // (cairo's DEST_IN clears what lies outside its shape: each column clipped and painted)
        cairo_set_operator(cr, CAIRO_OPERATOR_DEST_IN);
        for (int i = 0; i < 3; ++i)
            for (const double x : {x0 + i, w - i - 1}) {
                cairo_save(cr);
                cairo_rectangle(cr, x, top, 1, th);
                cairo_clip(cr);
                cairo_set_source_rgba(cr, 1, 1, 1, alpha[i]);
                cairo_paint(cr);
                cairo_restore(cr);
            }
    }
    cairo_pop_group_to_source(cr);
    cairo_paint(cr);
    cairo_restore(cr);
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

static std::vector<std::string> &registrationOrder()
{
    static std::vector<std::string> order;
    return order;
}

void registerClass(const std::string &name, Factory make)
{
    classes()[name] = std::move(make);
    registrationOrder().push_back(name);
}

size_t classCount() { return registrationOrder().size(); }

std::string classRegisteredAt(size_t index)
{
    return index < registrationOrder().size() ? registrationOrder()[index] : std::string();
}

Factory classNamed(const std::string &name)
{
    auto it = classes().find(name);
    return it == classes().end() ? Factory() : it->second;
}

} // namespace SystemUI
