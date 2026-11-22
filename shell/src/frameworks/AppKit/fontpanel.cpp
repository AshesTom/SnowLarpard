// SPDX-License-Identifier: LGPL-2.1-or-later
#include "fontpanel.h"

#include "controls.h"
#include "foundation.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <strings.h>

namespace AK {

namespace {

const char *const AppKitBundle = "/System/Library/Frameworks/AppKit.framework/Versions/C";
std::string fm(const std::string &key) { return Assets::tr(AppKitBundle, "FontManager", key, key); }

const int Sizes[] = {9, 10, 11, 12, 13, 14, 18, 24, 36, 48, 64, 72, 96, 144, 288};

struct Face {
    std::string name;   // "Regular", "Bold Italic"
    int weight = 400;
    bool italic = false;
};

struct Family {
    std::string name;
    bool monospace = false;
};

std::vector<Family> allFamilies()
{
    std::vector<Family> out;
    PangoFontFamily **families = nullptr;
    int n = 0;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &families, &n);
    for (int i = 0; i < n; ++i) {
        const std::string name = pango_font_family_get_name(families[i]);
        // fontconfig's aliases are not families
        if (name == "Sans" || name == "Serif" || name == "Monospace" || name == "System-ui" || name == "sans-serif" || name == "serif" ||
            name == "monospace" || name == "system-ui" || name == "cursive" || name == "fantasy" || name == "emoji" || name == "math")
            continue;
        out.push_back({name, pango_font_family_is_monospace(families[i]) != 0});
    }
    g_free(families);
    std::sort(out.begin(), out.end(), [](const Family &a, const Family &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    out.erase(std::unique(out.begin(), out.end(), [](const Family &a, const Family &b) { return a.name == b.name; }), out.end());
    return out;
}

std::vector<Face> facesOf(const std::string &family)
{
    std::vector<Face> out;
    PangoFontFamily **families = nullptr;
    int n = 0;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &families, &n);
    for (int i = 0; i < n; ++i) {
        if (family != pango_font_family_get_name(families[i]))
            continue;
        PangoFontFace **faces = nullptr;
        int m = 0;
        pango_font_family_list_faces(families[i], &faces, &m);
        for (int k = 0; k < m; ++k) {
            if (pango_font_face_is_synthesized(faces[k]))
                continue;
            PangoFontDescription *d = pango_font_face_describe(faces[k]);
            Face f;
            f.name = pango_font_face_get_face_name(faces[k]);
            f.weight = int(pango_font_description_get_weight(d));
            f.italic = pango_font_description_get_style(d) != PANGO_STYLE_NORMAL;
            pango_font_description_free(d);
            out.push_back(f);
        }
        g_free(faces);
        break;
    }
    g_free(families);
    // NSFontManager's order: by weight, then upright before slanted
    std::stable_sort(out.begin(), out.end(), [](const Face &a, const Face &b) {
        return a.weight != b.weight ? a.weight < b.weight : a.italic < b.italic;
    });
    if (out.empty())
        out.push_back({"Regular", 400, false});
    return out;
}

// a style's typeface among a family's: its weight and slant
int faceIndex(const std::vector<Face> &faces, const Style &s)
{
    const int want = s.bold ? 700 : s.face == "Light" ? 300 : s.face == "Thin" ? 100 : s.face == "UltraLight" ? 200
                                   : s.face == "Medium" ? 500 : s.face == "Black" || s.face == "Heavy" ? 900 : 400;
    int best = 0, bestScore = 1 << 30;
    for (size_t i = 0; i < faces.size(); ++i) {
        const int score = std::abs(faces[i].weight - want) + (faces[i].italic != s.italic ? 10000 : 0) +
                          (!s.face.empty() && faces[i].name.find(s.face) != std::string::npos ? -50 : 0);
        if (score < bestScore) {
            best = int(i);
            bestScore = score;
        }
    }
    return best;
}

void applyFace(Style &s, const Face &f)
{
    s.bold = f.weight >= 600;
    s.italic = f.italic;
    s.face.clear();
    for (const char *w : {"UltraLight", "Light", "Thin", "Medium", "Black", "Heavy", "Wide"})
        if (f.name.find(w) != std::string::npos)
            s.face = w;
}

std::string sizeText(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, std::fabs(v - std::round(v)) < 0.05 ? "%.0f" : "%.1f", v);
    return buf;
}

// AppKit's own pictures of the panel (extract.py cuts them out of AppKit's atlas)
Image picture(const std::string &name)
{
    return Image::load(Assets::path(std::string(AppKitBundle) + "/Resources/Images/" + name + ".png"));
}

// The panel's picture buttons: their picture, the pressed one while held (or while on)
class PictureButton : public View {
public:
    Image normal, pressed;
    bool on = false;
    bool enabled = true;
    std::function<void()> onPress;
    void draw(cairo_t *cr) override
    {
        const Image &img = (m_down || on) && !pressed.isNull() ? pressed : normal;
        if (!img.isNull())
            img.draw(cr, Point{0, 0}, enabled ? 1 : 0.5);
    }
    void mouseDown(Event &) override
    {
        if (!enabled)
            return;
        m_down = true;
        setNeedsDisplay();
        if (onPress)
            onPress();
        m_down = false;
        setNeedsDisplay();
    }

private:
    bool m_down = false;
};

// NSFontPanelEffectsSliderCell: NSFontPanelSliderTrack (41 × 10) and its thumb (7 × 7)
class EffectsSlider : public View {
public:
    double value = 0, minValue = 0, maxValue = 1;
    std::function<void()> onChange;
    void draw(cairo_t *cr) override
    {
        static const Image track = picture("NSFontPanelSliderTrack"), thumb = picture("NSFontPanelSliderThumb"),
                           thumbDown = picture("NSFontPanelSliderThumbPressed");
        track.draw(cr, Point{0, std::floor((frame().h - track.height()) / 2)});
        const double f = maxValue > minValue ? (value - minValue) / (maxValue - minValue) : 0;
        const Image &t = m_drag ? thumbDown : thumb;
        t.draw(cr, Point{std::round(2 + f * (frame().w - 4 - t.width())), std::floor((frame().h - t.height()) / 2)});
    }
    void mouseDown(Event &e) override
    {
        m_drag = true;
        track(e.location.x);
    }
    void mouseDragged(Event &e) override { track(e.location.x); }
    void mouseUp(Event &) override
    {
        m_drag = false;
        setNeedsDisplay();
    }

private:
    bool m_drag = false;
    void track(double x)
    {
        const double f = std::clamp((x - 5.5) / (frame().w - 11), 0.0, 1.0);
        value = minValue + f * (maxValue - minValue);
        setNeedsDisplay();
        if (onChange)
            onChange();
    }
};

// The shadow's angle: a small NSCircularSlider (0 to 360, its dot clockwise from the top)
class AngleDial : public View {
public:
    double value = 135;
    std::function<void()> onChange;
    void draw(cairo_t *cr) override
    {
        const double cx = frame().w / 2, cy = frame().h / 2, r = std::min(cx, cy) - 1.5;
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, cy - r, 0, cy + r);
        cairo_pattern_add_color_stop_rgb(g, 0, 0.98, 0.98, 0.98);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.82, 0.82, 0.82);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        const double a = (value - 90) * M_PI / 180;
        cairo_arc(cr, cx + std::cos(a) * (r - 3), cy + std::sin(a) * (r - 3), 1.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.15, 0.15, 0.15, 1);
        cairo_fill(cr);
    }
    void mouseDown(Event &e) override { track(e.location); }
    void mouseDragged(Event &e) override { track(e.location); }

