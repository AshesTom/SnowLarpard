// SPDX-License-Identifier: LGPL-2.1-or-later
#include "colorpanel.h"

#include "controls.h"
#include "foundation.h"
#include "toolbar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

namespace AK {

namespace {

const char *const AppKitBundle = "/System/Library/Frameworks/AppKit.framework/Versions/C";
const char *const WheelBundle = "/System/Library/ColorPickers/NSColorPickerWheel.colorPicker/Resources";

std::string cp(const std::string &key) { return Assets::tr(AppKitBundle, "NSColorPanel", key, key); }

Image picture(const std::string &name)
{
    return Image::load(Assets::path(std::string(AppKitBundle) + "/Resources/Images/" + name + ".png"));
}

// NSColor's HSB, as -hueComponent, -saturationComponent, -brightnessComponent give them
struct HSB {
    double h = 0, s = 0, b = 1;
};

HSB toHSB(const Color &c)
{
    const double mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    HSB o;
    o.b = mx;
    o.s = mx > 0 ? d / mx : 0;
    if (d > 0) {
        double h = mx == c.r ? (c.g - c.b) / d : mx == c.g ? 2 + (c.b - c.r) / d : 4 + (c.r - c.g) / d;
        h /= 6;
        o.h = h < 0 ? h + 1 : h;
    }
    return o;
}

Color fromHSB(const HSB &x, double alpha)
{
    const double h = std::fmod(x.h, 1.0) * 6, s = x.s, v = x.b;
    const int i = int(std::floor(h)) % 6;
    const double f = h - std::floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    Color c;
    switch (i) {
    case 0: c = {v, t, p, alpha}; break;
    case 1: c = {q, v, p, alpha}; break;
    case 2: c = {p, v, t, alpha}; break;
    case 3: c = {p, q, v, alpha}; break;
    case 4: c = {t, p, v, alpha}; break;
    default: c = {v, p, q, alpha}; break;
    }
    return c;
}

struct State;
State &state();

// NSColorPickerWheelView: the wheel in the largest square, darkened by the brightness
class WheelView : public View {
public:
    double hue = 0, saturation = 0, brightness = 1;
    std::function<void()> onChange;
    Rect square() const
    {
        const double side = std::min(frame().w, frame().h);
        return {std::floor((frame().w - side) / 2), std::floor((frame().h - side) / 2), side, side};
    }
    void draw(cairo_t *cr) override
    {
        static const Image wheel = Image::load(Assets::path(std::string(WheelBundle) + "/NSColorWheelImage.tiff"));
        const Rect sq = square();
        const double cx = sq.x + sq.w / 2, cy = sq.y + sq.h / 2, r = sq.w / 2;
        if (!wheel.isNull()) {
            // the picture's disc (its corners are not part of it)
            cairo_save(cr);
            cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
            cairo_clip(cr);
            wheel.draw(cr, sq);
            cairo_restore(cr);
        }
        // black over the disc at 1 − the brightness
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, 1 - brightness);
        cairo_fill(cr);
        // the current point: a small square, white then black
        const double a = hue * 2 * M_PI, d = saturation * r;
        const double px = std::round(cx + std::cos(a) * d), py = std::round(cy - std::sin(a) * d);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, px - 2.5, py - 2.5, 5, 5);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_stroke(cr);
        cairo_rectangle(cr, px - 3.5, py - 3.5, 7, 7);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_stroke(cr);
    }
    void mouseDown(Event &e) override { track(e.location); }
    void mouseDragged(Event &e) override { track(e.location); }

private:
    void track(Point p)
    {
        const Rect sq = square();
        const double cx = sq.x + sq.w / 2, cy = sq.y + sq.h / 2, r = sq.w / 2;
        const double dx = p.x - cx, dy = cy - p.y;
        double a = std::atan2(dy, dx) / (2 * M_PI);
        hue = a < 0 ? a + 1 : a;
        saturation = std::min(1.0, std::hypot(dx, dy) / r);
        setNeedsDisplay();
        if (onChange)
            onChange();
    }
};

// NSColorScaleSlider: vertical, its track the colour from black (bottom) to full brightness (top)
class BrightnessSlider : public View {
public:
    Color full{1, 1, 1, 1};
    double value = 1;
    std::function<void()> onChange;
    void draw(cairo_t *cr) override
    {
        const Rect t{std::floor(frame().w / 2) - 4, 5, 8, frame().h - 10};
        cairo_pattern_t *g = cairo_pattern_create_linear(0, t.y, 0, t.bottom());
        cairo_pattern_add_color_stop_rgb(g, 0, full.r, full.g, full.b);
        cairo_pattern_add_color_stop_rgb(g, 1, 0, 0, 0);
        cairo_rectangle(cr, t.x, t.y, t.w, t.h);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        cairo_rectangle(cr, t.x + 0.5, t.y + 0.5, t.w - 1, t.h - 1);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        // the knob: a bar across the track
        const double y = std::round(t.y + (1 - value) * t.h);
        const Rect k{1, y - 3, frame().w - 2, 6};
        cairo_rectangle(cr, k.x + 0.5, k.y + 0.5, k.w - 1, k.h - 1);
        cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_stroke(cr);
    }
    void mouseDown(Event &e) override { track(e.location.y); }
    void mouseDragged(Event &e) override { track(e.location.y); }

private:
    void track(double y)
    {
        value = std::clamp(1 - (y - 5) / (frame().h - 10), 0.0, 1.0);
        setNeedsDisplay();
        if (onChange)
            onChange();
    }
};

