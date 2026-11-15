// SPDX-License-Identifier: LGPL-2.1-or-later
#include "controls.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <cmath>
#include <memory>

namespace AK {

CoreUI::State controlState(const View &v, bool pressed)
{
    CoreUI::State s;
    s.enabled = v.isEnabled();
    s.pressed = pressed;
    s.windowActive = v.windowIsKey();
    s.focused = v.isFirstResponder();
    return s;
}

// ---- Label

void Label::setText(const std::string &t)
{
    if (t != m_text) {
        m_text = t;
        setNeedsDisplay();
    }
}

void Label::draw(cairo_t *cr)
{
    if (background.a > 0)
        fill(cr, bounds(), background);
    TextStyle s = style;
    if (!isEnabled())
        s.color = textColor(false);
    if (wrap)
        drawWrappedText(cr, m_text, Rect{0, 0, frame().w, frame().h}, s);
    else
        drawText(cr, m_text, bounds(), s);
}

// ---- Control: tracked as -[NSCell trackMouse:inRect:ofView:untilMouseUp:]

void Control::mouseDown(Event &)
{
    m_pressed = m_inside = true;
    setNeedsDisplay();
}

void Control::mouseDragged(Event &e)
{
    const bool inside = bounds().contains(e.location);
    if (inside != m_inside) {
        m_inside = m_pressed = inside;
        setNeedsDisplay();
    }
}

void Control::mouseUp(Event &e)
{
    const bool fire = m_pressed && bounds().contains(e.location);
    m_pressed = m_inside = false;
    setNeedsDisplay();
    if (fire)
        clicked();
}

// ---- Button

void Button::draw(cairo_t *cr)
{
    CoreUI::State s = controlState(*this, m_pressed);
    s.small = small;
    TextStyle t;
    t.align = Align::Center;
    t.color = titleColor.a > 0 ? titleColor : textColor(isEnabled());
    if (kind == Plain)
        t.font = font;
    switch (kind) {
    case Push:
        CoreUI::pushButton(cr, bounds(), s, isDefault);
        // the bezel's face is above its shadow: the title centred on it
        drawText(cr, title, Rect{8, 0, frame().w - 16, frame().h - 3}, t);
        break;
    case Textured:
        CoreUI::segments(cr, CoreUI::SegmentStyle::Textured, {bounds()}, {false}, m_pressed ? 0 : -1, s);
        if (!image.isNull())
            image.draw(cr, Point{std::floor((frame().w - image.width()) / 2), std::floor((frame().h - 1 - image.height()) / 2)},
                       isEnabled() ? 1 : 0.4);
        else
            drawText(cr, title, Rect{6, 0, frame().w - 12, frame().h - 1}, t);
        break;
    case Help:
        CoreUI::helpButton(cr, bounds(), s);
        break;
    case SmallSquare: {
        // a white face going to 241 at the bottom (pressed: 196 to 180), a 169 edge
        const Rect r = bounds();
        cairo_pattern_t *g = cairo_pattern_create_linear(0, r.y, 0, r.bottom());
        const double top = (s.pressed ? 196 : 255) / 255.0, bottom = (s.pressed ? 180 : 241) / 255.0;
        cairo_pattern_add_color_stop_rgb(g, 0, top, top, top);
        cairo_pattern_add_color_stop_rgb(g, 1, bottom, bottom, bottom);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        Color::gray(169).set(cr);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
        cairo_stroke(cr);
        t.font = Font::boldSystem(13);
        drawText(cr, title, Rect{0, 0, r.w, r.h}, t);
        if (!image.isNull())
            image.draw(cr, Point{std::floor((r.w - image.width()) / 2), std::floor((r.h - image.height()) / 2)}, isEnabled() ? 1 : 0.4);
        break;
    }
    case Round: {
        // NSCircularBezelStyle: roundbutton, its image or title in the middle
        CoreUI::draw(cr, CoreUI::element({"roundbutton", m_pressed ? "blue" : "clear", m_pressed ? "pressed" : "active", "regular"}), bounds());
        if (!image.isNull())
            image.draw(cr, Point{std::floor((frame().w - image.width()) / 2), std::floor((frame().h - image.height()) / 2)});
        else
            drawText(cr, title, Rect{0, 0, frame().w, frame().h - 2}, t);
        break;
    }
    case Disclosure:
        CoreUI::disclosureTriangle(cr, Rect{0, std::floor((frame().h - 13) / 2), 13, 13}, open, m_pressed);
        if (!title.empty()) {
            t.align = Align::Left;
            drawText(cr, title, Rect{16, 0, frame().w - 16, frame().h}, t);
        }
        break;
    case Plain:
        // borderless: the image, or the title
        if (!image.isNull()) {
            Size sz = image.size();
            if (sz.w > frame().w || sz.h > frame().h)
                sz = scaledToFit(sz, {frame().w, frame().h});
            image.draw(cr, Rect{std::floor((frame().w - sz.w) / 2), std::floor((frame().h - sz.h) / 2), sz.w, sz.h},
                       m_pressed ? 0.6 : isEnabled() ? 1 : 0.4);
        } else {
            drawText(cr, title, bounds(), t);
        }
        break;
    }
    if (kind == Push && !image.isNull())
        image.draw(cr, Point{std::floor((frame().w - image.width()) / 2), std::floor((frame().h - 3 - image.height()) / 2)});
}

void Button::clicked()
{
    if (kind == Disclosure)
        open = !open;
    Control::clicked();
}

// ---- CheckBox

void CheckBox::setChecked(bool c)
{
    if (c != m_checked) {
        m_checked = c;
        setNeedsDisplay();
    }
}

void CheckBox::clicked()
{
    if (radio)
        setChecked(true);
    else
        setChecked(!m_checked);
    if (onToggle)
        onToggle(this);
    Control::clicked();
}

void CheckBox::draw(cairo_t *cr)
{
    CoreUI::State s = controlState(*this, m_pressed);
    s.small = small;
    const double box = small ? 14 : 18;
    TextStyle t;
    t.font = font;
    t.color = textColor(isEnabled());
    // the box, its title beside it (or under it: NSImageAbove)
    const Rect art = imageAbove ? Rect{std::floor((frame().w - box) / 2), 0, box, box} : Rect{0, std::floor((frame().h - box) / 2), box, box};
    if (radio)
        CoreUI::radioButton(cr, art, s, m_checked);
    else
        CoreUI::checkBox(cr, art, s, m_checked ? 1 : 0);
    if (imageAbove) {
        t.align = Align::Center;
        drawText(cr, title, Rect{0, box, frame().w, frame().h - box}, t);
    } else {
        drawText(cr, title, Rect{box + 2, 0, frame().w - box - 2, frame().h}, t);
    }
}

// ---- SegmentedControl

SegmentedControl::SegmentedControl(CoreUI::SegmentStyle style, std::vector<std::string> labels, std::vector<double> widths)
    : m_style(style), m_labels(std::move(labels)), m_widths(std::move(widths))
{
    segmentEnabled.assign(m_labels.size(), true);
}

void SegmentedControl::setSelected(int i)
{
    if (i != m_selected) {
        m_selected = i;
        setNeedsDisplay();
    }
}

void SegmentedControl::setSegments(std::vector<std::string> labels, std::vector<double> widths)
{
    m_labels = std::move(labels);
    m_widths = std::move(widths);
    segmentEnabled.assign(m_labels.size(), true);
    setNeedsDisplay();
}

std::vector<Rect> SegmentedControl::rects() const
{
    std::vector<Rect> out;
    double x = 0;
    for (double w : m_widths) {
        out.push_back({x, 0, w, frame().h});
        x += w;
    }
    return out;
}

void SegmentedControl::draw(cairo_t *cr)
{
    const std::vector<Rect> r = rects();
    std::vector<bool> sel(r.size(), false);
    if (!momentary && m_selected >= 0 && size_t(m_selected) < sel.size())
        sel[size_t(m_selected)] = true;
    CoreUI::segments(cr, m_style, r, sel, m_pressedSegment, controlState(*this));
    for (size_t i = 0; i < r.size(); ++i) {
        const bool enabled = isEnabled() && segmentEnabled[i];
        const Rect label{r[i].x + 4, 0, r[i].w - 8, frame().h - (m_style == CoreUI::SegmentStyle::Tabs ? 2 : 1)};
        if (glyph) {
            glyph(cr, int(i), label, enabled);
            continue;
        }
        TextStyle t;
        t.align = Align::Center;
        t.color = textColor(enabled);
        drawText(cr, m_labels[i], label, t);
    }
}

void SegmentedControl::mouseDown(Event &e)
{
    const std::vector<Rect> r = rects();
    m_downSegment = -1;
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i].contains(e.location) && segmentEnabled[i])
            m_downSegment = int(i);
    m_pressedSegment = m_downSegment;
    setNeedsDisplay();
}