private:
    void track(Point p)
    {
        double deg = std::atan2(p.y - frame().h / 2, p.x - frame().w / 2) * 180 / M_PI + 90;
        while (deg < 0)
            deg += 360;
        value = std::fmod(deg, 360);
        setNeedsDisplay();
        if (onChange)
            onChange();
    }
};

struct Panel {
    std::unique_ptr<Window> window;
    std::function<TextView *()> target;
    TableView *collections = nullptr, *families = nullptr, *faces = nullptr, *sizes = nullptr;
    TextField *sizeField = nullptr;
    Slider *slider = nullptr;
    SearchField *search = nullptr;
    std::vector<Family> all;
    std::vector<std::string> shownFamilies;
    std::vector<Face> shownFaces;
    std::string family;
    Face face;
    double size = 12;
    Style lastSeen;
    bool haveSeen = false;
    int timer = 0;
    // the effects bar (NSFontEffectsBox)
    View *effects = nullptr, *main = nullptr;
    bool effectsShown = true;
    PictureButton *underline = nullptr, *strike = nullptr, *shadowToggle = nullptr;
    EffectsSlider *opacity = nullptr, *blur = nullptr, *radius = nullptr;
    AngleDial *angle = nullptr;
    Label *angleLabel = nullptr;
    PopUpButton *actions = nullptr;

