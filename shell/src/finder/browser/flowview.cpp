// SPDX-License-Identifier: LGPL-2.1-or-later
#include "flowview.h"

#include "controls.h"
#include "text.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Finder {

namespace {

// IKImageFlowView's look, estimated: the side covers turned to show this much of their width, their
// far edge this much smaller, the gaps between them
constexpr double SideWidth = 0.38, FarScale = 0.8, FirstGap = 0.72, Gap = 0.3;
constexpr double SliderHeight = 14;

} // namespace

class CoverFlow : public View {
public:
    explicit CoverFlow(FlowView *owner) : m_owner(owner) {}
    ~CoverFlow() override { stop(); }

    void setItems(std::vector<Item> items)
    {
        m_items = std::move(items);
        m_selected = m_items.empty() ? -1 : 0;
        m_position = m_target = 0;
        setNeedsDisplay();
    }

    void setSelected(int i, bool animate = true)
    {
        if (m_items.empty())
            return;
        m_selected = std::clamp(i, 0, int(m_items.size()) - 1);
        m_target = m_selected;
        if (!animate)
            m_position = m_target;
        start();
        setNeedsDisplay();
    }
    int selected() const { return m_selected; }
    const std::vector<Item> &items() const { return m_items; }

    void draw(cairo_t *cr) override
    {
        const double W = frame().w, H = frame().h;
        cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, H);
        cairo_pattern_add_color_stop_rgb(bg, 0, 0, 0, 0);
        cairo_pattern_add_color_stop_rgb(bg, 1, 0.2, 0.2, 0.2);
        cairo_rectangle(cr, 0, 0, W, H);
        cairo_set_source(cr, bg);
        cairo_fill(cr);
        cairo_pattern_destroy(bg);
        if (m_items.empty())
            return;
        const double s = coverSize();
        const double baseline = std::floor(H * 0.62);
        // the farthest first, the centre last
        std::vector<int> order;
        const int lo = std::max(0, int(std::floor(m_position)) - 12), hi = std::min(int(m_items.size()) - 1, int(std::ceil(m_position)) + 12);
        for (int i = lo; i <= hi; ++i)
            order.push_back(i);
        std::sort(order.begin(), order.end(), [this](int a, int b) { return std::abs(a - m_position) > std::abs(b - m_position); });
        for (int i : order)
            drawCover(cr, i, s, baseline);
        // the centre item's name and kind
        if (m_selected >= 0) {
            const Item &item = m_items[size_t(m_selected)];
            TextStyle t;
            t.font = Font::boldSystem(12);
            t.align = Align::Center;
            t.color = Color::gray(255);
            const double ty = std::min(H - SliderHeight - 44, baseline + s * 0.25 + 6);
            drawText(cr, item.name, Rect{20, ty, W - 40, 16}, t);
            t.font = Font::system(11);
            t.color = Color::gray(170);
            drawText(cr, kind(item), Rect{20, ty + 16, W - 40, 15}, t);
        }
        drawSlider(cr);
    }

    void mouseDown(Event &e) override
    {
        if (window())
            m_owner->focus();
        if (sliderRect().contains(e.location)) {
            m_sliding = true;
            slideTo(e.location.x);
            return;
        }
        // the cover under the click: the centre one opens on a double-click
        const int i = coverAt(e.location);
        if (i < 0)
            return;
        if (i == m_selected && e.clickCount == 2) {
            if (m_owner->onOpen)
                m_owner->onOpen(m_items[size_t(i)]);
            return;
        }
        choose(i);
    }

    void mouseDragged(Event &e) override
    {
        if (m_sliding)
            slideTo(e.location.x);
    }

    void mouseUp(Event &) override { m_sliding = false; }

    void scrollWheel(Event &e) override { choose(m_selected + (e.deltaY > 0 ? 1 : -1)); }

    void keyDown(Event &e) override
    {
        if (e.keysym == XKB_KEY_Right || e.keysym == XKB_KEY_Down)
            choose(m_selected + 1);
        else if (e.keysym == XKB_KEY_Left || e.keysym == XKB_KEY_Up)
            choose(m_selected - 1);
        else if ((e.modifiers & Command) && (e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_o) && m_selected >= 0 && m_owner->onOpen)
            m_owner->onOpen(m_items[size_t(m_selected)]);
        else
            View::keyDown(e);
    }

    bool acceptsFirstResponder() const override { return true; }