void SegmentedControl::mouseDragged(Event &e)
{
    const std::vector<Rect> r = rects();
    const int now = m_downSegment >= 0 && r[size_t(m_downSegment)].contains(e.location) ? m_downSegment : -1;
    if (now != m_pressedSegment) {
        m_pressedSegment = now;
        setNeedsDisplay();
    }
}

void SegmentedControl::mouseUp(Event &)
{
    const int hit = m_pressedSegment;
    m_pressedSegment = m_downSegment = -1;
    setNeedsDisplay();
    if (hit < 0)
        return;
    if (!momentary)
        m_selected = hit;
    if (onSelect)
        onSelect(hit);
    if (action)
        action();
}

// ---- Menu

namespace {

class MenuView : public View {
public:
    std::vector<MenuItem> items;
    Font font;
    int highlighted = -1;
    std::function<void(int)> done;
    double opened = now();
    bool moved = false;

    int itemAt(Point p) const
    {
        if (p.x < 0 || p.x >= frame().w)
            return -1;
        for (size_t i = 0; i < items.size(); ++i) {
            const double top = Menu::itemTop(items, int(i));
            const double h = items[i].separator ? Menu::SeparatorHeight : Menu::ItemHeight;
            if (p.y >= top && p.y < top + h)
                return items[i].separator || !items[i].enabled ? -1 : int(i);
        }
        return -1;
    }
    void draw(cairo_t *cr) override
    {
        CoreUI::menuBackground(cr, bounds());
        for (size_t i = 0; i < items.size(); ++i) {
            const double top = Menu::itemTop(items, int(i));
            if (items[i].separator) {
                // HIToolbox: a line in _CUIColorGet(0xE2) across the middle
                fill(cr, Rect{1, top + 5, frame().w - 2, 1}, CoreUI::color(CoreUI::MenuSeparator));
                continue;
            }
            const Rect row{0, top, frame().w, Menu::ItemHeight};
            const bool hi = int(i) == highlighted;
            if (hi)
                CoreUI::menuSelection(cr, row);
            TextStyle t;
            t.font = font;
            t.color = !items[i].enabled ? Color::gray(0, 0.3) : hi ? Color::gray(255) : Color::gray(0);
            if (items[i].checked)
                drawText(cr, "✓", Rect{6, top, 14, Menu::ItemHeight}, t);
            drawText(cr, items[i].title, Rect{21, top, frame().w - 21 - 18, Menu::ItemHeight}, t);
        }
    }
    void mouseMoved(Event &e) override { track(e.location); }
    void mouseDragged(Event &e) override { track(e.location); }
    void mouseExited(Event &) override
    {
        highlighted = -1;
        setNeedsDisplay();
    }
    void track(Point p)
    {
        moved = true;
        const int i = itemAt(p);
        if (i != highlighted) {
            highlighted = i;
            setNeedsDisplay();
        }
    }
    void mouseUp(Event &e) override
    {
        // a press and release on the button opens it; a release on an item after dragging chooses
        if (!moved && now() - opened < 0.3)
            return;
        finish(itemAt(e.location));
    }
    void mouseDown(Event &) override {}
    void keyDown(Event &e) override
    {
        if (e.keysym == XKB_KEY_Escape)
            finish(-1);
        else if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter || e.keysym == XKB_KEY_space)
            finish(highlighted);
        else if (e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_Up) {
            const int step = e.keysym == XKB_KEY_Down ? 1 : -1;
            for (int i = highlighted + step; i >= 0 && i < int(items.size()); i += step)
                if (!items[size_t(i)].separator && items[size_t(i)].enabled) {
                    highlighted = i;
                    break;
                }
            setNeedsDisplay();
        }
    }
    View *hitTest(Point p) override { return frame().contains(p) ? this : nullptr; }
    void finish(int index);
};

std::unique_ptr<Window> openMenu;

void MenuView::finish(int index)
{
    auto cb = done;
    Window *w = window();
    done = nullptr;
    // its window goes after this event
    App::shared().later([] { openMenu.reset(); });
    if (w)
        w->close();
    if (cb)
        cb(index);
}

} // namespace

double Menu::itemTop(const std::vector<MenuItem> &items, int index)
{
    double y = Padding;
    for (int i = 0; i < index && size_t(i) < items.size(); ++i)
        y += items[size_t(i)].separator ? SeparatorHeight : ItemHeight;
    return y;
}

void Menu::popUp(Window *parent, Point at, const std::vector<MenuItem> &items, double minWidth, Font font, std::function<void(int)> done)
{
    double w = minWidth;
    for (const MenuItem &i : items)
        w = std::max(w, 21 + textWidth(i.title, font) + 30);
    const double h = itemTop(items, int(items.size())) + Padding;
    // the window system keeps it on the screen (X11: under the menu bar; Wayland: the compositor slides it)
    openMenu = std::make_unique<Window>(Size{std::ceil(w), std::ceil(h)}, "menu", Window::Menu, parent, at);
    auto *v = openMenu->contentView()->add<MenuView>();
    v->setFrame({0, 0, std::ceil(w), std::ceil(h)});
    v->items = items;
    v->font = font;
    v->done = std::move(done);
    openMenu->makeFirstResponder(v);
    openMenu->onOutsideClick = [v](Event &e) {
        if (e.type == Event::MouseDown)
            v->finish(-1);
    };
    openMenu->show();
    openMenu->grabInput();
}

// ---- PopUpButton

void PopUpButton::setSelected(int i)
{
    if (i != m_selected) {
        m_selected = i;
        setNeedsDisplay();
    }
}

void PopUpButton::draw(cairo_t *cr)
{
    CoreUI::State s = controlState(*this, m_open);
    s.small = small;
    CoreUI::popUpButton(cr, bounds(), s);
    TextStyle t;
    t.color = textColor(isEnabled());
    t.font = Font::system(small ? 11 : 13);
    const int shown = pullDown ? 0 : m_selected;
    if (shown >= 0 && size_t(shown) < items.size())
        drawText(cr, items[size_t(shown)].title, Rect{small ? 9.0 : 11.0, 0, frame().w - 11 - 26, frame().h - 2}, t);
}