    TextView *text() const { return target ? target() : nullptr; }

    std::vector<std::string> list(const char *key)
    {
        return Settings("fontpanel").list(key);
    }

    void fillFamilies()
    {
        const int c = collections ? collections->selected : 0;
        std::vector<std::string> favorites = list("Favorites"), recent = list("RecentlyUsed");
        const std::string q = search ? search->text : std::string();
        shownFamilies.clear();
        auto matches = [&q](const std::string &name) { return q.empty() || strcasestr(name.c_str(), q.c_str()) != nullptr; };
        if (c == 1 || c == 2) {
            for (const std::string &f : c == 1 ? favorites : recent)
                if (matches(f))
                    shownFamilies.push_back(f);
        } else {
            for (const Family &f : all)
                if ((c != 3 || f.monospace) && matches(f.name))
                    shownFamilies.push_back(f.name);
        }
        families->rows.clear();
        families->selected = -1;
        for (size_t i = 0; i < shownFamilies.size(); ++i) {
            families->rows.push_back({shownFamilies[i]});
            if (shownFamilies[i] == family)
                families->selected = int(i);
        }
        families->reload();
        reveal(families);
    }

    void fillFaces(const Style *style)
    {
        shownFaces = facesOf(family);
        faces->rows.clear();
        for (const Face &f : shownFaces)
            faces->rows.push_back({f.name});
        int i = -1;
        if (style)
            i = faceIndex(shownFaces, *style);
        else
            for (size_t k = 0; k < shownFaces.size(); ++k)
                if (shownFaces[k].name == face.name)
                    i = int(k);
        if (i < 0)
            i = faceIndex(shownFaces, Style());   // the regular one
        faces->selected = i;
        face = shownFaces[size_t(i)];
        faces->reload();
        reveal(faces);
    }

    void showSize()
    {
        if (sizeField)
            sizeField->setText(sizeText(size));
        sizes->selected = -1;
        for (size_t i = 0; i < std::size(Sizes); ++i)
            if (std::fabs(Sizes[i] - size) < 0.05)
                sizes->selected = int(i);
        sizes->reload();
        reveal(sizes);
        if (slider)
            slider->setDoubleValue(std::clamp(size, slider->minValue, slider->maxValue));
    }

    static void reveal(TableView *t)
    {
        if (t->selected >= 0)
            t->scroll()->scrollToVisible({0, t->selected * t->rowHeight, 10, t->rowHeight});
    }