// NSColorSwatch: one row of chips 13 points apart (12 × 12, a grey edge), the colours kept
class Swatches : public View {
public:
    std::vector<std::optional<Color>> chips;
    std::function<void(const Color &)> onPick;
    int count() const { return int(frame().w / 13); }
    void draw(cairo_t *cr) override
    {
        for (int i = 0; i < count(); ++i) {
            const Rect r{i * 13.0, 2, 12, 12};
            fill(cr, r, Color::gray(255));
            if (size_t(i) < chips.size() && chips[size_t(i)])
                fill(cr, r.inset(1, 1), *chips[size_t(i)]);
            cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
    }
    void mouseDown(Event &e) override
    {
        const int i = int(e.location.x / 13);
        if (i >= 0 && size_t(i) < chips.size() && chips[size_t(i)] && onPick)
            onPick(*chips[size_t(i)]);
    }
};

// the toolbar's pickers: their pictures, the one shown pressed
class PickerButton : public Control {
public:
    Image image;
    bool selected = false;
    void draw(cairo_t *cr) override
    {
        if (selected || m_pressed) {
            const Rect r = bounds().inset(1, 1);
            cairo_new_path(cr);
            const double rad = 4;
            cairo_arc(cr, r.x + rad, r.y + rad, rad, M_PI, 1.5 * M_PI);
            cairo_arc(cr, r.right() - rad, r.y + rad, rad, 1.5 * M_PI, 2 * M_PI);
            cairo_arc(cr, r.right() - rad, r.bottom() - rad, rad, 0, 0.5 * M_PI);
            cairo_arc(cr, r.x + rad, r.bottom() - rad, rad, 0.5 * M_PI, M_PI);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, m_pressed ? 0.28 : 0.18);
            cairo_fill(cr);
        }
        if (!image.isNull())
            image.draw(cr, Point{std::floor((frame().w - image.width()) / 2), std::floor((frame().h - image.height()) / 2)},
                       isEnabled() ? 1 : 0.4);
    }
};

struct State {
    std::unique_ptr<Window> window;
    std::function<void(const Color &)> target;
    Color color{0, 0, 0, 1};
    ColorWell *well = nullptr;
    WheelView *wheel = nullptr;
    BrightnessSlider *brightness = nullptr;
    Slider *opacity = nullptr;
    TextField *opacityField = nullptr;
    Swatches *swatches = nullptr;

    void show()
    {
        const HSB x = toHSB(color);
        if (wheel) {
            if (x.s > 0 && x.b > 0)
                wheel->hue = x.h;
            wheel->saturation = x.s;
            wheel->brightness = x.b;
            wheel->setNeedsDisplay();
        }
        if (brightness) {
            brightness->value = x.b;
            brightness->full = fromHSB({wheel ? wheel->hue : x.h, x.s, 1}, 1);
            brightness->setNeedsDisplay();
        }
        if (well) {
            well->color = color;
            well->setNeedsDisplay();
        }
        if (opacity)
            opacity->setDoubleValue(color.a * 100);
        if (opacityField)
            opacityField->setText(std::to_string(int(std::lround(color.a * 100))));
    }

    // the picker's colour to the panel and on to the target (-[NSColorPanel setColor:], its action)
    void changed(const Color &c)
    {
        color = c;
        show();
        if (target)
            target(color);
    }

