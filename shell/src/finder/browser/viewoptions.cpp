// SPDX-License-Identifier: LGPL-2.1-or-later
#include "viewoptions.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "Localizable", key, fallback); }

// the tab view's pages (their identifiers: Icon, List, Column, Desktop)
enum Page { IconPage = 0, ListPage = 1, ColumnPage = 2 };

bool inside(const View *v, const View *page)
{
    for (; v; v = v->superview())
        if (v == page)
            return true;
    return false;
}

// Arrange By's items as TViewOptionsSettings names them in icvp
const std::vector<std::pair<std::string, std::string>> &arrangements()
{
    static const std::vector<std::pair<std::string, std::string>> list = {
        {"None", "none"}, {"Snap to Grid", "grid"}, {"Name", "name"}, {"Date Modified", "dateModified"},
        {"Date Created", "dateCreated"}, {"Size", "size"}, {"Kind", "kind"}, {"Label", "label"},
    };
    return list;
}

} // namespace

ViewOptions &ViewOptions::shared()
{
    static ViewOptions panel;
    return panel;
}

bool ViewOptions::isOpen() const { return m_window && m_window->isVisible(); }

void ViewOptions::toggle(const ViewSettings &settings)
{
    if (isOpen()) {
        m_window->close();
        return;
    }
    m_settings = settings;
    if (!m_window)
        build();
    if (!m_window)
        return;
    fill();
    m_window->show();
}

void ViewOptions::update(const ViewSettings &settings)
{
    m_settings = settings;
    if (isOpen())
        fill();
}