void PopUpButton::mouseDown(Event &)
{
    Window *w = window();
    if (!w || items.empty())
        return;
    std::vector<MenuItem> shown = items;
    if (pullDown && !shown.empty())
        shown.erase(shown.begin());   // a pull-down's title isn't in its menu
    for (size_t i = 0; i < shown.size(); ++i)
        shown[i].checked = !pullDown && int(i) == m_selected;
    // the chosen item over the button's title (-[NSPopUpButtonCell attachPopUpWithFrame:inView:])
    const Point origin = toWindow({0, 0});
    // a pull-down's menu opens under it
    const double itemTop = pullDown ? -frame().h + 2 : Menu::itemTop(shown, std::max(0, m_selected));
    const Point at{origin.x - 10, origin.y + std::floor((frame().h - 2 - Menu::ItemHeight) / 2) - itemTop};
    m_open = true;
    setNeedsDisplay();
    Menu::popUp(w, at, shown, frame().w + 2, Font::system(small ? 11 : 13), [this](int chosen) {
        m_open = false;
        setNeedsDisplay();
        if (pullDown && chosen >= 0) {
            if (onSelect)
                onSelect(chosen + 1);
            if (action)
                action();
            return;
        }
        if (chosen < 0 || chosen == m_selected)
            return;
        m_selected = chosen;
        if (onSelect)
            onSelect(chosen);
        if (action)
            action();
    });
}

// ---- ColorWell

void ColorWell::draw(cairo_t *cr)
{
    // NSColorWell's bezel: a button face, the colour inset in it with a dark edge
    CoreUI::State s = controlState(*this, m_pressed);
    const Rect r = bounds();
    fill(cr, r, Color::gray(s.pressed ? 190 : 232));
    Color::gray(128).set(cr);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
    cairo_stroke(cr);
    const Rect inner = r.inset(5, 5);
    Color c = color;
    if (!isEnabled())
        c.a = 0.4;
    fill(cr, inner, c);
    Color::gray(0, 0.4).set(cr);
    cairo_rectangle(cr, inner.x + 0.5, inner.y + 0.5, inner.w - 1, inner.h - 1);
    cairo_stroke(cr);
}

// ---- ImageWell

void ImageWell::draw(cairo_t *cr)
{
    CoreUI::imageWell(cr, bounds(), controlState(*this));
    if (!image.isNull())
        image.draw(cr, Rect{imageInsets.x, imageInsets.y, frame().w - imageInsets.w, frame().h - imageInsets.h});
}

// ---- TextField

namespace {

void magnifier(cairo_t *cr, double x, double y)
{
    // NSSearchFieldCell's search button: the glass and its handle
    cairo_save(cr);
    cairo_set_line_width(cr, 1.6);
    Color::gray(70).set(cr);
    cairo_arc(cr, x + 5, y + 5, 3.8, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, x + 8, y + 8);
    cairo_line_to(cr, x + 11, y + 11);
    cairo_stroke(cr);
    cairo_restore(cr);
}

size_t prevChar(const std::string &s, size_t at)
{
    if (at == 0)
        return 0;
    --at;
    while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80)
        --at;
    return at;
}

size_t nextChar(const std::string &s, size_t at)
{
    if (at >= s.size())
        return s.size();
    ++at;
    while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80)
        ++at;
    return at;
}

} // namespace

Rect TextField::textRect() const
{
    // the search field: after the magnifier, before the cancel button; a square bezel: 3 points in
    if (style == Search)
        return {22, 0, frame().w - 22 - 20, frame().h - 1};
    if (style == Bezel)
        return {4, 0, frame().w - 8, frame().h};
    return {2, 0, frame().w - 4, frame().h};
}

std::string TextField::shown() const
{
    if (!secure)
        return text;
    std::string dots;
    for (size_t at = 0; at < text.size(); at = nextChar(text, at))
        dots += "•";
    return dots;
}

void TextField::setText(const std::string &t)
{
    text = t;
    m_caret = std::min(m_caret, text.size());
    setNeedsDisplay();
}

void TextField::draw(cairo_t *cr)
{
    CoreUI::State s = controlState(*this);
    if (style == Search)
        CoreUI::searchField(cr, bounds(), s, inToolbar);
    else if (style == Bezel)
        CoreUI::textField(cr, bounds(), s);
    const double mid = std::floor((frame().h - 1) / 2);
    if (style == Search)
        magnifier(cr, 7, mid - 6);
    const Rect r = textRect();
    TextStyle t;
    t.font = font;
    t.elide = false;
    t.align = align;
    t.color = textColor(isEnabled());
    cairo_save(cr);
    cairo_rectangle(cr, r.x, 0, r.w, frame().h);
    cairo_clip(cr);
    const std::string str = shown();
    if (str.empty() && !placeholder.empty() && !isFirstResponder()) {
        TextStyle p = t;
        p.color = Color::gray(0, 0.4);
        drawText(cr, placeholder, r, p);
    }
    drawText(cr, str, r, t);
    if (isFirstResponder() && m_caretOn && windowIsKey()) {
        // the caret after the characters before it (the dots for a secure field)
        std::string before = text.substr(0, m_caret);
        if (secure) {
            before.clear();
            for (size_t at = 0; at < m_caret; at = nextChar(text, at))
                before += "•";
        }
        double x = r.x + textWidth(before, font);
        if (align == Align::Right)
            x += r.w - textWidth(str, font);
        else if (align == Align::Center)
            x += (r.w - textWidth(str, font)) / 2;
        const double h = std::round(font.size + 1);
        fill(cr, Rect{std::round(x), std::round(mid - h / 2 + 1), 1, h}, Color::gray(0));
    }
    cairo_restore(cr);
    if (style == Search && !text.empty()) {
        // the cancel button: a grey disc, a white cross
        const double cx = frame().w - 12, cy = mid + 0.5;
        Color::gray(0, 0.35).set(cr);
        cairo_arc(cr, cx, cy, 6.5, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, cx - 2.5, cy - 2.5);
        cairo_line_to(cr, cx + 2.5, cy + 2.5);
        cairo_move_to(cr, cx + 2.5, cy - 2.5);
        cairo_line_to(cr, cx - 2.5, cy + 2.5);
        cairo_stroke(cr);
    }
}

void TextField::mouseDown(Event &e)
{
    if (style == Search && !text.empty() && e.location.x > frame().w - 20) {
        text.clear();
        m_caret = 0;
        if (onChange)
            onChange(text);
    } else {
        // the caret at the nearest character
        const Rect r = textRect();
        m_caret = 0;
        for (size_t at = 0; at <= text.size(); at = nextChar(text, at)) {
            if (r.x + textWidth(secure ? std::string() : text.substr(0, at), font) > e.location.x)
                break;
            m_caret = at;
            if (at == text.size())
                break;
        }
    }
    m_caretOn = true;
    setNeedsDisplay();
}

