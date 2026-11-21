// SPDX-License-Identifier: LGPL-2.1-or-later
#include "textruleroptions.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace AK {

namespace {

const char *const RulerOptionsNib = "/System/Library/Frameworks/AppKit.framework/Versions/C/Resources/English.lproj/NSTextRulerOptions.nib";

// -[NSLayoutManager defaultLineHeightForFont:]: the font's ascent and descent (its leading in)
double defaultLineHeight(const Style &s)
{
    PangoFontMap *map = pango_cairo_font_map_get_default();
    PangoContext *ctx = pango_font_map_create_context(map);
    PangoFontDescription *d = pango_font_description_new();
    pango_font_description_set_family(d, (s.family + ", Helvetica, Lucida Grande, sans-serif").c_str());
    pango_font_description_set_weight(d, s.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(d, s.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(d, s.size * PANGO_SCALE);
    double h = s.size * 1.2;
    if (PangoFont *font = pango_font_map_load_font(map, ctx, d)) {
        PangoFontMetrics *m = pango_font_get_metrics(font, nullptr);
        h = (pango_font_metrics_get_ascent(m) + pango_font_metrics_get_descent(m)) / double(PANGO_SCALE);
        pango_font_metrics_unref(m);
        g_object_unref(font);
    }
    pango_font_description_free(d);
    g_object_unref(ctx);
    return std::ceil(h);
}

// A field and its stepper as one value (NSTextField's number formatter: one decimal)
struct Number {
    TextField *field = nullptr;
    Stepper *stepper = nullptr;
    double get() const
    {
        if (field)
            return std::strtod(field->text.c_str(), nullptr);
        return stepper ? stepper->value : 0;
    }
    void set(double v) const
    {
        if (field) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", v);
            field->setText(buf);
        }
        if (stepper)
            stepper->value = v;
    }
    void setEnabled(bool e) const
    {
        if (field) {
            field->setEnabled(e);
            field->setNeedsDisplay();
        }
        if (stepper) {
            stepper->setEnabled(e);
            stepper->setNeedsDisplay();
        }
    }
};

struct SpacingSheet {
    std::unique_ptr<Nib> nib;
    Window *window = nullptr;
    TextView *text = nullptr;
    std::function<void()> changed;
    Number multiple, minimum, maximum, lineSpacing, before, after;
    RadioGroup *minMatrix = nullptr;
    CheckBox *maxButton = nullptr;
    double fontLineHeight = 0;

    bool exactly() const { return minMatrix && minMatrix->selected() == 0; }
    void setMaxEnabled(bool on)
    {
        maximum.setEnabled(on);
        if (!on)
            maximum.set(0);
    }
};

std::unique_ptr<SpacingSheet> s_spacing;

void endSpacing()
{
    if (!s_spacing)
        return;
    Window *w = s_spacing->window;
    SpacingSheet *gone = s_spacing.release();
    Sheet::end(w);
    App::shared().later([gone] { delete gone; });
}

} // namespace

void TextRulerOptions::setSpacing(Window *window, TextView *text, std::function<void()> changed)
{
    if (!window || !text || Sheet::isOpen(window))
        return;
    auto st = std::make_unique<SpacingSheet>();
    st->nib = std::make_unique<Nib>();
    const std::string dir = RulerOptionsNib;
    if (!st->nib->load(Assets::path(dir + "/keyedobjects.nib"), Assets::path(dir)))
        return;
    const int object = st->nib->outletObject("spacingPanel");
    if (object < 0)
        return;
    Size size;
    View *content = st->nib->instantiateWindow(object, &size);
    if (!content)
        return;
    Nib &nib = *st->nib;
    st->window = window;
    st->text = text;
    st->changed = std::move(changed);
    auto number = [&nib](const char *field, const char *stepper) {
        return Number{nib.outlet<TextField>(field), nib.outlet<Stepper>(stepper)};
    };
    st->multiple = number("multipleField", "multipleStepper");
    st->minimum = number("minField", "minStepper");
    st->maximum = number("maxField", "maxStepper");
    st->lineSpacing = number("lineSpacingField", "lineSpacingStepper");
    st->before = number("spaceBeforeField", "spaceBeforeStepper");
    st->after = number("spaceAfterField", "spaceAfterStepper");
    st->minMatrix = nib.outlet<RadioGroup>("minMatrix");
    st->maxButton = nib.outlet<CheckBox>("maxButton");

    // -updateLineSpacingUI: the selection's paragraph style
    const Style sel = text->styleAtSelection();
    const ParagraphStyle &ps = sel.para;
    st->fontLineHeight = defaultLineHeight(sel);
    st->multiple.set(ps.lineHeightMultiple != 0 ? ps.lineHeightMultiple : 1);
    st->minimum.set(ps.minimumLineHeight);
    st->maximum.set(ps.maximumLineHeight);
    st->lineSpacing.set(ps.lineSpacing);
    st->before.set(ps.paragraphSpacingBefore);
    st->after.set(ps.paragraphSpacing);
    if (ps.minimumLineHeight > 0 && ps.minimumLineHeight == ps.maximumLineHeight) {
        if (st->minMatrix)
            st->minMatrix->select(0);
        if (st->maxButton) {
            st->maxButton->setEnabled(false);
            st->maxButton->setChecked(false);
        }
        st->setMaxEnabled(false);
    } else {
        if (st->minMatrix)
            st->minMatrix->select(1);
        const bool hasMax = ps.maximumLineHeight > 0;
        if (st->maxButton) {
            st->maxButton->setEnabled(true);
            st->maxButton->setChecked(hasMax);
        }
        st->setMaxEnabled(hasMax);
    }

    // -modifyOptionsViaPanel:, by the control that sent it
    SpacingSheet *s = st.get();
    auto modifyMultiple = [s](double v) {
        if (s_spacing.get() != s)
            return;
        s->multiple.set(v > 0 ? v : 1);
    };
    auto modifyMinimum = [s](double v) {
        if (s_spacing.get() != s)
            return;
        v = std::max(v, 0.0);
        s->minimum.set(v);
        if (s->maximum.field && s->maximum.field->isEnabled() && v > s->maximum.get())
            s->maximum.set(v);
    };
    auto modifyMaximum = [s](double v) {
        if (s_spacing.get() != s)
            return;
        v = std::max(v, 0.0);
        s->maximum.set(v);
        if (s->minimum.get() > v)
            s->minimum.set(v);
    };
    auto modifyPlain = [s](const Number &n) {
        return [s, n](double v) {
            if (s_spacing.get() == s)
                n.set(v);
        };
    };
    auto connect = [](const Number &n, std::function<void(double)> modify) {
        if (n.stepper)
            n.stepper->onChange = modify;
        if (n.field) {
            TextField *f = n.field;
            f->onEnter = [f, modify] { modify(std::strtod(f->text.c_str(), nullptr)); };
        }
    };
    connect(s->multiple, modifyMultiple);
    connect(s->minimum, modifyMinimum);
    connect(s->maximum, modifyMaximum);
    connect(s->lineSpacing, modifyPlain(s->lineSpacing));
    connect(s->before, modifyPlain(s->before));
    connect(s->after, modifyPlain(s->after));
    if (s->minMatrix)
        s->minMatrix->onSelect = [s](int row) {
            if (s_spacing.get() != s)
                return;
            if (row == 0) {
                // Exactly: no maximum; the minimum the font's line height when it had none
                if (s->maxButton) {
                    s->maxButton->setEnabled(false);
                    s->maxButton->setChecked(false);
                }
                s->setMaxEnabled(false);
                if (s->minimum.get() == 0)
                    s->minimum.set(s->fontLineHeight);
            } else if (s->maxButton) {
                s->maxButton->setEnabled(true);
            }
        };
    if (s->maxButton)
        s->maxButton->onToggle = [s](CheckBox *b) {
            if (s_spacing.get() != s)
                return;
            s->setMaxEnabled(b->checked());
            if (b->checked())
                s->maximum.set(std::max(s->minimum.get(), s->fontLineHeight));
        };

    for (View *v : nib.withAction("confirmSheet:"))
        if (auto *b = dynamic_cast<Button *>(v))
            b->action = [s] {
                if (s_spacing.get() != s)
                    return;
                // the fields as typed (their editing ended), then the paragraphs
                const double mult = s->multiple.get() > 0 ? s->multiple.get() : 1;
                const double mn = std::max(0.0, s->minimum.get());
                const double mx = s->exactly() ? mn : (s->maxButton && s->maxButton->checked() ? std::max(mn, s->maximum.get()) : 0);
                const double ls = s->lineSpacing.get(), sb = std::max(0.0, s->before.get()), sa = std::max(0.0, s->after.get());
                TextView *t = s->text;
                auto changed = s->changed;
                endSpacing();
                t->changeParagraphStyle([=](Style &style) {
                    style.para.lineHeightMultiple = mult == 1 ? 0 : mult;
                    style.para.minimumLineHeight = mn;
                    style.para.maximumLineHeight = mx;
                    style.para.lineSpacing = ls;
                    style.para.paragraphSpacingBefore = sb;
                    style.para.paragraphSpacing = sa;
                });
                if (changed)
                    changed();
            };
    for (View *v : nib.withAction("cancelSheet:"))
        if (auto *b = dynamic_cast<Button *>(v))
            b->action = [] { endSpacing(); };

    s_spacing = std::move(st);
    Sheet::begin(window, content, size, 0, [] {
        if (s_spacing) {
            SpacingSheet *gone = s_spacing.release();
            App::shared().later([gone] { delete gone; });
        }
    });
}

} // namespace AK