private:
    double coverSize() const { return std::floor(std::min(frame().h * 0.5, frame().w * 0.32)); }

    // a cover's centre across and how far it is turned (0 facing, 1 on its side), d items from the centre
    void place(double d, double s, double &cx, double &turn) const
    {
        const double a = std::abs(d);
        turn = std::min(1.0, a);
        const double off = a < 1 ? a * s * FirstGap : s * FirstGap + (a - 1) * s * Gap;
        cx = frame().w / 2 + (d < 0 ? -off : off);
    }

    void drawCover(cairo_t *cr, int i, double s, double baseline)
    {
        const Item &item = m_items[size_t(i)];
        const Image img = icon(item, 256);
        if (img.isNull())
            return;
        const Size fit = scaledToFit(img.size(), {s, s});
        double cx, turn;
        const double d = i - m_position;
        place(d, s, cx, turn);
        const double width = fit.w * (1 - turn * (1 - SideWidth));
        const double x0 = cx - width / 2;
        const int strips = turn < 0.01 ? 1 : 32;
        for (int k = 0; k < strips; ++k) {
            const double u = (k + 0.5) / strips;
            // the near edge (towards the centre) full size, the far edge smaller
            const double nearness = d < 0 ? u : 1 - u;
            const double scale = 1 - turn * (1 - FarScale) * (1 - nearness);
            const double h = fit.h * scale;
            const Rect src{img.width() * double(k) / strips, 0, double(img.width()) / strips, double(img.height())};
            const Rect dst{x0 + width * double(k) / strips, baseline - h, width / strips + 0.5, h};   // on the floor
            const double dim = 1 - turn * 0.25;   // the turned covers darker
            img.draw(cr, src, dst, dim);
            // the reflection: the strip upside down under the floor, fading
            cairo_save(cr);
            cairo_rectangle(cr, dst.x, baseline, dst.w, h * 0.4);
            cairo_clip(cr);
            cairo_translate(cr, 0, 2 * baseline);
            cairo_scale(cr, 1, -1);
            cairo_push_group(cr);
            img.draw(cr, src, dst, 1);
            cairo_pattern_t *fade = cairo_pattern_create_linear(0, baseline, 0, baseline - h * 0.4);
            cairo_pattern_add_color_stop_rgba(fade, 0, 0, 0, 0, 0.35 * dim);
            cairo_pattern_add_color_stop_rgba(fade, 1, 0, 0, 0, 0);
            cairo_pop_group_to_source(cr);
            cairo_mask(cr, fade);
            cairo_pattern_destroy(fade);
            cairo_restore(cr);
        }
    }

    int coverAt(Point p) const
    {
        // the nearest to the centre first: they are drawn over the others
        const double s = coverSize(), baseline = std::floor(frame().h * 0.62);
        int best = -1;
        double bestDistance = 1e9;
        for (int i = 0; i < int(m_items.size()); ++i) {
            double cx, turn;
            place(i - m_position, s, cx, turn);
            const double w = s * (1 - turn * (1 - SideWidth));
            if (p.x >= cx - w / 2 && p.x <= cx + w / 2 && p.y >= baseline - s && p.y <= baseline && std::abs(i - m_position) < bestDistance) {
                best = i;
                bestDistance = std::abs(i - m_position);
            }
        }
        return best;
    }

    Rect sliderRect() const
    {
        const double w = std::floor(frame().w * 0.5);
        return {std::floor((frame().w - w) / 2), frame().h - SliderHeight - 10, w, SliderHeight};
    }

    void drawSlider(cairo_t *cr)
    {
        if (m_items.size() < 2)
            return;
        const Rect r = sliderRect();
        auto pill = [cr](const Rect &p) {
            const double rad = p.h / 2;
            cairo_new_sub_path(cr);
            cairo_arc(cr, p.x + rad, p.y + rad, rad, M_PI / 2, 3 * M_PI / 2);
            cairo_arc(cr, p.right() - rad, p.y + rad, rad, -M_PI / 2, M_PI / 2);
            cairo_close_path(cr);
        };
        pill(r);
        cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, 1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.45, 0.45, 0.45, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        const double kw = std::max(30.0, r.w / m_items.size());
        const double t = m_position / (m_items.size() - 1);
        const Rect knob{r.x + 1 + t * (r.w - kw - 2), r.y + 1, kw, r.h - 2};
        pill(knob);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, knob.y, 0, knob.bottom());
        cairo_pattern_add_color_stop_rgb(g, 0, 0.62, 0.62, 0.62);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.33, 0.33, 0.33);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    void slideTo(double x)
    {
        const Rect r = sliderRect();
        const double t = std::clamp((x - r.x) / r.w, 0.0, 1.0);
        choose(int(std::lround(t * (m_items.size() - 1))));
    }

    void choose(int i)
    {
        if (m_items.empty())
            return;
        setSelected(i);
        m_owner->m_list->selectPath(m_items[size_t(m_selected)].path);
        if (m_owner->onSelectionChanged)
            m_owner->onSelectionChanged();
    }

    // the covers glide to the selection
    void start()
    {
        if (m_timer)
            return;
        m_timer = App::shared().addTimer(1.0 / 60, true, [this] {
            m_position += (m_target - m_position) * 0.22;
            if (std::abs(m_target - m_position) < 0.005) {
                m_position = m_target;
                stop();
            }
            setNeedsDisplay();
        });
    }

    void stop()
    {
        if (m_timer)
            App::shared().removeTimer(m_timer);
        m_timer = 0;
    }

    FlowView *m_owner;
    std::vector<Item> m_items;
    int m_selected = -1;
    double m_position = 0, m_target = 0;
    int m_timer = 0;
    bool m_sliding = false;
};