void TextField::keyDown(Event &e)
{
    const std::string before = text;
    switch (e.keysym) {
    case XKB_KEY_BackSpace:
        if (m_caret > 0) {
            const size_t p = prevChar(text, m_caret);
            text.erase(p, m_caret - p);
            m_caret = p;
        }
        break;
    case XKB_KEY_Delete:
        if (m_caret < text.size())
            text.erase(m_caret, nextChar(text, m_caret) - m_caret);
        break;
    case XKB_KEY_Left: m_caret = prevChar(text, m_caret); break;
    case XKB_KEY_Right: m_caret = nextChar(text, m_caret); break;
    case XKB_KEY_Home: m_caret = 0; break;
    case XKB_KEY_End: m_caret = text.size(); break;
    case XKB_KEY_Escape:
        if (style != Search) {
            View::keyDown(e);
            return;
        }
        text.clear();
        m_caret = 0;
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (onEnter)
            onEnter();
        else
            View::keyDown(e);
        return;
    case XKB_KEY_Tab:
        View::keyDown(e);
        return;
    default:
        if (e.text.empty()) {
            View::keyDown(e);
            return;
        }
        text.insert(m_caret, e.text);
        m_caret += e.text.size();
    }
    m_caretOn = true;
    setNeedsDisplay();
    if (text != before && onChange)
        onChange(text);
}

TextField::~TextField()
{
    // a field gone with its window while it had the keyboard: its caret's timer goes with it
    if (m_blink)
        App::shared().removeTimer(m_blink);
}

void TextField::focusChanged()
{
    // the caret blinks while the field has the keyboard
    if (m_blink)
        App::shared().removeTimer(m_blink);
    m_blink = 0;
    if (isFirstResponder()) {
        m_caret = text.size();
        m_caretOn = true;
        m_blink = App::shared().addTimer(0.53, true, [this] {
            m_caretOn = !m_caretOn;
            setNeedsDisplay();
        });
    } else if (onEnter && editable) {
        onEnter();   // leaving the field ends its editing, as NSTextField sends its action
    }
    setNeedsDisplay();
}

// ---- Slider

void Slider::setValue(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    if (v != m_value) {
        m_value = v;
        setNeedsDisplay();
    }
}

void Slider::draw(cairo_t *cr)
{
    CoreUI::State s = controlState(*this, m_pressed);
    s.small = small;
    CoreUI::slider(cr, bounds(), m_value, ticks, s, ticksAbove);
}

void Slider::track(Point p)
{
    const CoreUI::SliderParts parts = CoreUI::sliderParts(bounds(), m_value, ticks, ticksAbove, small);
    double v;
    if (parts.vertical) {
        const double range = frame().h - parts.thumb.h;
        v = range > 0 ? 1 - (p.y - m_grab) / range : 0;
    } else {
        const double range = frame().w - parts.thumb.w;
        v = range > 0 ? (p.x - m_grab) / range : 0;
    }
    if (onlyTicks && ticks > 1)
        v = std::round(std::clamp(v, 0.0, 1.0) * (ticks - 1)) / (ticks - 1);
    const double old = m_value;
    setValue(v);
    if (m_value != old && onChange)
        onChange(m_value);
}

void Slider::mouseDown(Event &e)
{
    const CoreUI::SliderParts parts = CoreUI::sliderParts(bounds(), m_value, ticks, ticksAbove, small);
    // on the thumb: it keeps its place under the pointer; elsewhere it jumps there
    if (parts.vertical)
        m_grab = parts.thumb.contains(e.location) ? e.location.y - parts.thumb.y : parts.thumb.h / 2;
    else
        m_grab = parts.thumb.contains(e.location) ? e.location.x - parts.thumb.x : parts.thumb.w / 2;
    m_pressed = true;
    track(e.location);
    setNeedsDisplay();
}

void Slider::mouseDragged(Event &e) { track(e.location); }

void Slider::mouseUp(Event &)
{
    m_pressed = false;
    setNeedsDisplay();
    if (action)
        action();
}

// ---- ScrollView

class Scroller : public View {
public:
    explicit Scroller(ScrollView *sv) : m_sv(sv) {}
    double visible() const { return m_sv->frame().h; }
    double range() const { return std::max(0.0, m_sv->m_height - visible()); }
    double proportion() const { return m_sv->m_height > 0 ? std::min(1.0, visible() / m_sv->m_height) : 1; }
    double value() const { return range() > 0 ? m_sv->m_offset / range() : 0; }
    void draw(cairo_t *cr) override
    {
        CoreUI::scroller(cr, bounds(), value(), proportion(), controlState(*this), m_arrow);
    }
    void mouseDown(Event &e) override
    {
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(bounds(), value(), proportion());
        if (proportion() >= 1)
            return;
        if (p.knob.contains(e.location)) {
            m_dragFrom = e.location.y;
            m_dragOffset = m_sv->m_offset;
            m_dragging = true;
        } else if (p.up.contains(e.location) || p.down.contains(e.location)) {
            m_arrow = p.up.contains(e.location) ? 1 : 2;
            step();
            m_repeat = App::shared().addTimer(0.35, false, [this] {
                m_repeat = App::shared().addTimer(0.05, true, [this] { step(); });
            });
        } else {
            // a page towards the click
            m_sv->scrollTo(m_sv->m_offset + (e.location.y < p.knob.y ? -1 : 1) * (visible() - m_sv->lineStep));
        }
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        if (!m_dragging)
            return;
        const CoreUI::ScrollerParts p = CoreUI::scrollerParts(bounds(), value(), proportion());
        const double travel = p.track.h - p.knob.h;
        if (travel > 0)
            m_sv->scrollTo(m_dragOffset + (e.location.y - m_dragFrom) * range() / travel);
    }
    void mouseUp(Event &) override
    {
        m_dragging = false;
        m_arrow = 0;
        if (m_repeat)
            App::shared().removeTimer(m_repeat);
        m_repeat = 0;
        setNeedsDisplay();
    }
    void step() { m_sv->scrollTo(m_sv->m_offset + (m_arrow == 1 ? -1 : 1) * m_sv->lineStep); }

private:
    ScrollView *m_sv;
    double m_dragFrom = 0, m_dragOffset = 0;
    bool m_dragging = false;
    int m_arrow = 0, m_repeat = 0;
};

ScrollView::ScrollView()
{
    m_document = add<View>();
    m_scroller = add<Scroller>(this);
}

void ScrollView::setFrame(const Rect &r)
{
    View::setFrame(r);
    layout();
}

void ScrollView::layout()
{
    // the scroller only when there is something to scroll (as the lists' of 10.6 panes)
    const bool scrolls = m_height > frame().h;
    const double w = frame().w - (scrolls ? CoreUI::ScrollerWidth : 0);
    m_offset = std::clamp(m_offset, 0.0, std::max(0.0, m_height - frame().h));
    m_document->setFrame({0, -m_offset, w, std::max(m_height, frame().h)});
    m_scroller->setFrame({w, 0, CoreUI::ScrollerWidth, frame().h});
    m_scroller->setHidden(!scrolls);
    for (View *v : m_document->subviews())
        v->setFrame({v->frame().x, v->frame().y, w, v->frame().h});
}

void ScrollView::setDocumentHeight(double h)
{
    m_height = h;
    layout();
}

void ScrollView::scrollTo(double y)
{
    m_offset = y;
    layout();
}

void ScrollView::scrollToVisible(const Rect &r)
{
    if (r.y < m_offset)
        scrollTo(r.y);
    else if (r.bottom() > m_offset + frame().h)
        scrollTo(r.bottom() - frame().h);
}

void ScrollView::scrollWheel(Event &e) { scrollTo(m_offset + e.deltaY * lineStep * 2); }

// ---- OutlineView