void ViewOptions::build()
{
    m_nib = std::make_unique<Nib>();
    const std::string res = std::string(Bundle) + "/Contents/Resources/";
    if (!m_nib->load(Assets::path(res + "English.lproj/ViewOptionsWindow.nib/keyedobjects.nib"), Assets::path(res)))
        return;
    const auto windows = m_nib->windows();
    if (windows.empty())
        return;
    Size size;
    View *content = m_nib->instantiateWindow(windows.front().object, &size);
    // TViewOptionsWindow: an NSPanel over the Finder's windows
    m_window = std::make_unique<Window>(size, tr("VO1", "View Options"), Window::Titled);
    m_window->setFloatingPanel();
    m_window->contentView()->addSubview(content);
    content->setFrame({0, 0, size.w, size.h});

    const std::string base = "targetedViewOptionsSettings.";
    auto *tabs = m_nib->outlet<TabView>("_tabView");
    View *iconPage = tabs ? tabs->page(IconPage) : nullptr;
    View *listPage = tabs ? tabs->page(ListPage) : nullptr;
    View *columnPage = tabs ? tabs->page(ColumnPage) : nullptr;
    auto in = [&](const std::string &key, const std::string &binding, View *page) -> View * {
        for (View *v : m_nib->bound(base + key, binding))
            if (!page || inside(v, page))
                return v;
        return nullptr;
    };
    auto check = [&](const std::string &key, View *page, std::function<void(bool)> set) {
        if (auto *c = dynamic_cast<CheckBox *>(in(key, "value", page)))
            c->onToggle = [this, set](CheckBox *b) {
                if (m_filling)
                    return;
                set(b->checked());
                changed();
            };
    };
    auto textSize = [&](const std::string &key, View *page, int ViewSettings::*field) {
        if (auto *p = dynamic_cast<PopUpButton *>(in(key, "selectedValue", page)))
            p->onSelect = [this, p, field](int i) {
                if (m_filling || i < 0 || size_t(i) >= p->items.size())
                    return;
                m_settings.*field = std::atoi(p->items[size_t(i)].title.c_str());
                changed();
            };
    };

    // the icon view's page
    if (auto *s = m_nib->outlet<Slider>("_iconViewIconSizeSlider"))
        s->onChange = [this](double v) {
            if (m_filling)
                return;
            m_settings.iconSize = std::clamp(int(std::lround(v / 4)) * 4, 16, 512);   // by fours, as the status bar's
            changed();
        };
    if (auto *s = m_nib->outlet<Slider>("_iconViewGridSpacingSlider"))
        s->onChange = [this](double v) {
            if (m_filling)
                return;
            m_settings.gridSpacing = int(std::lround(v));
            changed();
        };
    textSize("iconViewTextSize", iconPage, &ViewSettings::iconTextSize);
    if (auto *g = dynamic_cast<RadioGroup *>(in("iconViewLabelOnBottom", "selectedIndex", iconPage)))
        g->onSelect = [this, g](int i) {
            if (m_filling || i < 0)
                return;
            m_settings.labelOnBottom = g->buttons[size_t(i)]->title == "Bottom";
            changed();
        };
    check("iconViewShowItemInfo", iconPage, [this](bool on) { m_settings.showItemInfo = on; });
    check("iconViewShowIconPreview", iconPage, [this](bool on) { m_settings.iconPreview = on; });
    if (auto *p = dynamic_cast<PopUpButton *>(in("iconViewArrangeBy", "selectedIndex", iconPage)))
        p->onSelect = [this, p](int i) {
            if (m_filling || i < 0 || size_t(i) >= p->items.size())
                return;
            for (const auto &[title, value] : arrangements())
                if (p->items[size_t(i)].title == title)
                    m_settings.arrangeBy = value;
            changed();
        };
    // the background (white, a colour, a picture): white only, for now
    if (View *bg = in("iconViewBackgroundType", "selectedIndex", iconPage))
        bg->setEnabled(false);

    // the list's page
    textSize("listViewTextSize", listPage, &ViewSettings::listTextSize);
    if (View *m = in("listViewIconSize", "selectedIndex", listPage)) {
        // its two cells are images of a small and a large icon over radio buttons: radio buttons here
        m->removeAllSubviews();
        auto *g = new RadioGroup;
        g->setFrame({0, 0, m->frame().w, m->frame().h});
        for (int k = 0; k < 2; ++k) {
            auto *b = new CheckBox({}, true);
            b->setFrame({k * m->frame().w / 2 + (k ? 6 : 0), 0, 18, m->frame().h});
            g->addSubview(b);
            g->buttons.push_back(b);
            b->onToggle = [this, g, b, k](CheckBox *) {
                for (CheckBox *o : g->buttons)
                    o->setChecked(o == b);
                if (m_filling)
                    return;
                m_settings.listIconSize = k ? 32 : 16;
                changed();
            };
        }
        m->addSubview(g);
    }
    const std::pair<const char *, const char *> columns[] = {
        {"listViewShowDateModified", "dateModified"}, {"listViewShowDateCreated", "dateCreated"}, {"listViewShowSize", "size"},
        {"listViewShowKind", "kind"},                 {"listViewShowVersion", "version"},         {"listViewShowComments", "comments"},
        {"listViewShowLabel", "label"},               {"listViewShowDateLastOpened", "dateLastOpened"},
    };
    for (const auto &[key, id] : columns) {
        const std::string column = id;
        check(key, listPage, [this, column](bool on) {
            auto &cols = m_settings.listColumns;
            cols.erase(std::remove(cols.begin(), cols.end(), column), cols.end());
            if (on)
                cols.push_back(column);
        });
    }
    check("listViewUseRelativeDates", listPage, [this](bool on) { m_settings.relativeDates = on; });
    check("listViewCalculateAllSizes", listPage, [this](bool on) { m_settings.calculateAllSizes = on; });
    check("listViewShowIconPreview", listPage, [this](bool on) { m_settings.listIconPreview = on; });

    // the column view's page
    textSize("columnViewTextSize", columnPage, &ViewSettings::columnTextSize);
    check("columnViewShowIcons", columnPage, [this](bool on) { m_settings.columnShowIcons = on; });
    check("columnViewShowIconPreview", columnPage, [this](bool on) { m_settings.columnIconPreview = on; });
    check("columnViewShowPreviewColumn", columnPage, [this](bool on) { m_settings.columnPreview = on; });
    if (View *arrange = in("columnViewArrangeBy", "selectedIndex", columnPage))
        arrange->setEnabled(false);   // the columns sorted by name, for now

    // the top: "Always open in icon view" (the folder's vstl); "Open in" and its pop-up are the
    // FXShowCustomViewStylePopup variant, hidden
    for (View *v : m_nib->bound(base + "nodeCustomViewStyleIndex", "selectedIndex"))
        v->setHidden(true);
    for (View *v : m_nib->bound(base + "nodeHasCustomViewStyle", "value"))
        if (auto *c = dynamic_cast<CheckBox *>(v)) {
            if (c->title == "Open in") {
                c->setHidden(true);
                continue;
            }
            c->onToggle = [this](CheckBox *b) {
                if (m_filling)
                    return;
                m_settings.alwaysOpen = b->checked();
                changed();
            };
        }
    // the bottom: Use as Defaults (Restore to Defaults with the Option key down)
    for (View *v : m_nib->bound("targetedViewOptionsSettings", "target")) {
        auto *b = dynamic_cast<Button *>(v);
        if (!b)
            continue;
        if (b->title == "Restore to Defaults")
            b->setHidden(true);
        else if (b->title == "Use as Defaults")
            b->action = [this] {
                if (onUseAsDefaults)
                    onUseAsDefaults();
            };
    }
}