    // the selection's font shown (setSelectedFont:)
    void follow(bool force)
    {
        TextView *t = text();
        if (!t)
            return;
        const Style s = t->styleAtSelection();
        if (!force && haveSeen && s.family == lastSeen.family && s.face == lastSeen.face && s.bold == lastSeen.bold &&
            s.italic == lastSeen.italic && s.size == lastSeen.size && s.underline == lastSeen.underline && s.strike == lastSeen.strike &&
            s.shadow == lastSeen.shadow)
            return;
        showEffects(s);
        lastSeen = s;
        haveSeen = true;
        // the family as fontconfig names it (Helvetica may be another's alias)
        family = s.family;
        for (const Family &f : all)
            if (strcasecmp(f.name.c_str(), s.family.c_str()) == 0)
                family = f.name;
        size = s.size;
        fillFamilies();
        fillFaces(&s);
        showSize();
    }

    // -[NSFontEffectsBox _setAttributes:isMultiple:] (0x3ac270): the bar shows the selection's
    void showEffects(const Style &s)
    {
        if (!underline)
            return;
        underline->on = s.underline != 0;
        strike->on = s.strike != 0;
        shadowToggle->on = s.shadow.on;
        if (s.shadow.on) {
            opacity->value = s.shadow.alpha;
            blur->value = std::clamp(s.shadow.blur, blur->minValue, blur->maxValue);
            const double dist = std::hypot(s.shadow.dx, s.shadow.dy);
            radius->value = std::clamp(s.size > 0 ? dist / s.size : 0.125, radius->minValue, radius->maxValue);
            if (dist > 0) {
                double a = 450 - std::atan2(s.shadow.dy, s.shadow.dx) * 180 / M_PI;
                angle->value = std::fmod(std::fmod(a, 360) + 360, 360);
            }
        }
        showAngle();
        for (View *v : std::initializer_list<View *>{underline, strike, shadowToggle, opacity, blur, radius, angle})
            v->setNeedsDisplay();
    }

    void showAngle()
    {
        // -_changeShadowAngle: (0x3abc9f): "%d%@" with the degree sign, the dial's angle as the
        // offset's direction (450 − the dial, within 360)
        double a = 450 - angle->value;
        if (a >= 360)
            a -= 360;
        if (angleLabel)
            angleLabel->setText(std::to_string(std::lround(a) % 360) + "\u00B0");
    }

    // -[NSFontEffectsBox convertAttributes:] (0x3abf3f): the shadow from the bar, each run's: its
    // distance the radius slider times the run's point size, its direction 450° − the dial, the blur
    // slider's radius, black at the opacity slider's alpha; none when the toggle is off
    void sendShadow()
    {
        TextView *t = text();
        if (!t)
            return;
        const bool on = shadowToggle->on;
        double a = 450 - angle->value;
        if (a > 360)
            a -= 360;
        const double rad = a * M_PI / 180, r = radius->value, b = blur->value, al = opacity->value;
        t->changeStyle([=](Style &s) {
            s.shadow.on = on;
            if (on) {
                s.shadow.dx = std::cos(rad) * r * s.size;
                s.shadow.dy = std::sin(rad) * r * s.size;
                s.shadow.blur = b;
                s.shadow.alpha = al;
            }
        });
        lastSeen = t->styleAtSelection();
    }

    // _openEffectsButton:: None, Single, Double (Color… with the colour panel: not here)
    void effectsMenu(PictureButton *b, bool isStrike)
    {
        TextView *t = text();
        const Style cur = t ? t->styleAtSelection() : Style();
        const int now = isStrike ? cur.strike : cur.underline;
        const std::vector<MenuItem> items = {MenuItem("None", false, true, now == 0), MenuItem("Single", false, true, now == 1),
                                             MenuItem("Double", false, true, now == 2), MenuItem({}, true),
                                             MenuItem("Color\u2026", false, false)};
        const Rect f = b->frame(), e = effects->frame();
        b->on = true;
        b->setNeedsDisplay();
        Menu::popUp(window.get(), Point{e.x + f.x, e.y + f.bottom()}, items, 90, Font::system(13), [this, b, isStrike](int i) {
            TextView *tv = text();
            if (tv && i >= 0 && i <= 2)
                tv->changeStyle([i, isStrike](Style &s) { (isStrike ? s.strike : s.underline) = i; });
            if (tv)
                showEffects(lastSeen = tv->styleAtSelection());
            b->setNeedsDisplay();
        });
    }