std::vector<int> OutlineView::visibleRows() const
{
    std::vector<int> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        bool shown = true;
        for (int p = rows[i].parent; p >= 0; p = rows[size_t(p)].parent)
            if (!rows[size_t(p)].expanded)
                shown = false;
        if (shown)
            out.push_back(int(i));
    }
    return out;
}

void OutlineView::reload()
{
    if (auto *sv = dynamic_cast<ScrollView *>(superview() ? superview()->superview() : nullptr)) {
        sv->setDocumentHeight(double(visibleRows().size()) * rowHeight);
        setFrame(sv->document()->bounds());
    }
    setNeedsDisplay();
}

void OutlineView::select(int index, bool notify)
{
    if (index >= 0 && (size_t(index) >= rows.size() || !rows[size_t(index)].selectable))
        return;
    if (index == m_selected)
        return;
    m_selected = index;
    setNeedsDisplay();
    if (notify && onSelect)
        onSelect(index);
}

void OutlineView::draw(cairo_t *cr)
{
    fill(cr, bounds(), background);
    const std::vector<int> vis = visibleRows();
    const bool focused = isFirstResponder() && windowIsKey();
    for (size_t n = 0; n < vis.size(); ++n) {
        const OutlineRow &r = rows[size_t(vis[n])];
        const double y = double(n) * rowHeight;
        const bool sel = vis[n] == m_selected;
        if (sel)   // alternateSelectedControlColor when it has the keyboard, else secondarySelectedControlColor
            fill(cr, Rect{0, y, frame().w, rowHeight}, focused ? Color::rgb(56, 117, 215) : Color::gray(212));
        const double x = 7 + r.level * indent;
        if (r.group)
            CoreUI::disclosureTriangle(cr, Rect{x - 1, y + std::floor((rowHeight - 13) / 2), 13, 13}, r.expanded, false);
        double textX = x + 13;
        if (!r.icon.isNull()) {
            r.icon.draw(cr, Rect{x + 1, y + std::floor((rowHeight - 16) / 2), 16, 16});
            textX = x + 21;
        }
        TextStyle t;
        if (r.header) {
            t.font = Font::boldSystem(11);
            t.color = Color::rgb(112, 126, 140);
        }
        t.color = sel && focused ? Color::gray(255) : t.color;
        drawText(cr, r.title, Rect{textX, y, frame().w - textX - 4 - trailing, rowHeight}, t);
    }
}

void OutlineView::mouseDown(Event &e)
{
    const std::vector<int> vis = visibleRows();
    const int n = int(e.location.y / rowHeight);
    if (n < 0 || size_t(n) >= vis.size())
        return;
    OutlineRow &r = rows[size_t(vis[size_t(n)])];
    const double x = 7 + r.level * indent;
    if (r.group && (e.location.x < x + 13 || !r.selectable || e.clickCount == 2)) {
        r.expanded = !r.expanded;
        reload();
        return;
    }
    select(vis[size_t(n)], true);
}

void OutlineView::keyDown(Event &e)
{
    const std::vector<int> vis = visibleRows();
    auto at = std::find(vis.begin(), vis.end(), m_selected);
    if (e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_Up) {
        const int step = e.keysym == XKB_KEY_Down ? 1 : -1;
        for (long i = (at == vis.end() ? -1 : at - vis.begin()) + step; i >= 0 && size_t(i) < vis.size(); i += step)
            if (rows[size_t(vis[size_t(i)])].selectable) {
                select(vis[size_t(i)], true);
                if (auto *sv = dynamic_cast<ScrollView *>(superview() ? superview()->superview() : nullptr))
                    sv->scrollToVisible({0, double(i) * rowHeight, frame().w, rowHeight});
                break;
            }
        return;
    }
    View::keyDown(e);
}

// ---- ImageGrid

Rect ImageGrid::cellRect(int i) const
{
    return {origin.x + (i % columns) * cellSize.w, origin.y + (i / columns) * cellSize.h, cellSize.w, cellSize.h};
}

double ImageGrid::contentHeight() const
{
    const int rowsCount = (int(cells.size()) + columns - 1) / columns;
    return origin.y * 2 + rowsCount * cellSize.h;
}

void ImageGrid::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(255));
    for (size_t i = 0; i < cells.size(); ++i) {
        const Rect c = cellRect(int(i));
        const Rect thumb{c.x + std::floor((c.w - thumbSize.w) / 2), c.y + std::floor((c.h - thumbSize.h) / 2), thumbSize.w, thumbSize.h};
        if (int(i) == selected) {
            // IKImageBrowserView's selection: a rounded grey ground under the picture
            const Rect s = thumb.inset(-3, -3);
            const double rad = 4;
            cairo_new_sub_path(cr);
            cairo_arc(cr, s.right() - rad, s.y + rad, rad, -M_PI / 2, 0);
            cairo_arc(cr, s.right() - rad, s.bottom() - rad, rad, 0, M_PI / 2);
            cairo_arc(cr, s.x + rad, s.bottom() - rad, rad, M_PI / 2, M_PI);
            cairo_arc(cr, s.x + rad, s.y + rad, rad, M_PI, 3 * M_PI / 2);
            cairo_close_path(cr);
            Color::gray(0, windowIsKey() ? 0.22 : 0.14).set(cr);
            cairo_fill(cr);
        }
        const Image &img = cells[i].image;
        if (img.isNull()) {
            fill(cr, thumb, Color::gray(0, 0.06));
            continue;
        }
        // the picture covering the cell's thumbnail, its middle kept
        const Size full = scaledToFill(img.size(), thumbSize);
        const double kx = img.width() / full.w, ky = img.height() / full.h;
        const Rect src{(full.w - thumbSize.w) / 2 * kx, (full.h - thumbSize.h) / 2 * ky, thumbSize.w * kx, thumbSize.h * ky};
        img.draw(cr, src, thumb);
    }
}

void ImageGrid::mouseDown(Event &e)
{
    for (size_t i = 0; i < cells.size(); ++i)
        if (cellRect(int(i)).contains(e.location)) {
            selected = int(i);
            setNeedsDisplay();
            if (onSelect)
                onSelect(int(i));
            return;
        }
}

// ---- Box

void Box::draw(cairo_t *cr)
{
    // NSBox: the title (11 points) over the box, the box from half its height
    const double titleH = title.empty() ? 0 : 15;
    if (kind == Separator) {
        const bool across = frame().w >= frame().h;
        fill(cr, across ? Rect{0, std::floor(frame().h / 2) - 1, frame().w, 1} : Rect{std::floor(frame().w / 2) - 1, 0, 1, frame().h},
             Color::gray(0, 0.2));
        fill(cr, across ? Rect{0, std::floor(frame().h / 2), frame().w, 1} : Rect{std::floor(frame().w / 2), 0, 1, frame().h},
             Color::gray(255, 0.6));
        return;
    }
    if (kind == Primary || kind == Secondary)
        CoreUI::groupBox(cr, Rect{0, titleH, frame().w, frame().h - titleH}, controlState(*this), kind == Secondary);
    if (!title.empty()) {
        TextStyle t;
        t.font = titleFont;
        drawText(cr, title, Rect{4, 0, frame().w - 8, titleH}, t);
    }
}

View *Box::hitTest(Point p)
{
    View *v = View::hitTest(p);
    return v == this ? nullptr : v;   // the box itself takes no clicks
}

// ---- ImageView