void ViewOptions::fill()
{
    if (!m_nib || !m_window)
        return;
    m_filling = true;
    const ViewSettings &s = m_settings;
    m_window->setTitle(s.folderName.empty() ? tr("VO1", "View Options") : s.folderName);
    const std::string base = "targetedViewOptionsSettings.";
    auto *tabs = m_nib->outlet<TabView>("_tabView");
    const int page = s.view == 2 ? ColumnPage : s.view == 0 ? IconPage : ListPage;
    if (tabs)
        tabs->select(page);
    View *iconPage = tabs ? tabs->page(IconPage) : nullptr;
    View *listPage = tabs ? tabs->page(ListPage) : nullptr;
    View *columnPage = tabs ? tabs->page(ColumnPage) : nullptr;
    auto in = [&](const std::string &key, const std::string &binding, View *p) -> View * {
        for (View *v : m_nib->bound(base + key, binding))
            if (!p || inside(v, p))
                return v;
        return nullptr;
    };
    auto setCheck = [&](const std::string &key, View *p, bool on) {
        if (auto *c = dynamic_cast<CheckBox *>(in(key, "value", p)))
            c->setChecked(on);
    };
    auto setPopUp = [&](const std::string &key, const std::string &binding, View *p, const std::string &title) {
        if (auto *b = dynamic_cast<PopUpButton *>(in(key, binding, p)))
            for (size_t i = 0; i < b->items.size(); ++i)
                if (b->items[i].title == title)
                    b->setSelected(int(i));
    };

    // the top checkbox: "Always open in icon view" (VO14...VO17)
    static const char *const keys[] = {"VO14", "VO15", "VO16", "VO17"};
    static const char *const fallbacks[] = {"Always open in icon view", "Always open in list view", "Always open in column view",
                                            "Always open in Cover Flow"};
    for (View *v : m_nib->bound(base + "nodeHasCustomViewStyle", "value"))
        if (auto *c = dynamic_cast<CheckBox *>(v); c && !c->isHidden()) {
            c->title = tr(keys[std::clamp(s.view, 0, 3)], fallbacks[std::clamp(s.view, 0, 3)]);
            c->setChecked(s.alwaysOpen);
            c->setNeedsDisplay();
        }

    if (auto *slider = m_nib->outlet<Slider>("_iconViewIconSizeSlider"))
        slider->setDoubleValue(s.iconSize);
    if (auto *slider = m_nib->outlet<Slider>("_iconViewGridSpacingSlider"))
        slider->setDoubleValue(s.gridSpacing);
    // "64 × 64" (VO9 less its "Icon size: ", the nib's own label before it)
    if (auto *l = dynamic_cast<Label *>(in("iconViewIconSize", "value", iconPage))) {
        std::string f = tr("VO9", "Icon size: ^0 × ^1");
        const size_t colon = f.find(": ");
        if (colon != std::string::npos)
            f = f.substr(colon + 2);
        for (const char *mark : {"^0", "^1"}) {
            const size_t at = f.find(mark);
            if (at != std::string::npos)
                f.replace(at, 2, std::to_string(s.iconSize));
        }
        l->setText(f);
    }
    setPopUp("iconViewTextSize", "selectedValue", iconPage, std::to_string(s.iconTextSize));
    if (auto *g = dynamic_cast<RadioGroup *>(in("iconViewLabelOnBottom", "selectedIndex", iconPage)))
        for (size_t i = 0; i < g->buttons.size(); ++i)
            g->buttons[i]->setChecked((g->buttons[i]->title == "Bottom") == s.labelOnBottom);
    setCheck("iconViewShowItemInfo", iconPage, s.showItemInfo);
    setCheck("iconViewShowIconPreview", iconPage, s.iconPreview);
    for (const auto &[title, value] : arrangements())
        if (value == s.arrangeBy)
            setPopUp("iconViewArrangeBy", "selectedIndex", iconPage, title);
    if (auto *g = dynamic_cast<RadioGroup *>(in("iconViewBackgroundType", "selectedIndex", iconPage)))
        g->select(0);
    // White: neither the colour well nor the picture's well (iconViewHideColorWell, iconViewHideImageWell)
    for (const char *well : {"iconViewHideColorWell", "iconViewHideImageWell"})
        for (View *v : m_nib->bound(base + well, "hidden"))
            v->setHidden(true);

    setPopUp("listViewTextSize", "selectedValue", listPage, std::to_string(s.listTextSize));
    if (View *m = in("listViewIconSize", "selectedIndex", listPage))
        for (View *child : m->subviews())
            if (auto *g = dynamic_cast<RadioGroup *>(child))
                for (size_t i = 0; i < g->buttons.size(); ++i)
                    g->buttons[i]->setChecked((i == 1) == (s.listIconSize >= 32));
    auto shown = [&](const char *id) { return std::find(s.listColumns.begin(), s.listColumns.end(), id) != s.listColumns.end(); };
    setCheck("listViewShowDateModified", listPage, shown("dateModified"));
    setCheck("listViewShowDateCreated", listPage, shown("dateCreated"));
    setCheck("listViewShowSize", listPage, shown("size"));
    setCheck("listViewShowKind", listPage, shown("kind"));
    setCheck("listViewShowVersion", listPage, shown("version"));
    setCheck("listViewShowComments", listPage, shown("comments"));
    setCheck("listViewShowLabel", listPage, shown("label"));
    setCheck("listViewShowDateLastOpened", listPage, shown("dateLastOpened"));
    setCheck("listViewUseRelativeDates", listPage, s.relativeDates);
    setCheck("listViewCalculateAllSizes", listPage, s.calculateAllSizes);
    setCheck("listViewShowIconPreview", listPage, s.listIconPreview);

    setPopUp("columnViewTextSize", "selectedValue", columnPage, std::to_string(s.columnTextSize));
    setCheck("columnViewShowIcons", columnPage, s.columnShowIcons);
    setCheck("columnViewShowIconPreview", columnPage, s.columnIconPreview);
    setCheck("columnViewShowPreviewColumn", columnPage, s.columnPreview);
    if (auto *c = dynamic_cast<CheckBox *>(in("columnViewShowIconPreview", "value", columnPage)))
        c->setEnabled(s.columnShowIcons);   // enabled: columnViewShowIcons
    m_filling = false;
    m_window->contentView()->setNeedsDisplay();
}

void ViewOptions::changed()
{
    fill();   // the labels that follow (the icon size's)
    if (onChange)
        onChange(m_settings);
}

} // namespace Finder
