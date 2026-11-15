// SPDX-License-Identifier: LGPL-2.1-or-later
#include "nib.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace AK {

Rect parseRect(const std::string &s)
{
    std::vector<double> n;
    std::string num;
    for (char c : s + ' ') {
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e')
            num += c;
        else if (!num.empty()) {
            n.push_back(std::atof(num.c_str()));
            num.clear();
        }
    }
    if (n.size() >= 4)
        return {n[0], n[1], n[2], n[3]};
    if (n.size() == 2)
        return {0, 0, n[0], n[1]};
    return {};
}

namespace {

// a nib's plain view (NSView, a custom view whose code isn't here): it holds others, and lets the
// clicks through where it has none (it would otherwise hide the controls of the views under it)
struct Container : View {
    Container() { clipsToBounds = false; }
    View *hitTest(Point p) override
    {
        View *v = View::hitTest(p);
        return v == this ? nullptr : v;
    }
};

uint32_t u32(long long v) { return uint32_t(v & 0xFFFFFFFF); }

// the texts a nib holds for its code to replace: "<< do not localize >>", "<do not loc>", "<<<<<"
bool placeholder(const std::string &text)
{
    std::string t;
    for (char c : text)
        t += char(std::tolower(static_cast<unsigned char>(c)));
    if (t.find("do not loc") != std::string::npos || t.find("no loc") != std::string::npos)
        return true;
    if (t.size() >= 4 && t.find_first_not_of("<> ") == std::string::npos)
        return true;
    return false;
}

} // namespace

bool Nib::load(const std::string &file, const std::string &resources)
{
    m_resources = resources;
    if (!m_archive.load(file)) {
        // a nib of the older format: objects.nib, NSArchiver's typedstream
        const size_t at = file.rfind("keyedobjects.nib");
        if (at == std::string::npos || !m_archive.loadTypedStream(file.substr(0, at) + "objects.nib"))
            return false;
    }
    const int data = m_archive.top("IB.objectdata");
    m_owner = m_archive.object(data, "NSRoot");
    for (int c : m_archive.array(data, "NSConnections")) {
        const std::string cls = m_archive.className(c);
        const int source = m_archive.object(c, "NSSource"), dest = m_archive.object(c, "NSDestination");
        const std::string label = m_archive.string(c, "NSLabel");
        if (cls == "NSNibOutletConnector" && source == m_owner)
            m_ownerOutlets[label] = dest;
        else if (cls == "NSNibControlConnector")
            m_actions[source] = label;
        else if (cls == "NSNibBindingConnector")
            m_bindings.push_back({source, m_archive.string(c, "NSBinding"), m_archive.string(c, "NSKeyPath")});
    }
    return true;
}

std::vector<View *> Nib::bound(const std::string &keyPath, const std::string &binding) const
{
    // the views built so far whose binding's key path ends with keyPath (the controller's own path
    // before it: "viewOptionsSettingsController.targetedViewOptionsSettings.iconViewTextSize")
    std::vector<View *> out;
    for (const Binding &b : m_bindings) {
        if (b.binding != binding || b.keyPath.size() < keyPath.size() ||
            b.keyPath.compare(b.keyPath.size() - keyPath.size(), keyPath.size(), keyPath) != 0)
            continue;
        if (b.keyPath.size() > keyPath.size() && b.keyPath[b.keyPath.size() - keyPath.size() - 1] != '.')
            continue;
        auto v = m_views.find(b.source);
        if (v != m_views.end() && std::find(out.begin(), out.end(), v->second) == out.end())
            out.push_back(v->second);
    }
    return out;
}

std::string Nib::classOf(int obj) const
{
    const std::string cls = m_archive.className(obj);
    if (cls == "NSClassSwapper") {
        const std::string original = m_archive.string(obj, "NSOriginalClassName");
        return original.empty() ? "NSView" : original;
    }
    return cls;
}

std::string Nib::customClassOf(int obj) const
{
    const std::string cls = m_archive.className(obj);
    if (cls == "NSClassSwapper" || cls == "NSCustomView")
        return m_archive.string(obj, "NSClassName");
    return cls;
}