void ImageView::draw(cairo_t *cr)
{
    Rect r = bounds();
    if (frameStyle == Button) {
        CoreUI::imageWell(cr, r, controlState(*this));
        r = Rect{6, 5, r.w - 12, r.h - 13};
    } else if (frameStyle == GrayBezel || frameStyle == Groove) {
        fill(cr, r, Color::gray(0, 0.25));
        fill(cr, r.inset(1, 1), Color::gray(255, 0.5));
        r = r.inset(3, 3);
    } else if (frameStyle == Photo) {
        fill(cr, r, Color::gray(0, 0.35));
        fill(cr, r.inset(1, 1), Color::gray(255));
        r = r.inset(2, 2);
    }
    if (image.isNull())
        return;
    // NSImageScaleProportionallyDown, centred
    Size s = image.size();
    if (s.w > r.w || s.h > r.h)
        s = scaledToFit(s, {r.w, r.h});
    image.draw(cr, Rect{r.x + std::floor((r.w - s.w) / 2), r.y + std::floor((r.h - s.h) / 2), s.w, s.h}, isEnabled() ? 1 : 0.5);
}

// ---- TabView

TabView::TabView() { clipsToBounds = false; }

void TabView::addTab(const std::string &label, View *page)
{
    m_labels.push_back(label);
    m_pages.push_back(page);
    addSubview(page);
    layout();
}

void TabView::setTabLabel(int i, const std::string &label)
{
    if (i < 0 || size_t(i) >= m_labels.size())
        return;
    m_labels[size_t(i)] = label;
    layout();
}

void TabView::select(int i)
{
    if (i < 0 || size_t(i) >= m_pages.size())
        return;
    m_selected = i;
    for (size_t n = 0; n < m_pages.size(); ++n)
        m_pages[n]->setHidden(int(n) != i);
    if (m_tabs) {
        const auto at = std::find(m_shown.begin(), m_shown.end(), i);
        m_tabs->setSelected(at == m_shown.end() ? -1 : int(at - m_shown.begin()));
    }
    setNeedsDisplay();
}

void TabView::setFrame(const Rect &r)
{
    View::setFrame(r);
    layout();
}

void TabView::layout()
{
    // NSTabView: the tabs' middle on the frame's top + 12 (the pane's edge), the pages inside the pane
    if (m_tabs)
        m_tabs->setHidden(noTabs);
    if (!noTabs) {
        // the tabs with a label (a nib's "Tab" or unnamed items are the code's hidden pages)
        m_shown.clear();
        std::vector<std::string> labels;
        std::vector<double> widths;
        double total = 0;
        for (size_t i = 0; i < m_labels.size(); ++i) {
            if (m_labels[i].empty() || m_labels[i] == "Tab")
                continue;
            m_shown.push_back(int(i));
            labels.push_back(m_labels[i]);
            widths.push_back(std::round(textWidth(m_labels[i], Font::system()) + 26));
            total += widths.back();
        }
        if (!m_tabs)
            m_tabs = add<SegmentedControl>(CoreUI::SegmentStyle::Tabs, labels, widths);
        else
            m_tabs->setSegments(labels, widths);
        m_tabs->setFrame({std::floor((frame().w - total) / 2), 0, total, 24});
        m_tabs->onSelect = [this](int i) {
            const int page = i >= 0 && size_t(i) < m_shown.size() ? m_shown[size_t(i)] : i;
            select(page);
            if (onSelect)
                onSelect(page);
        };
    }
    // the pages: NSTabView's content rect, inside the pane's edges (top 12 + 24 of tabs)
    const Rect content = noTabs ? (noBorder ? bounds() : Rect{2, 2, frame().w - 4, frame().h - 4}) : Rect{7, 36, frame().w - 14, frame().h - 46};
    if (!keepPageFrames)
        for (View *p : m_pages)
            p->setFrame(content);
    select(m_selected);
}

void TabView::draw(cairo_t *cr)
{
    if (noTabs && noBorder)
        return;
    CoreUI::tabPane(cr, noTabs ? bounds() : Rect{0, 12, frame().w, frame().h - 12}, controlState(*this));
}

// ---- TableView

namespace {

struct TableBody : View {
    TableView *table;
    explicit TableBody(TableView *t) : table(t) {}
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(255));
        const bool focused = table->isFirstResponder() && windowIsKey();
        for (size_t r = 0; r < table->rows.size(); ++r) {
            const double y = double(r) * table->rowHeight;
            if (int(r) == table->selected)
                fill(cr, Rect{0, y, frame().w, table->rowHeight}, focused ? Color::rgb(56, 117, 215) : Color::gray(212));
            double x = 0;
            std::vector<TableView::Column> columns = table->columns;
            if (columns.empty()) {
                columns.emplace_back();
                columns.back().width = frame().w;
            }
            for (size_t c = 0; c < columns.size(); ++c) {
                const auto &col = columns[c];
                const std::string cell = c < table->rows[r].size() ? table->rows[r][c] : std::string();
                if (col.checkBox) {
                    CoreUI::State s;
                    s.windowActive = windowIsKey();
                    s.small = true;
                    CoreUI::checkBox(cr, Rect{x + std::floor((col.width - 14) / 2), y + std::floor((table->rowHeight - 16) / 2), 14, 16}, s,
                                     cell == "1" ? 1 : 0);
                } else {
                    TextStyle t;
                    t.font = Font::system(table->rowHeight < 17 ? 11 : 13);
                    t.color = int(r) == table->selected && focused ? Color::gray(255) : Color::gray(0);
                    drawText(cr, cell, Rect{x + 4, y, col.width - 6, table->rowHeight}, t);
                }
                x += col.width + 3;
            }
        }
    }
    void mouseDown(Event &e) override
    {
        const int r = int(e.location.y / table->rowHeight);
        if (r < 0 || size_t(r) >= table->rows.size())
            return;
        double x = 0;
        for (size_t c = 0; c < table->columns.size(); ++c) {
            if (table->columns[c].checkBox && e.location.x >= x && e.location.x < x + table->columns[c].width) {
                auto &cell = table->rows[size_t(r)];
                cell.resize(std::max(cell.size(), c + 1));
                const bool on = cell[c] != "1";
                cell[c] = on ? "1" : "0";
                if (table->onCheck)
                    table->onCheck(r, int(c), on);
            }
            x += table->columns[c].width + 3;
        }
        table->selected = r;
        table->setNeedsDisplay();
        window()->makeFirstResponder(table);
        if (table->onSelect)
            table->onSelect(r);
    }
};

} // namespace

TableView::TableView()
{
    m_scroll = add<ScrollView>();
    m_body = m_scroll->document()->add<TableBody>(this);
}

void TableView::setFrame(const Rect &r)
{
    View::setFrame(r);
    reload();
}

void TableView::reload()
{
    const double top = header && !columns.empty() ? 17 : 0, edge = bordered ? 1 : 0;
    m_scroll->setFrame({edge, edge + top, frame().w - 2 * edge, frame().h - 2 * edge - top});
    m_scroll->setDocumentHeight(double(rows.size()) * rowHeight);
    m_body->setFrame(m_scroll->document()->bounds());
    setNeedsDisplay();
}