    void build()
    {
        // NSColorPanel.nib: 205 × 322, under an icon-only toolbar
        auto *toolbar = new Toolbar(false);
        const double tb = toolbar->height();
        window = std::make_unique<Window>(Size{205, 322 + tb}, cp("Colors") == "Colors" ? "Colors" : cp("Colors"));
        window->setFloatingPanel(true);
        View *c = window->contentView();
        c->addSubview(toolbar);
        const char *pickers[][2] = {{"NSColorPickerWheel", "Color Wheel"}, {"NSColorPickerSliders", "Color Sliders"},
                                    {"NSColorPickerList", "Color Palettes"}, {"NSColorPickerUser", "Image Palettes"},
                                    {"NSColorPickerCrayon", "Crayons"}};
        for (const auto &p : pickers) {
            auto *b = new PickerButton;
            b->image = picture(p[0]);
            b->selected = std::string(p[0]) == "NSColorPickerWheel";
            b->setEnabled(b->selected);   // the wheel alone here
            toolbar->addItem(p[0], cp(p[1]), b, 30);   // five in the panel's 205 points
        }
        toolbar->setFrame({0, 0, 205, tb});
        View *body = c->add<View>();
        body->setFrame({0, tb, 205, 322});

        // the magnifier (picking a colour from the screen: not here) and the colour well
        auto *magnify = body->add<ImageView>();
        magnify->image = picture("NSSmallMagnifyingGlass");
        magnify->setFrame({3, 7, 23, 23});
        well = body->add<ColorWell>();
        well->setFrame({28, 4, 170, 26});

        // NSColorPickerWheel.nib's view in the picker box (its top 31 points down)
        auto *bezel = body->add<Box>();
        bezel->kind = Box::Secondary;
        bezel->setFrame({6, 37, 171, 194});
        wheel = body->add<WheelView>();
        wheel->setFrame({7, 38, 169, 192});
        brightness = body->add<BrightnessSlider>();
        brightness->setFrame({180, 36, 20, 196});

        // Opacity
        auto *label = body->add<Label>(cp("Opacity") == "Opacity" ? "Opacity" : cp("Opacity"), Font::system(11));
        label->setFrame({8, 243, 72, 14});
        opacity = body->add<Slider>();
        opacity->small = true;
        opacity->minValue = 0;
        opacity->maxValue = 100;
        opacity->setFrame({6, 257, 140, 18});
        opacityField = body->add<TextField>();
        opacityField->font = Font::system(11);
        opacityField->setFrame({153, 253, 34, 19});
        auto *percent = body->add<Label>("%", Font::system(11));
        percent->setFrame({189, 256, 14, 14});
        auto *sep = body->add<Box>();
        sep->kind = Box::Separator;
        sep->setFrame({0, 276, 205, 1});

        // the swatches
        swatches = body->add<Swatches>();
        swatches->setFrame({8, 285, 189, 16});
        Settings conf("colorpanel");
        for (const std::string &v : conf.list("Swatches")) {
            double r, g, b;
            if (std::sscanf(v.c_str(), "%lf %lf %lf", &r, &g, &b) == 3)
                swatches->chips.push_back(Color{r, g, b, 1});
            else
                swatches->chips.push_back(std::nullopt);
        }
        auto *dimple = body->add<ImageView>();
        dimple->image = picture("NSColorSwatchResizeDimple");
        dimple->setFrame({93, 312, 8, 8});

        State *s = this;
        wheel->onChange = [s] {
            s->changed(fromHSB({s->wheel->hue, s->wheel->saturation, s->brightness->value}, s->color.a));
        };
        brightness->onChange = [s] {
            s->wheel->brightness = s->brightness->value;
            s->changed(fromHSB({s->wheel->hue, s->wheel->saturation, s->brightness->value}, s->color.a));
        };
        opacity->onChange = [s](double) {
            Color c = s->color;
            c.a = std::round(s->opacity->doubleValue()) / 100;
            s->changed(c);
        };
        opacityField->onEnter = [s] {
            Color c = s->color;
            c.a = std::clamp(std::strtod(s->opacityField->text.c_str(), nullptr), 0.0, 100.0) / 100;
            s->changed(c);
        };
        swatches->onPick = [s](const Color &c) {
            Color k = c;
            k.a = s->color.a;
            s->changed(k);
        };
        // the well onto a chip (a drag; here a click on the well then on a chip): the well's colour kept
        well->action = [s] {
            std::vector<std::string> list;
            auto &chips = s->swatches->chips;
            size_t at = 0;
            while (at < chips.size() && chips[at])
                ++at;
            if (at >= size_t(s->swatches->count()))
                at = 0, chips.erase(chips.begin());
            if (at >= chips.size())
                chips.resize(at + 1);
            chips[at] = s->color;
            for (const auto &k : chips) {
                char buf[64];
                if (k)
                    std::snprintf(buf, sizeof buf, "%.4f %.4f %.4f", k->r, k->g, k->b);
                list.push_back(k ? buf : "");
            }
            Settings("colorpanel").set("Swatches", list);
            s->swatches->setNeedsDisplay();
        };
        show();
    }
};

State &state()
{
    static State s;
    return s;
}

} // namespace

void ColorPanel::show()
{
    State &s = state();
    if (!s.window)
        s.build();
    s.show();
    s.window->orderFront();
}

bool ColorPanel::isVisible() { return state().window && state().window->isVisible(); }

Color ColorPanel::color() { return state().color; }

void ColorPanel::setColor(const Color &c)
{
    State &s = state();
    s.color = c;
    if (s.window)
        s.show();
}

void ColorPanel::setTarget(std::function<void(const Color &)> changed) { state().target = std::move(changed); }

} // namespace AK