    // Show Effects / Hide Effects: the bar above the columns, the panel taller by it
    void layoutEffects()
    {
        const double top = effectsShown ? 40 : 0;
        effects->setHidden(!effectsShown);
        main->setFrame({0, top, 622, 231});
        window->setContentSize({622, 231 + top});
        if (actions)
            actions->items[3].title = fm(effectsShown ? "Hide Effects" : "Show Effects");
    }

    // -changeFont: to the text: each run converted (NSFontManager convertFont:)
    enum Change { FamilyChange, FaceChange, SizeChange };
    void send(Change c)
    {
        TextView *t = text();
        if (!t)
            return;
        const std::string fam = family;
        const Face f = face;
        const double sz = size;
        t->changeStyle([&](Style &s) {
            if (c == FamilyChange || c == FaceChange) {
                s.family = fam;
                applyFace(s, f);
            } else {
                s.size = sz;
            }
        });
        if (c == FamilyChange) {
            // Recently Used: the family first, ten kept
            std::vector<std::string> recent = list("RecentlyUsed");
            recent.erase(std::remove(recent.begin(), recent.end(), fam), recent.end());
            recent.insert(recent.begin(), fam);
            if (recent.size() > 10)
                recent.resize(10);
            Settings("fontpanel").set("RecentlyUsed", recent);
        }
        lastSeen = t->styleAtSelection();
        if (t->window())
            t->setNeedsDisplay();
    }