// ---- FlowView

FlowView::FlowView()
{
    m_flow = add<CoverFlow>(this);
    m_list = add<ListView>();
    m_list->onOpen = [this](const Item &item) {
        if (onOpen)
            onOpen(item);
    };
    m_list->onSelectionChanged = [this] {
        syncFromList();
        if (onSelectionChanged)
            onSelectionChanged();
    };
}

void FlowView::setItems(std::vector<Item> items)
{
    m_list->setItems(std::move(items));
    m_flow->setItems(m_list->rowItems());
}

void FlowView::syncFromList()
{
    // the flow shows the list's rows (open folders' items too) and follows its selection
    const std::vector<Item> rows = m_list->rowItems();
    if (rows.size() != m_flow->items().size())
        m_flow->setItems(rows);
    const std::vector<Item> sel = m_list->selectedItems();
    if (sel.empty())
        return;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].path == sel.back().path) {
            m_flow->setSelected(int(i));
            break;
        }
}

void FlowView::setFrame(const Rect &r)
{
    View::setFrame(r);
    layout();
}

void FlowView::layout()
{
    if (m_flowHeight < 0)
        m_flowHeight = std::floor(frame().h * 2 / 3);
    const double flow = std::clamp(m_flowHeight, std::min(FlowMinHeight, frame().h), std::max(FlowMinHeight, frame().h - 60));
    m_flow->setFrame({0, 0, frame().w, flow});
    m_list->setFrame({0, flow, frame().w, frame().h - flow});
}

void FlowView::focus()
{
    if (window())
        window()->makeFirstResponder(m_flow);
}

} // namespace Finder