void TableView::draw(cairo_t *cr)
{
    if (bordered) {
        fill(cr, bounds(), Color::gray(0, 0.35));
        fill(cr, bounds().inset(1, 1), Color::gray(255));
    }
    if (header && !columns.empty()) {
        const double edge = bordered ? 1 : 0;
        double x = edge;
        for (size_t c = 0; c < columns.size(); ++c) {
            const double w = c + 1 == columns.size() ? frame().w - edge - x : columns[c].width + 3;
            CoreUI::listHeader(cr, Rect{x, edge, w, 17}, controlState(*this));
            TextStyle t;
            t.font = Font::system(11);
            drawText(cr, columns[c].title, Rect{x + 4, edge, w - 8, 16}, t);
            x += w;
        }
    }
}

// ---- ProgressIndicator

ProgressIndicator::~ProgressIndicator() { stop(); }

void ProgressIndicator::start()
{
    if (m_timer)
        return;
    animating = true;
    m_timer = App::shared().addTimer(spinning ? 1.0 / 12 : 1.0 / 30, true, [this] {
        m_phase += 1;
        setNeedsDisplay();
    });
}

void ProgressIndicator::stop()
{
    if (m_timer)
        App::shared().removeTimer(m_timer);
    m_timer = 0;
    animating = false;
    setNeedsDisplay();
}

void ProgressIndicator::draw(cairo_t *cr)
{
    if (!spinning) {
        const double v = maxValue > minValue ? (value - minValue) / (maxValue - minValue) : 0;
        CoreUI::progressBar(cr, bounds(), v, indeterminate, m_phase * 2, controlState(*this));
        return;
    }
    if (!animating)
        return;   // NSProgressIndicator hides its wheel when stopped
    // the twelve spokes, the darkest turning
    const double cx = frame().w / 2, cy = frame().h / 2, r = std::min(cx, cy);
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, std::max(1.5, r / 5));
    for (int i = 0; i < 12; ++i) {
        const double a = (i - m_phase) * M_PI / 6;
        Color::gray(0, 0.15 + 0.7 * ((12 - i) % 12) / 12.0).set(cr);
        cairo_move_to(cr, cx + std::cos(a) * r * 0.45, cy + std::sin(a) * r * 0.45);
        cairo_line_to(cr, cx + std::cos(a) * r * 0.9, cy + std::sin(a) * r * 0.9);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

// ---- Stepper

void Stepper::draw(cairo_t *cr) { CoreUI::stepper(cr, bounds(), controlState(*this), m_pressedPart); }

void Stepper::mouseDown(Event &e)
{
    m_pressedPart = e.location.y < frame().h / 2 ? 1 : 2;
    const double old = value;
    value = std::clamp(value + (m_pressedPart == 1 ? increment : -increment), minValue, maxValue);
    if (value != old && onChange)
        onChange(value);
    setNeedsDisplay();
}

void Stepper::mouseUp(Event &)
{
    m_pressedPart = 0;
    setNeedsDisplay();
    if (action)
        action();
}

// ---- ComboBox

void ComboBox::draw(cairo_t *cr)
{
    CoreUI::comboBox(cr, bounds(), controlState(*this));
    TextStyle t;
    t.font = font;
    t.color = textColor(isEnabled());
    drawText(cr, text, Rect{4, 0, frame().w - 26, std::min(frame().h, 22.0)}, t);
}

void ComboBox::mouseDown(Event &e)
{
    if (e.location.x < frame().w - 20 || items.empty() || !window())
        return TextField::mouseDown(e);
    std::vector<MenuItem> list;
    for (const std::string &i : items)
        list.push_back({i, false, true, i == text});
    Menu::popUp(window(), toWindow({0, frame().h}), list, frame().w, font, [this](int chosen) {
        if (chosen < 0)
            return;
        setText(items[size_t(chosen)]);
        if (onChange)
            onChange(text);
        if (onEnter)
            onEnter();
    });
}

// ---- LevelIndicator

void LevelIndicator::draw(cairo_t *cr)
{
    // discrete: one cell a unit, green filled up to the value
    const int n = std::max(1, int(maxValue));
    const double w = frame().w / n;
    for (int i = 0; i < n; ++i) {
        const Rect c{std::floor(i * w) + 1, 1, std::floor(w) - 2, frame().h - 2};
        fill(cr, c, Color::gray(0, 0.3));
        fill(cr, c.inset(1, 1), i < value ? Color::rgb(105, 190, 70) : Color::gray(250));
    }
}

// ---- RadioGroup

int RadioGroup::selected() const
{
    for (size_t i = 0; i < buttons.size(); ++i)
        if (buttons[i]->checked())
            return int(i);
    return -1;
}

void RadioGroup::select(int i)
{
    for (size_t n = 0; n < buttons.size(); ++n)
        buttons[n]->setChecked(int(n) == i);
}

View *RadioGroup::hitTest(Point p)
{
    View *v = View::hitTest(p);
    return v == this ? nullptr : v;
}

// ---- CalendarView, ClockView

void CalendarView::draw(cairo_t *cr)
{
    // the month's name, the days' initials, six weeks; today on a blue square
    const std::time_t now = std::time(nullptr);
    std::tm t = *std::localtime(&now);
    char title[64];
    std::strftime(title, sizeof title, "%B %Y", &t);
    fill(cr, bounds(), Color::gray(255));
    Color::gray(0, 0.3).set(cr);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, frame().w - 1, frame().h - 1);
    cairo_stroke(cr);
    TextStyle head;
    head.align = Align::Center;
    head.font = Font::system(11);
    drawText(cr, title, Rect{0, 2, frame().w, 16}, head);
    const double cw = (frame().w - 8) / 7, top = 34, ch = (frame().h - top - 4) / 6;
    const char *days[] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int d = 0; d < 7; ++d) {
        TextStyle s = head;
        s.font = Font::system(10);
        s.color = Color::gray(90);
        drawText(cr, days[d], Rect{4 + d * cw, 18, cw, 14}, s);
    }
    std::tm first = t;
    first.tm_mday = 1;
    std::mktime(&first);
    std::tm next = first;
    next.tm_mon += 1;
    const std::time_t a = std::mktime(&first), b = std::mktime(&next);
    const int length = int(std::lround(std::difftime(b, a) / 86400));
    for (int day = 1; day <= length; ++day) {
        const int cell = first.tm_wday + day - 1;
        const Rect r{4 + (cell % 7) * cw, top + (cell / 7) * ch, cw, ch};
        TextStyle s = head;
        if (day == t.tm_mday) {
            fill(cr, r.inset(1, 1), Color::rgb(56, 117, 215));
            s.color = Color::gray(255);
        }
        drawText(cr, std::to_string(day), r, s);
    }
}

ClockView::ClockView()
{
    m_timer = App::shared().addTimer(1, true, [this] { setNeedsDisplay(); });
}

ClockView::~ClockView() { App::shared().removeTimer(m_timer); }

