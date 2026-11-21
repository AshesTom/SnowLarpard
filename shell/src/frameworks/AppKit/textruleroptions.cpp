// SPDX-License-Identifier: LGPL-2.1-or-later
#include "textruleroptions.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
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

struct LinkSheet {
    std::unique_ptr<Nib> nib;
    Window *window = nullptr;
    TextView *text = nullptr;
    TextField *field = nullptr;
};

std::unique_ptr<LinkSheet> s_link;

std::string trimmed(std::string s)
{
    const char *ws = " \t\n\r";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos)
        return {};
    s.erase(0, a);
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

bool startsWith(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }
bool endsWith(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

void endLink(bool ok)
{
    if (!s_link)
        return;
    LinkSheet *gone = s_link.release();
    const std::string typed = gone->field ? trimmed(gone->field->text) : std::string();
    TextView *t = gone->text;
    Sheet::end(gone->window);
    App::shared().later([gone] { delete gone; });
    if (!ok || !t)
        return;
    // -sheetDidEnd:returnCode:contextInfo:: NSLink added ("Edit Link") or removed ("Remove Link")
    const std::string url = typed.empty() ? std::string() : TextRulerOptions::urlForString(typed);
    t->changeStyle([url](Style &s) { s.link = url; });
}

} // namespace

std::string TextRulerOptions::urlForString(const std::string &in)
{
    std::string s = trimmed(in);
    if (s.find(':') == std::string::npos && !startsWith(s, ".")) {
        const std::string host = s.substr(0, s.find('/'));
        if (!host.empty() && (startsWith(host, "www.") || startsWith(host, "web.") || startsWith(host, "ftp.") ||
                              endsWith(host, ".com") || endsWith(host, ".net") || endsWith(host, ".org") || endsWith(host, ".gov") ||
                              endsWith(host, ".edu") || endsWith(host, ".mil")))
            s = "http://" + s;
    }
    // URLWithString: refuses spaces and the like: percent escapes for them
    std::string out;
    for (unsigned char c : s) {
        if (c <= ' ' || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}' || c >= 0x7f) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        } else {
            out += char(c);
        }
    }
    return out;
}

void TextRulerOptions::setLink(Window *window, TextView *text)
{
    if (!window || !text || Sheet::isOpen(window))
        return;
    auto st = std::make_unique<LinkSheet>();
    st->nib = std::make_unique<Nib>();
    const std::string dir = RulerOptionsNib;
    if (!st->nib->load(Assets::path(dir + "/keyedobjects.nib"), Assets::path(dir)))
        return;
    const int object = st->nib->outletObject("linkPanel");
    if (object < 0)
        return;
    Size size;
    View *content = st->nib->instantiateWindow(object, &size);
    if (!content)
        return;
    Nib &nib = *st->nib;
    st->window = window;
    st->text = text;
    st->field = nib.outlet<TextField>("linkField");

    // -orderFrontLinkPanel:: the link where the selection is (all of it selected), else the text
    const RichText &t = text->text();
    size_t a = text->selectionStart(), b = text->selectionEnd();
    std::string value;
    const size_t probe = a < t.size() ? a : (a > 0 ? a - 1 : a);
    if (probe < t.size() && !t.styleAt(probe).link.empty()) {
        value = t.styleAt(probe).link;
        if (a == b) {
            while (a > 0 && t.styleAt(a - 1).link == value)
                --a;
            while (b < t.size() && t.styleAt(b).link == value)
                ++b;
            text->select(a, b);
        }
        // shown unescaped (stringByReplacingPercentEscapesUsingEncoding:)
        std::string plain;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size() && std::isxdigit((unsigned char)value[i + 1]) && std::isxdigit((unsigned char)value[i + 2])) {
                plain += char(std::strtol(value.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            } else {
                plain += value[i];
            }
        }
        value = plain;
    } else if (a != b) {
        std::string sel;
        for (size_t i = a; i < b; ++i)
            if (t.text.compare(i, 3, AttachmentCharacter) == 0)
                i += 2;
            else
                sel += t.text[i];
        value = trimmed(sel);
    }
    if (st->field) {
        st->field->setText(value);
        st->field->selectAll();
    }
    // the label's text is an attributed string with an RTF line break left in it: its first line
    std::function<void(View *)> fixLabels = [&fixLabels](View *v) {
        if (auto *l = dynamic_cast<Label *>(v)) {
            const std::string t = l->text();
            if (t.find('\n') != std::string::npos)
                l->setText(t.substr(0, t.find('\n')));
        }
        for (View *c : v->subviews())
            fixLabels(c);
    };
    fixLabels(content);
    LinkSheet *s = st.get();
    for (View *v : nib.withAction("confirmSheet:"))
        if (auto *btn = dynamic_cast<Button *>(v)) {
            btn->isDefault = true;
            btn->action = [s] {
                if (s_link.get() == s)
                    endLink(true);
            };
        }
    for (View *v : nib.withAction("cancelSheet:"))
        if (auto *btn = dynamic_cast<Button *>(v))
            btn->action = [s] {
                if (s_link.get() == s)
                    endLink(false);
            };
    for (View *v : nib.withAction("removeLink:"))
        if (auto *btn = dynamic_cast<Button *>(v))
            btn->action = [s] {
                if (s_link.get() != s)
                    return;
                if (s->field)
                    s->field->setText("");
                endLink(true);
            };
    if (st->field)
        st->field->onEnter = [s] {
            if (s_link.get() == s)
                endLink(true);
        };

    s_link = std::move(st);
    Sheet::begin(window, content, size, 0, [] {
        if (s_link) {
            LinkSheet *gone = s_link.release();
            App::shared().later([gone] { delete gone; });
        }
    });
    if (s->field)
        window->makeFirstResponder(s->field);
}

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