Rect Nib::frameOf(int obj, double parentHeight, bool parentFlipped) const
{
    Rect r;
    const std::string f = m_archive.string(obj, "NSFrame");
    if (!f.empty())
        r = parseRect(f);
    else
        r = parseRect(m_archive.string(obj, "NSFrameSize"));
    if (!parentFlipped)
        r.y = parentHeight - r.y - r.h;
    return r;
}

Font Nib::fontOf(int cell) const
{
    Font f = Font::system();
    const int font = m_archive.object(cell, "NSSupport");
    if (font < 0 || m_archive.className(font) != "NSFont")
        return f;
    const std::string name = m_archive.string(font, "NSName");
    f.size = m_archive.real(font, "NSSize", 13);
    f.bold = name.find("Bold") != std::string::npos;
    if (name.rfind("LucidaGrande", 0) != 0 && name.rfind(".Lucida", 0) != 0 && !name.empty()) {
        // another face: its family, as fontconfig knows it ("Helvetica-Bold" -> "Helvetica")
        f.family = name.substr(0, name.find('-'));
    }
    return f;
}

Color Nib::colorOf(int c, const Color &fallback) const
{
    if (c < 0)
        return fallback;
    const std::string name = m_archive.string(c, "NSColorName");
    if (!name.empty()) {
        if (name == "disabledControlTextColor")
            return Color::gray(0, 0.4);
        if (name == "controlShadowColor")
            return Color::gray(128);
        if (name == "textBackgroundColor" || name == "controlBackgroundColor" || name == "selectedTextColor")
            return Color::gray(255);
        if (name == "controlColor" || name == "windowBackgroundColor")
            return Color{0, 0, 0, 0};
        if (name.find("TextColor") != std::string::npos || name == "labelColor")
            return Color::gray(0);
        const int inner = m_archive.object(c, "NSColor");
        return inner >= 0 ? colorOf(inner, fallback) : fallback;
    }
    std::istringstream in(m_archive.data(c, "NSRGB").empty() ? m_archive.data(c, "NSWhite") : m_archive.data(c, "NSRGB"));
    std::vector<double> v;
    double x;
    while (in >> x)
        v.push_back(x);
    if (!m_archive.data(c, "NSRGB").empty() && v.size() >= 3)
        return {v[0], v[1], v[2], v.size() > 3 ? v[3] : 1};
    if (v.size() >= 1)
        return {v[0], v[0], v[0], v.size() > 1 ? v[1] : 1};
    return fallback;
}

Image Nib::image(const std::string &name) const
{
    // the bundle's pictures, then System Preferences' and CoreTypes' for the system's names
    static const std::map<std::string, std::string> system = {
        {"NSUser", "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/UserIcon.icns"},
        {"NSUserGroup", "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GroupIcon.icns"},
        {"NSEveryone", "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/Everyone.icns"},
        {"NSNetwork", "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericNetworkIcon.icns"},
        {"NSComputer", "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericComputerIcon.icns"},
        {"NSFolder", "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericFolderIcon.icns"},
        {"NSCaution", "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/AlertCautionIcon.icns"},
        {"NSApplicationIcon", "/Applications/System Preferences.app/Contents/Resources/PrefApp.icns"},
    };
    if (name.empty())
        return {};
    auto s = system.find(name);
    if (s != system.end())
        return Image::load(Assets::path(s->second), {32, 32});
    std::string base = name;
    for (const char *ext : {".tiff", ".tif", ".png", ".icns"})
        if (base.size() > std::string(ext).size() && base.compare(base.size() - std::string(ext).size(), std::string::npos, ext) == 0)
            base = base.substr(0, base.size() - std::string(ext).size());
    for (const std::string &dir : {m_resources, Assets::path("/Applications/System Preferences.app/Contents/Resources")})
        for (const char *ext : {".tiff", ".tif", ".png", ".icns"}) {
            Image img = Image::load(dir + '/' + base + ext, {32, 32});
            if (!img.isNull())
                return img;
        }
    return {};
}

View *Nib::instantiate(const std::string &outlet)
{
    m_views.clear();
    m_controls.clear();
    int top = -1;
    auto it = m_ownerOutlets.find(outlet);
    if (it != m_ownerOutlets.end())
        top = it->second;
    if (top < 0) {
        // the first window of the nib's top level objects
        const int data = m_archive.top("IB.objectdata");
        for (int o : m_archive.array(data, "NSObjectsKeys"))
            if (m_archive.className(o) == "NSWindowTemplate") {
                top = o;
                break;
            }
    }
    if (top < 0)
        return nullptr;
    return instantiateWindow(top, &m_size);
}

