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

// NSActionTemplate on the action button: a gear, drawn (its picture is not on the DVD's assets)
class Gear : public View {
public:
    void draw(cairo_t *cr) override
    {
        const double cx = frame().w / 2, cy = frame().h / 2, r = 5.5, teeth = 8;
        cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 1);
        for (int k = 0; k < teeth; ++k) {
            const double a = k * 2 * M_PI / teeth;
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_rotate(cr, a);
            cairo_rectangle(cr, -1.2, -r - 1.5, 2.4, 3);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.93, 0.93, 0.93, 1);
        cairo_arc(cr, cx, cy, 2, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    View *hitTest(Point) override { return nullptr; }
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
            s.italic == lastSeen.italic && s.size == lastSeen.size)
            return;
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
        window = std::make_unique<Window>(Size{W, H}, fm("Font") == "Font" ? "Fonts" : "Fonts");
        window->setFloatingPanel(true);
        View *c = window->contentView();
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

        auto *add = c->add<Button>("+", Button::SmallSquare);
        add->setFrame({3, 205, 23, 20});
        add->setEnabled(false);
        auto *remove = c->add<Button>("−", Button::SmallSquare);
        remove->setFrame({26, 205, 22, 20});
        remove->setEnabled(false);
        auto *actions = c->add<PopUpButton>();
        actions->pullDown = true;
        actions->small = true;
        actions->items = {MenuItem("⚙"), MenuItem(fm("Add to Favorites") == "Add to Favorites" ? "Add to Favorites" : fm("Add to Favorites")),
                          MenuItem(fm("Show Preview"), false, false), MenuItem(fm("Show Effects"), false, false), MenuItem({}, true),
                          MenuItem("Color…", false, false), MenuItem("Characters…", false, false), MenuItem("Typography…", false, false),
                          MenuItem({}, true), MenuItem("Edit Sizes…", false, false), MenuItem({}, true),
                          MenuItem("Manage Fonts…", false, false)};
        actions->setFrame({52, 205, 40, 20});
        actions->items[0].title = " ";
        auto *gear = c->add<Gear>();
        gear->setFrame({56, 205, 18, 20});
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