void ClockView::draw(cairo_t *cr)
{
    // a white face in a grey ring, the hours' marks, the hands, AM or PM
    const std::time_t now = std::time(nullptr);
    const std::tm t = *std::localtime(&now);
    const double cx = frame().w / 2, cy = frame().h / 2, r = std::min(cx, cy) - 3;
    cairo_save(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    Color::gray(170).set(cr);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, r - 3, 0, 2 * M_PI);
    Color::gray(255).set(cr);
    cairo_fill(cr);
    for (int i = 0; i < 12; ++i) {
        const double a = i * M_PI / 6;
        Color::gray(60).set(cr);
        cairo_set_line_width(cr, i % 3 ? 1 : 2.5);
        cairo_move_to(cr, cx + std::sin(a) * (r - 6), cy - std::cos(a) * (r - 6));
        cairo_line_to(cr, cx + std::sin(a) * (r - (i % 3 ? 10 : 13)), cy - std::cos(a) * (r - (i % 3 ? 10 : 13)));
        cairo_stroke(cr);
    }
    auto hand = [&](double turn, double length, double width, const Color &c) {
        c.set(cr);
        cairo_set_line_width(cr, width);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, cx + std::sin(turn * 2 * M_PI) * length, cy - std::cos(turn * 2 * M_PI) * length);
        cairo_stroke(cr);
    };
    hand((t.tm_hour % 12 + t.tm_min / 60.0) / 12, r * 0.5, 3.5, Color::gray(20));
    hand((t.tm_min + t.tm_sec / 60.0) / 60, r * 0.75, 2.5, Color::gray(20));
    hand(t.tm_sec / 60.0, r * 0.8, 1, Color::rgb(200, 30, 30));
    cairo_restore(cr);
    TextStyle s;
    s.align = Align::Center;
    s.font = Font::system(10);
    s.color = Color::gray(110);
    drawText(cr, t.tm_hour < 12 ? "AM" : "PM", Rect{0, cy + r * 0.3, frame().w, 14}, s);
}

// ---- Sheet

namespace {

// the panel's own ground: the window's grey
struct SheetPanel : View {
    void draw(cairo_t *cr) override { fill(cr, bounds(), Color::gray(237)); }
};

// the area under the toolbar the sheet slides into: its shadow drawn around it
struct SheetClip : View {
    View *panel = nullptr;
    void draw(cairo_t *cr) override
    {
        if (!panel)
            return;
        const Rect p = panel->frame();
        // a soft shadow at the sides and under it, and the toolbar's shadow over its top
        for (int i = 1; i <= 10; ++i) {
            const double a = 0.05 * (1 - (i - 1) / 10.0);
            Color::gray(0, a).set(cr);
            cairo_rectangle(cr, p.x - i, p.y, 1, p.h + i);
            cairo_rectangle(cr, p.right() + i - 1, p.y, 1, p.h + i);
            cairo_rectangle(cr, p.x - i, p.bottom() + i - 1, p.w + 2 * i, 1);
            cairo_fill(cr);
        }
    }
};

struct SheetHost : View {
    SheetClip *clip = nullptr;
    SheetPanel *panel = nullptr;
    std::function<void()> ended;
    View *previous = nullptr;   // the window's first responder before the sheet
    Size panelSize, oldWindow;
    double top = 0, progress = 0;
    int timer = 0;
    bool closing = false;
    ~SheetHost() override
    {
        if (timer)
            App::shared().removeTimer(timer);
    }
    void layoutPanel()
    {
        // eased: fast then slow, as AppKit's sheets
        const double t = 1 - std::pow(1 - progress, 3);
        panel->setFrame({std::floor((frame().w - panelSize.w) / 2), std::round(-panelSize.h + t * panelSize.h), panelSize.w, panelSize.h});
        clip->setNeedsDisplay();
    }
    View *hitTest(Point p) override
    {
        // the window answers the sheet only: everything else is taken here
        View *v = View::hitTest(p);
        return v == clip ? this : v;
    }
    void keyDown(Event &e) override
    {
        if (e.keysym == XKB_KEY_Escape) {
            Sheet::end(window());
        } else if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter) {
            // the default button
            std::function<void(View *)> find = [&](View *v) {
                if (auto *b = dynamic_cast<Button *>(v))
                    if (b->isDefault && b->action) {
                        b->action();
                        return;
                    }
                for (View *c : v->subviews())
                    find(c);
            };
            find(panel);
        }
    }
};

std::map<Window *, SheetHost *> &sheets()
{
    static std::map<Window *, SheetHost *> map;
    return map;
}

View *firstField(View *v)
{
    if (v->acceptsFirstResponder() && !v->isHidden())
        return v;
    for (View *c : v->subviews())
        if (View *f = firstField(c))
            return f;
    return nullptr;
}

} // namespace

void Sheet::begin(Window *w, View *content, Size size, double top, std::function<void()> ended)
{
    if (!w || isOpen(w)) {
        delete content;
        return;
    }
    View *root = w->contentView();
    auto *host = root->add<SheetHost>();
    host->top = top;
    host->panelSize = size;
    host->ended = std::move(ended);
    host->oldWindow = w->size();
    // a sheet taller than the window: the window grows for it
    const double need = top + size.h + 12;
    if (need > w->size().h)
        w->setContentSize({std::max(w->size().w, size.w), need});
    host->setFrame(root->bounds());
    host->clip = host->add<SheetClip>();
    host->clip->setFrame({0, top, root->frame().w, root->frame().h - top});
    host->panel = host->clip->add<SheetPanel>();
    host->clip->panel = host->panel;
    content->setFrame({0, 0, size.w, size.h});
    host->panel->addSubview(content);
    host->layoutPanel();
    sheets()[w] = host;
    host->previous = w->firstResponder();
    w->makeFirstResponder(firstField(content) ? firstField(content) : host);
    host->timer = App::shared().addTimer(1.0 / 60, true, [host] {
        host->progress = std::min(1.0, host->progress + 1.0 / 12);   // 0.2 s
        host->layoutPanel();
        if (host->progress >= 1) {
            App::shared().removeTimer(host->timer);
            host->timer = 0;
        }
    });
}

void Sheet::end(Window *w)
{
    auto it = sheets().find(w);
    if (it == sheets().end() || it->second->closing)
        return;
    SheetHost *host = it->second;
    host->closing = true;
    if (host->timer)
        App::shared().removeTimer(host->timer);
    // back up under the toolbar, then gone
    host->timer = App::shared().addTimer(1.0 / 60, true, [w, host] {
        host->progress = std::max(0.0, host->progress - 1.0 / 9);
        host->layoutPanel();
        if (host->progress > 0)
            return;
        App::shared().removeTimer(host->timer);
        host->timer = 0;
        auto ended = host->ended;
        const Size old = host->oldWindow;
        View *previous = host->previous;
        sheets().erase(w);
        // the host goes after this timer's call
        App::shared().later([w, host, ended, old, previous] {
            w->makeFirstResponder(previous);
            w->contentView()->removeSubview(host);
            w->setContentSize(old);
            if (ended)
                ended();
        });
    });
}

bool Sheet::isOpen(Window *w) { return sheets().count(w) != 0; }

// ---- LockView

void LockView::draw(cairo_t *cr)
{
    // SFAuthorizationView: the padlock (open), its sentence beside it
    const double s = std::min(frame().h, 32.0), x = 2, y = std::floor((frame().h - s) / 2);
    cairo_save(cr);
    cairo_set_line_width(cr, s / 9);
    Color::gray(110).set(cr);
    cairo_arc(cr, x + s * 0.62, y + s * 0.38, s * 0.2, M_PI, 2 * M_PI);   // the shackle, open to the right
    cairo_stroke(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y + s * 0.42, 0, y + s * 0.95);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.93, 0.80, 0.42);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.72, 0.55, 0.18);
    cairo_rectangle(cr, x + s * 0.12, y + s * 0.45, s * 0.5, s * 0.45);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
    TextStyle t;
    t.font = Font::system(11);
    drawText(cr, "Click the lock to prevent further changes.", Rect{s + 6, 0, frame().w - s - 6, frame().h}, t);
}

} // namespace AK