View *Nib::instantiateWindow(int top, Size *size)
{
    int content = top;
    Size s;
    if (m_archive.className(top) == "NSWindowTemplate") {
        content = m_archive.object(top, "NSWindowView");
        const Rect r = parseRect(m_archive.string(top, "NSWindowRect"));
        s = Size{r.w, r.h};
    }
    const Rect cf = frameOf(content, 0, true);
    if (cf.w > 0 && cf.h > 0)
        s = {cf.w, cf.h};
    if (size)
        *size = s;
    auto *root = new Container;
    root->setFrame({0, 0, s.w, s.h});
    buildChildren(content, root, s.h, false);
    return root;
}

void Nib::collectViews(int obj, std::vector<int> &out) const
{
    // a view and everything archived under it: subviews, cells of matrices, tab items' views, documents
    if (obj < 0 || std::find(out.begin(), out.end(), obj) != out.end())
        return;
    out.push_back(obj);
    for (int v : m_archive.array(obj, "NSSubviews"))
        collectViews(v, out);
    for (int c : m_archive.array(obj, "NSCells"))
        collectViews(c, out);
    for (int item : m_archive.array(obj, "NSTabViewItems"))
        collectViews(m_archive.object(item, "NSView"), out);
    collectViews(m_archive.object(obj, "NSContentView"), out);
    collectViews(m_archive.object(obj, "NSDocView"), out);
}

std::vector<Nib::WindowEntry> Nib::windows() const
{
    std::vector<WindowEntry> out;
    const int data = m_archive.top("IB.objectdata");
    for (int o : m_archive.array(data, "NSObjectsKeys")) {
        if (m_archive.className(o) != "NSWindowTemplate")
            continue;
        WindowEntry w;
        w.object = o;
        w.title = m_archive.string(o, "NSWindowTitle");
        const Rect r = parseRect(m_archive.string(o, "NSWindowRect"));
        w.size = {r.w, r.h};
        for (const auto &[name, target] : m_ownerOutlets)
            if (target == o)
                w.outlet = name;
        std::vector<int> views;
        collectViews(m_archive.object(o, "NSWindowView"), views);
        for (int v : views) {
            auto a = m_actions.find(v);
            if (a != m_actions.end())
                w.actions.push_back(a->second);
        }
        out.push_back(w);
    }
    return out;
}

void Nib::buildChildren(int obj, View *into, double height, bool flipped)
{
    for (int sub : m_archive.array(obj, "NSSubviews"))
        if (View *v = build(sub, height, flipped))
            into->addSubview(v);
}

View *Nib::build(int obj, double parentHeight, bool parentFlipped)
{
    const std::string cls = classOf(obj);
    // what NSScrollView, NSTableView and their kind are made of: drawn by their owner
    if (cls == "NSScroller" || cls == "NSClipView" || cls == "NSTableHeaderView" || cls == "_NSCornerView")
        return nullptr;
    Rect frame = frameOf(obj, parentHeight, parentFlipped);
    View *v = buildControl(obj, cls, frame);
    if (!v) {
        v = new Container;
        v->setFrame(frame);
        buildChildren(obj, v, frame.h, false);
    }
    if (u32(m_archive.integer(obj, "NSvFlags")) & 0x80000000)
        v->setHidden(true);
    if (m_archive.has(obj, "NSEnabled") && !m_archive.boolean(obj, "NSEnabled", true))
        v->setEnabled(false);
    m_views[obj] = v;
    const int cell = m_archive.object(obj, "NSCell");
    Connection c{v, {}, int(m_archive.integer(obj, "NSTag", m_archive.integer(cell, "NSTag"))), m_archive.string(cell, "NSContents")};
    auto a = m_actions.find(obj);
    if (a != m_actions.end())
        c.action = a->second;
    if (!c.action.empty() || c.tag || dynamic_cast<class Control *>(v) || dynamic_cast<TextField *>(v) || dynamic_cast<RadioGroup *>(v) ||
        dynamic_cast<TableView *>(v) || dynamic_cast<TabView *>(v))
        m_controls.push_back(c);
    return v;
}