    void build()
    {
        // the nib's main box (622 × 271) without its effects bar (the top 40 points)
        const double W = 622, H = 231;
        window = std::make_unique<Window>(Size{W, H + 40}, "Fonts");
        window->setFloatingPanel(true);
        main = window->contentView()->add<View>();
        main->setFrame({0, 40, W, H});
        View *c = main;
        all = allFamilies();
        auto table = [&](const Rect &r, const std::string &title, bool header) {
            auto *t = c->add<TableView>();
            t->columns = {{title, r.w - 17}};
            t->header = header;
            t->setFrame(r);
            return t;
        };
        collections = table({1, 18, 161, 181}, "Collection", true);
        families = table({163, 18, 155, 181}, "Family", true);
        faces = table({320, 18, 158, 181}, "Typeface", true);
        sizes = table({533, 40, 39, 159}, "Size", false);
        auto *sizeLabel = c->add<Label>("Size", Font::system(11));
        sizeLabel->setFrame({533, 1, 64, 14});
        collections->rows = {{fm("All Fonts")}, {fm("Favorites")}, {fm("Recently Used")}, {"Fixed Width"}};
        collections->selected = 0;
        collections->reload();
        for (int s : Sizes)
            sizes->rows.push_back({std::to_string(s)});
        // the sizes right-aligned in their narrow column
        sizes->drawCell = [](cairo_t *cr, int row, int, const Rect &cell, bool highlighted) {
            TextStyle t;
            t.align = Align::Right;
            t.font = Font::system(12);
            t.color = highlighted ? Color::gray(255) : Color::gray(0);
            drawText(cr, std::to_string(Sizes[row]), Rect{cell.x, cell.y, cell.w + 2, cell.h}, t);
            return true;
        };
        sizes->reload();

        sizeField = c->add<TextField>();
        sizeField->setFrame({533, 16, 64, 21});
        slider = c->add<Slider>();
        slider->minValue = 9;
        slider->maxValue = 72;
        slider->setFrame({588, 38, 21, 164});

        // + and − (collections: not here), AppKit's pictures
        auto *add = c->add<PictureButton>();
        add->normal = picture("NSFontPanelPlusIdle");
        add->pressed = picture("NSFontPanelPlusPressed");
        add->enabled = false;
        add->setFrame({3, 205, 23, 20});
        auto *remove = c->add<PictureButton>();
        remove->normal = picture("NSFontPanelMinusIdle");
        remove->pressed = picture("NSFontPanelMinusPressed");
        remove->enabled = false;
        remove->setFrame({26, 205, 22, 20});
        actions = c->add<PopUpButton>();
        actions->pullDown = true;
        actions->small = true;
        actions->items = {MenuItem("⚙"), MenuItem(fm("Add to Favorites") == "Add to Favorites" ? "Add to Favorites" : fm("Add to Favorites")),
                          MenuItem(fm("Show Preview"), false, false), MenuItem(fm("Hide Effects")), MenuItem({}, true),
                          MenuItem("Color…", false, false), MenuItem("Characters…", false, false), MenuItem("Typography…", false, false),
                          MenuItem({}, true), MenuItem("Edit Sizes…", false, false), MenuItem({}, true),
                          MenuItem("Manage Fonts…", false, false)};
        actions->setFrame({52, 205, 28, 20});
        actions->items[0].title = " ";
        actions->setHidden(true);   // its menu opened by the picture over it (NSFontPanelActionButton)
        auto *gear = c->add<PictureButton>();
        gear->normal = picture("NSFontPanelActionButton");
        gear->pressed = picture("NSFontPanelActionButtonPressed");
        gear->setFrame({52, 205, 28, 22});
        gear->onPress = [this, gear] {
            std::vector<MenuItem> items(actions->items.begin() + 1, actions->items.end());
            const Rect f = gear->frame(), m = main->frame();
            Menu::popUp(window.get(), Point{m.x + f.x, m.y + f.bottom()}, items, 150, Font::system(13), [this](int i) {
                if (actions->onSelect && i >= 0)
                    actions->onSelect(i + 1);
            });
        };
        search = c->add<SearchField>();
        search->inToolbar = false;
        search->setFrame({144, 205, 155, 19});

        Panel *p = this;
        collections->onSelect = [p](int) { p->fillFamilies(); };
        families->onSelect = [p](int i) {
            if (i < 0 || size_t(i) >= p->shownFamilies.size())
                return;
            p->family = p->shownFamilies[size_t(i)];
            p->fillFaces(nullptr);   // the typeface's name kept when the family has it
            p->send(FamilyChange);
        };
        faces->onSelect = [p](int i) {
            if (i < 0 || size_t(i) >= p->shownFaces.size())
                return;
            p->face = p->shownFaces[size_t(i)];
            p->send(FaceChange);
        };
        sizes->onSelect = [p](int i) {
            if (i < 0 || size_t(i) >= std::size(Sizes))
                return;
            p->size = Sizes[i];
            p->showSize();
            p->send(SizeChange);
        };
        sizeField->onEnter = [p] {
            const double v = std::strtod(p->sizeField->text.c_str(), nullptr);
            if (v > 0 && v <= 4096) {
                p->size = v;
                p->send(SizeChange);
            }
            p->showSize();
        };
        slider->onChange = [p](double) {
            p->size = std::round(p->slider->doubleValue());
            if (p->sizeField)
                p->sizeField->setText(sizeText(p->size));
            p->send(SizeChange);
        };
        actions->onSelect = [p](int i) {
            if (i == 3) {
                p->effectsShown = !p->effectsShown;
                p->layoutEffects();
                return;
            }
            if (i == 1 && !p->family.empty()) {
                std::vector<std::string> favorites = p->list("Favorites");
                if (std::find(favorites.begin(), favorites.end(), p->family) == favorites.end())
                    favorites.push_back(p->family);
                Settings("fontpanel").set("Favorites", favorites);
                if (p->collections->selected == 1)
                    p->fillFamilies();
            }
        };
        search->onChange = [p](const std::string &) { p->fillFamilies(); };

        // NSFontEffectsBox (NSFontPanel.nib's, 616 × 34 at the top): underline and strikethrough
        // (pop-up picture buttons), text and document colour (the colour panel: not here), the divider,
        // the shadow's toggle, opacity, blur and distance sliders and its angle dial
        effects = window->contentView()->add<View>();
        effects->setFrame({3, 0, 616, 34});
        auto pictureButton = [&](const char *name, const Rect &r) {
            auto *b = effects->add<PictureButton>();
            b->normal = picture(name);
            b->pressed = picture(std::string(name) + "Pressed");
            b->setFrame(r);
            return b;
        };
        auto icon = [&](const char *name, const Rect &r) {
            auto *v = effects->add<ImageView>();
            v->image = picture(name);
            v->setFrame(r);
        };
        underline = pictureButton("NSFontPanelUnderlineEffect", {53, 2, 40, 22});
        strike = pictureButton("NSFontPanelStrikeEffect", {99, 2, 40, 22});
        auto *textColour = pictureButton("NSFontPanelTextColour", {145, 2, 40, 22});
        auto *paperColour = pictureButton("NSFontPanelPaperColour", {191, 2, 40, 22});
        textColour->enabled = paperColour->enabled = false;
        icon("NSFontPanelEffectsDivider", {235, 1, 1, 23});
        shadowToggle = pictureButton("NSFontPanelDropEffect", {240, 2, 40, 22});
        icon("NSFontPanelOpacityEffect", {302, 3, 13, 9});
        icon("NSFontPanelBlurEffect", {343, 0, 20, 14});
        icon("NSFontPanelSpreadEffect", {394, 1, 11, 11});
        auto slider = [&](double x, double lo, double hi, double v) {
            auto *sl = effects->add<EffectsSlider>();
            sl->minValue = lo;
            sl->maxValue = hi;
            sl->value = v;
            sl->setFrame({x, 13, 41, 12});
            return sl;
        };
        // -awakeFromNib (0x3aadd4): opacity 0…1 at ⅓, blur 0…10 (the nib's 50: its most), the
        // distance 0.01…0.5 at 0.125 of the point size
        opacity = slider(287, 0, 1, 1.0 / 3);
        blur = slider(333, 0, 10, 10);
        radius = slider(379, 0.01, 0.5, 0.125);
        angle = effects->add<AngleDial>();
        angle->setFrame({430, 3, 18, 20});
        angleLabel = effects->add<Label>("", Font::system(11));
        angleLabel->setFrame({453, 6, 35, 17});
        auto *line = effects->add<Box>();
        line->kind = Box::Separator;
        line->setFrame({-3, 33, 622, 1});
        showAngle();
        underline->onPress = [p] { p->effectsMenu(p->underline, false); };
        strike->onPress = [p] { p->effectsMenu(p->strike, true); };
        // _toggleShadow: the toggle flipped, the shadow sent
        shadowToggle->onPress = [p] {
            p->shadowToggle->on = !p->shadowToggle->on;
            p->sendShadow();
        };
        for (EffectsSlider *sl : {opacity, blur, radius})
            sl->onChange = [p] {
                if (p->shadowToggle->on)
                    p->sendShadow();
            };
        angle->onChange = [p] {
            p->showAngle();
            if (p->shadowToggle->on)
                p->sendShadow();
        };
        layoutEffects();
        window->onClose = [p] {
            if (p->timer) {
                App::shared().removeTimer(p->timer);
                p->timer = 0;
            }
        };
    }
};

Panel &panel()
{
    static Panel p;
    return p;
}

} // namespace

void FontPanel::setTarget(std::function<TextView *()> target) { panel().target = std::move(target); }

bool FontPanel::isVisible() { return panel().window && panel().window->isVisible(); }

void FontPanel::show()
{
    Panel &p = panel();
    if (!p.window)
        p.build();
    p.follow(true);
    p.window->orderFront();
    if (!p.timer)
        p.timer = App::shared().addTimer(0.25, true, [] { panel().follow(false); });
}

} // namespace AK