View *Nib::buildControl(int obj, const std::string &cls, const Rect &frame)
{
    const int cell = m_archive.object(obj, "NSCell");
    const uint32_t flags = u32(m_archive.integer(cell, "NSCellFlags")), flags2 = u32(m_archive.integer(cell, "NSCellFlags2"));
    const bool small = ((flags2 >> 17) & 3) == 1;
    const int alignBits = (flags2 >> 26) & 7;
    const Align align = alignBits == 1 ? Align::Right : alignBits == 2 ? Align::Center : Align::Left;
    const std::string custom = customClassOf(obj);
    auto place = [&](View *v) {
        v->setFrame(frame);
        return v;
    };

    if (custom == "SFAuthorizationView")
        return place(new LockView);
    if (custom == "AddRemoveView") {
        // + and - side by side, as under 10.6's lists
        auto *v = new View;
        v->setFrame(frame);
        auto *add = v->add<Button>("+", Button::SmallSquare);
        add->setFrame({0, 0, 23, std::min(22.0, frame.h)});
        auto *remove = v->add<Button>("−", Button::SmallSquare);
        remove->setFrame({22, 0, 23, std::min(22.0, frame.h)});
        return v;
    }

    if (cls == "NSTextField" || cls == "NSSecureTextField" || cls == "NSTokenField" || cls == "NSSearchField" ||
        m_archive.isA(obj, "NSTextField")) {
        const bool editable = flags & 0x10000000, bezeled = flags & 0x00400000, bordered = flags & 0x00800000;
        const std::string text = m_archive.string(cell, "NSContents");
        if (cls == "NSSearchField") {
            auto *f = new SearchField;
            f->inToolbar = false;
            f->text = text;
            return place(f);
        }
        if (cls == "NSComboBox") {
            auto *f = new ComboBox;
            f->text = text;
            return place(f);
        }
        if (editable && (bezeled || bordered)) {
            auto *f = new TextField;
            f->text = text;
            f->font = fontOf(cell);
            f->align = align;
            f->secure = cls.find("Secure") != std::string::npos || custom.find("Secure") != std::string::npos;
            f->style = bezeled ? TextField::Bezel : TextField::Plain;
            return place(f);
        }
        // a label: NSTextFieldCell's text 2 points in
        auto *l = new Label(placeholder(text) ? std::string() : text, fontOf(cell));
        l->style.align = align;
        l->style.color = colorOf(m_archive.object(cell, "NSTextColor"), Color::gray(0));
        l->wrap = !(flags & 0x00100000) && frame.h > l->style.font.size * 1.8;
        if (m_archive.boolean(cell, "NSDrawsBackground"))
            l->background = colorOf(m_archive.object(cell, "NSBackgroundColor"), Color{0, 0, 0, 0});
        // NSTextFieldCell: no ellipsis, the text past the frame if it is longer
        l->style.elide = false;
        l->clipsToBounds = false;
        Rect r = frame;
        r.x += 2;
        r.w -= 4;
        l->setFrame(r);
        return l;
    }

    if (cls == "NSComboBox") {
        auto *f = new ComboBox;
        f->text = m_archive.string(cell, "NSContents");
        return place(f);
    }

    if (cls == "NSPopUpButton") {
        auto *p = new PopUpButton;
        const int menu = m_archive.object(cell, "NSMenu");
        for (int item : m_archive.array(menu, "NSMenuItems")) {
            if (m_archive.boolean(item, "NSIsHidden"))
                continue;
            if (m_archive.boolean(item, "NSIsSeparator")) {
                p->items.push_back(MenuItem::line());
                continue;
            }
            MenuItem mi{m_archive.string(item, "NSTitle")};
            if (placeholder(mi.title))
                mi.title = "\u2013";   // filled by the pane's code on the Mac
            mi.enabled = !m_archive.boolean(item, "NSIsDisabled");
            p->items.push_back(mi);
        }
        p->pullDown = m_archive.boolean(cell, "NSPullDown");
        p->setSelected(int(m_archive.integer(cell, "NSSelectedIndex", 0)));
        p->small = small;
        return place(p);
    }

    if (cls == "NSButton" || m_archive.isA(obj, "NSButton")) {
        const uint32_t bf2 = u32(m_archive.integer(cell, "NSButtonFlags2"));
        const int bezel = int((bf2 & 7) | ((bf2 & 0x20) >> 2));
        const int alt = m_archive.object(cell, "NSAlternateImage");
        const std::string altName = alt >= 0 ? m_archive.string(alt, "NSImageName") : std::string();
        const std::string title = m_archive.string(cell, "NSContents");
        const bool on = flags & 0x80000000;
        if (altName == "NSSwitch" || altName == "NSRadioButton") {
            auto *b = new CheckBox(title, altName == "NSRadioButton");
            b->setChecked(on);
            b->small = small;
            b->font = fontOf(cell);
            b->imageAbove = frame.h >= 30 && !title.empty();
            return place(b);
        }
        Image img;
        const int normal = m_archive.object(cell, "NSNormalImage");
        if (normal >= 0) {
            const std::string kind = m_archive.className(normal);
            img = image(kind == "NSCustomResource" ? m_archive.string(normal, "NSResourceName") : m_archive.string(normal, "NSImageName"));
        }
        Button::Kind kind = Button::Push;
        switch (bezel) {
        case 1: kind = Button::Push; break;
        case 9: kind = Button::Help; break;
        case 10: kind = Button::SmallSquare; break;
        case 7: kind = Button::Round; break;
        case 5: case 14: kind = Button::Disclosure; break;
        case 8: case 11: case 12: case 13: kind = Button::Textured; break;
        default: kind = title.empty() && !img.isNull() ? Button::Plain : (bezel == 0 ? Button::Plain : Button::SmallSquare);
        }
        // a borderless push button with an image: the image alone
        if (kind == Button::Push && frame.h < 25 && !img.isNull() && title.empty())
            kind = Button::Plain;
        // not bordered (NSButtonFlags 0x00800000): its title alone, in its cell's font
        if (kind == Button::Push && !(u32(m_archive.integer(cell, "NSButtonFlags")) & 0x00800000))
            kind = Button::Plain;
        auto *b = new Button(title, kind);
        if (kind == Button::Plain)
            b->font = fontOf(cell);
        b->image = img;
        b->small = small;
        b->isDefault = m_archive.string(cell, "NSKeyEquivalent") == "\r";
        if (kind == Button::Disclosure)
            b->open = on;
        return place(b);
    }

    if (cls == "NSSlider") {
        auto *s = new Slider;
        s->minValue = m_archive.real(cell, "NSMinValue", 0);
        s->maxValue = m_archive.real(cell, "NSMaxValue", 1);
        s->ticks = int(m_archive.integer(cell, "NSNumberOfTickMarks"));
        s->ticksAbove = m_archive.integer(cell, "NSTickMarkPosition") == 1;
        s->onlyTicks = m_archive.boolean(cell, "NSAllowsTickMarkValuesOnly");
        s->small = small;
        s->setDoubleValue(m_archive.real(cell, "NSValue", s->minValue));
        return place(s);
    }

    if (cls == "NSBox") {
        auto *b = new Box;
        const int type = int(m_archive.integer(obj, "NSBoxType"));
        b->kind = m_archive.boolean(obj, "NSTransparent") ? Box::None : type == 2 ? Box::Separator : type == 1 ? Box::Secondary
                  : type == 4 ? Box::None : Box::Primary;
        if (m_archive.integer(obj, "NSTitlePosition") != 0)
            b->title = m_archive.string(m_archive.object(obj, "NSTitleCell"), "NSContents");
        b->setFrame(frame);
        buildChildren(obj, b, frame.h, false);
        return b;
    }

    if (cls == "NSTabView") {
        auto *t = new TabView;
        const int type = int(m_archive.integer(obj, "NSTvFlags") & 7);
        t->noTabs = type >= 4;
        t->noBorder = type == 6;
        t->keepPageFrames = true;
        t->setFrame(frame);
        const std::vector<int> items = m_archive.array(obj, "NSTabViewItems");
        const int chosen = m_archive.object(obj, "NSSelectedTabViewItem");
        int selected = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            const int view = m_archive.object(items[i], "NSView");
            const Rect pf = frameOf(view, frame.h, false);
            auto *page = new Container;
            page->setFrame(pf);
            buildChildren(view, page, pf.h, false);
            t->addTab(m_archive.string(items[i], "NSLabel"), page);
            if (items[i] == chosen)
                selected = int(i);
        }
        t->select(selected);
        return t;
    }

    if (cls == "NSMatrix") {
        const std::vector<int> cells = m_archive.array(obj, "NSCells");
        const int cols = std::max(1, int(m_archive.integer(obj, "NSNumCols", 1)));
        const Rect size = parseRect(m_archive.string(obj, "NSCellSize")), gap = parseRect(m_archive.string(obj, "NSIntercellSpacing"));
        bool radio = false, buttons = false;
        for (int c : cells) {
            const int alt = m_archive.object(c, "NSAlternateImage");
            const std::string name = alt >= 0 ? m_archive.string(alt, "NSImageName") : std::string();
            radio = radio || name == "NSRadioButton";
            buttons = buttons || name == "NSSwitch" || name == "NSRadioButton";
        }
        View *holder = radio ? static_cast<View *>(new RadioGroup) : new Container;
        holder->clipsToBounds = false;
        holder->setFrame(frame);
        for (size_t i = 0; i < cells.size(); ++i) {
            const int c = cells[i];
            const Rect r{(double(i % size_t(cols))) * (size.w + gap.w), double(i / size_t(cols)) * (size.h + gap.h), size.w, size.h};
            const std::string title = m_archive.string(c, "NSContents");
            if (buttons) {
                auto *b = new CheckBox(title, radio);
                b->setChecked(u32(m_archive.integer(c, "NSCellFlags")) & 0x80000000);
                b->font = fontOf(c);
                b->small = ((u32(m_archive.integer(c, "NSCellFlags2")) >> 17) & 3) == 1;
                b->setFrame(r);
                holder->addSubview(b);
                if (auto *g = dynamic_cast<RadioGroup *>(holder)) {
                    g->buttons.push_back(b);
                    b->onToggle = [g, b](CheckBox *) {
                        for (size_t n = 0; n < g->buttons.size(); ++n)
                            g->buttons[n]->setChecked(g->buttons[n] == b);
                        if (g->onSelect)
                            g->onSelect(g->selected());
                    };
                }
                m_controls.push_back({b, {}, int(m_archive.integer(c, "NSTag")), title});
            } else {
                auto *l = new Label(title, fontOf(c));
                l->setFrame(r);
                holder->addSubview(l);
            }
        }
        return holder;
    }

    if (cls == "NSImageView" || custom == "NSImageView") {
        auto *iv = new ImageView;
        const int contents = m_archive.object(cell, "NSContents");
        if (contents >= 0)
            iv->image = image(m_archive.className(contents) == "NSCustomResource" ? m_archive.string(contents, "NSResourceName")
                                                                                 : m_archive.string(contents, "NSImageName"));
        const int style = int(m_archive.integer(cell, "NSStyle"));
        iv->frameStyle = style == 1 ? ImageView::Photo : style == 2 ? ImageView::GrayBezel : style == 3 ? ImageView::Groove
                         : style == 4 ? ImageView::Button : ImageView::NoFrame;
        return place(iv);
    }

    if (cls == "NSScrollView") {
        const int clip = m_archive.object(obj, "NSContentView");
        const int doc = m_archive.object(clip, "NSDocView");
        const std::string docClass = classOf(doc);
        if (doc >= 0 && (docClass == "NSTableView" || docClass == "NSOutlineView" || m_archive.isA(doc, "NSTableView"))) {
            auto *t = new TableView;
            for (int col : m_archive.array(doc, "NSTableColumns")) {
                TableView::Column c;
                c.title = m_archive.string(m_archive.object(col, "NSHeaderCell"), "NSContents");
                c.width = m_archive.real(col, "NSWidth", 100);
                const int data = m_archive.object(col, "NSDataCell");
                c.checkBox = m_archive.isA(data, "NSButtonCell") || classOf(data) == "NSButtonCell";
                t->columns.push_back(c);
            }
            t->header = m_archive.object(doc, "NSHeaderView") >= 0;
            t->rowHeight = m_archive.real(doc, "NSRowHeight", 17) + m_archive.real(doc, "NSIntercellSpacingHeight", 2);
            t->bordered = (m_archive.integer(obj, "NSsFlags") & 3) != 0;
            t->setFrame(frame);
            m_views[doc] = t;
            auto a = m_actions.find(doc);
            if (a != m_actions.end())
                m_controls.push_back({t, a->second, 0, {}});
            return t;
        }
        // another document: a white, framed area with the document's own views
        auto *v = new View;
        v->setFrame(frame);
        if (doc >= 0) {
            const Rect df = frameOf(doc, frame.h, true);
            auto *d = new View;
            d->setFrame(df);
            buildChildren(doc, d, df.h, true);
            v->addSubview(d);
        }
        return v;
    }

    if (cls == "NSProgressIndicator") {
        auto *p = new ProgressIndicator;
        const uint32_t pf = u32(m_archive.integer(obj, "NSpiFlags"));
        p->spinning = pf & 0x1000;
        p->indeterminate = pf & 0x2;
        p->minValue = m_archive.real(obj, "NSMinValue", 0);
        p->maxValue = m_archive.real(obj, "NSMaxValue", 100);
        return place(p);
    }

    if (cls == "NSSegmentedControl") {
        std::vector<std::string> labels;
        std::vector<double> widths;
        int selected = -1;
        for (int seg : m_archive.array(cell, "NSSegmentImages")) {
            const std::string l = m_archive.string(seg, "NSSegmentItemLabel");
            labels.push_back(l);
            const double w = m_archive.real(seg, "NSSegmentItemWidth", 0);
            widths.push_back(w > 0 ? w : std::round(textWidth(l, Font::system()) + 18));
            if (m_archive.boolean(seg, "NSSegmentItemSelected"))
                selected = int(labels.size()) - 1;
        }
        auto *s = new SegmentedControl(CoreUI::SegmentStyle::Tabs, labels, widths);
        s->setSelected(selected);
        return place(s);
    }

    if (cls == "NSStepper") {
        auto *s = new Stepper;
        s->minValue = m_archive.real(cell, "NSMinValue", 0);
        s->maxValue = m_archive.real(cell, "NSMaxValue", 100);
        s->value = m_archive.real(cell, "NSValue", 0);
        s->increment = m_archive.real(cell, "NSIncrement", 1);
        return place(s);
    }

    if (cls == "NSLevelIndicator") {
        auto *l = new LevelIndicator;
        l->maxValue = m_archive.real(cell, "NSMaxValue", 10);
        return place(l);
    }

    if (cls == "NSColorWell") {
        auto *w = new ColorWell;
        w->color = colorOf(m_archive.object(obj, "NSColor"), Color::gray(128));
        return place(w);
    }

    if (cls == "NSDatePicker") {
        const long long elements = m_archive.integer(cell, "NSDatePickerElements");
        // the graphical style (a calendar, a clock): the frames taller than a field
        if (frame.h > 40) {
            if (elements & 0xE0)
                return place(new CalendarView);
            return place(new ClockView);
        }
        // the date as text in its field
        auto *f = new TextField;
        f->editable = false;
        char buf[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(buf, sizeof buf, (elements & 0xE0) && !(elements & 0x0E) ? "%-m/%-d/%Y" : "%-I:%M:%S %p", std::localtime(&now));
        f->text = buf;
        return place(f);
    }

    return nullptr;
}

int Nib::outletObject(const std::string &name) const
{
    auto it = m_ownerOutlets.find(name);
    return it == m_ownerOutlets.end() ? -1 : it->second;
}

View *Nib::outlet(const std::string &name) const
{
    auto it = m_ownerOutlets.find(name);
    if (it == m_ownerOutlets.end())
        return nullptr;
    auto v = m_views.find(it->second);
    return v == m_views.end() ? nullptr : v->second;
}

std::vector<View *> Nib::withAction(const std::string &selector) const
{
    std::vector<View *> out;
    for (const Connection &c : m_controls)
        if (c.action == selector)
            out.push_back(c.view);
    return out;
}

View *Nib::withTitle(const std::string &title) const
{
    for (const Connection &c : m_controls)
        if (c.title == title)
            return c.view;
    return nullptr;
}

View *Nib::withTag(int tag) const
{
    for (const Connection &c : m_controls)
        if (c.tag == tag)
            return c.view;
    return nullptr;
}

} // namespace AK
