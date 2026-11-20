// SPDX-License-Identifier: LGPL-2.1-or-later
// The Equalizer, as EQWindow.nib lays it out and EQWindow (FUN_00156142 installs its handlers,
// FUN_0015680c its commands) runs it:
//   a gradient utility window (GradientWindow.nib's utility frame: a 16-point title bar, TXTS 731, the
//   buttons 13014–13016 at 5, 23, 41, a point down), its content 440 × 225 under it;
//   "On" (EQcb 5000, the command EQon, TXTS 500) at (36, 10); the presets' popup (EQpu 5001, PBUT 786:
//   three columns disabled, normal, pressed, 4 and 12 kept at its ends; TXTS 510) at (123, 6), 164 × 20,
//   its menu xMNU 513's Make Preset…, Edit List…, Manual, then the presets (STR# 600, FUN_00155694);
//   the user pane 1000 at (11, 32), 420 × 165: the preamp (EQsl 110) at (15, 9), the bands (EQsl 100 to
//   109) from 101, 31 apart, each 25 × 127 (track SlTr 3210: 21 × 122, 10 kept at each end; thumb SlTh
//   3212: three 21 × 20 cells of kind 3); their names under them (Labl 200 to 210, TXTS 501) and the
//   scale (Labl 11 to 13, TXTS 502: +12 dB, 0 dB, −12 dB).
//   A slider's value is its gain in hundredths of a dB, −1200 to 1200; vertical (UISlider: taller than
//   wide), the thumb at (len − thumb) − (v − min)(len − thumb)/(max − min) (FUN_0002c454), dragged by
//   the pointer's move times (max − min)/(len − thumb) (FUN_004691d6). Moving one (EQsc) puts the popup
//   on Manual (FUN_001557e4 with 4); a click on a band's name (EQlc) sets that band to 0, on "0 dB" all
//   eleven. The built-in presets: the table at 0xa2b520, 22 entries of 0x30 bytes (FUN_000a6d48): +0 the
//   name (STR# 600, its index), +4 the preamp, +6 the ten bands, in hundredths of a dB.
#include "equalizer.h"

#include "controls.h"
#include "foundation.h"
#include "frame.h"
#include "player.h"
#include "resources.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace AK;

namespace iTunes {

namespace {

constexpr double UtilityTitleBar = 16;
constexpr int Min = -1200, Max = 1200;

// the table at 0xa2b520: the preamp, then 32 Hz to 16 kHz (all the built-in presets' preamps are 0)
const std::array<int, 11> Presets[Equalizer::PresetCount] = {
    {0, 500, 490, 395, 105, 215, 175, 350, 410, 355, 215},        // Acoustic
    {0, 550, 425, 350, 250, 125, 0, 0, 0, 0, 0},                  // Bass Booster
    {0, -550, -425, -350, -250, -125, 0, 0, 0, 0, 0},             // Bass Reducer
    {0, 475, 375, 300, 250, -150, -150, 0, 225, 325, 375},        // Classical
    {0, 357, 655, 499, 0, 192, 365, 515, 454, 359, 0},            // Dance
    {0, 495, 355, 175, 100, 285, 250, 145, -215, -355, -460},     // Deep
    {0, 425, 380, 120, 0, -215, 225, 85, 125, 395, 480},          // Electronic
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},                            // Flat
    {0, 500, 425, 150, 300, -100, -100, 150, -50, 200, 300},      // Hip-Hop
    {0, 400, 300, 150, 225, -150, -150, 0, 150, 300, 375},        // Jazz
    {0, 450, 300, 0, 0, -150, -150, -150, 0, 300, 450},           // Latin
    {0, 600, 400, 0, 0, -200, 0, -100, -500, 500, 100},           // Loudness
    {0, -300, -150, -50, 150, 400, 250, 0, -150, 200, 100},       // Lounge
    {0, 300, 200, 0, 250, 300, 150, 350, 450, 300, 350},          // Piano
    {0, -150, -100, 0, 200, 400, 400, 200, 0, -100, -150},        // Pop
    {0, 262, 692, 565, 133, -219, -150, 232, 265, 300, 375},      // R&B
    {0, 500, 400, 300, 150, -50, -100, 50, 250, 350, 450},        // Rock
    {0, 550, 425, 350, 250, 125, 0, -125, -250, -350, -425},      // Small Speakers
    {0, -346, -47, 0, 69, 346, 461, 484, 428, 254, 0},            // Spoken Word
    {0, 0, 0, 0, 0, 0, 125, 250, 350, 425, 550},                  // Treble Booster
    {0, 0, 0, 0, 0, 0, -125, -250, -350, -425, -550},             // Treble Reducer
    {0, -150, -300, -300, 150, 375, 375, 300, 150, 0, -150},      // Vocal Booster
};

AK::TextStyle txts(int id)
{
    const TextStyle s = textStyle(id);
    AK::TextStyle t;
    t.font = s.font;
    t.color = s.color;
    t.align = s.align;
    return t;
}

// A label in a TXTS: its text over its shadow #C4C4C4 a point under it; clickable (EQlc)
class EQLabel : public View {
public:
    EQLabel(std::string text, int style) : m_text(std::move(text)), m_style(style) {}
    std::function<void()> onClick;
    void draw(cairo_t *cr) override
    {
        AK::TextStyle t = txts(m_style);
        if (m_style == 501)
            t.align = Align::Center;
        drawShadowed(cr, m_text, bounds(), t, Color::rgb(0xc4, 0xc4, 0xc4));
    }
    void mouseUp(Event &e) override
    {
        if (bounds().contains(e.location) && onClick)
            onClick();
    }

private:
    std::string m_text;
    int m_style;
};

// UISlider, vertical: the track 3210 stretched between its 10-point ends, the thumb 3212
class EQSlider : public View {
public:
    int value = 0;
    std::function<void(int)> onChange;
    void draw(cairo_t *cr) override
    {
        // the track, its 10-point ends kept (the table's fixed top and bottom)
        drawCell(cr, 3210, 0, 0, Rect{0, 0, double(cellSize(3210).w), frame().h});
        const Size thumb = cellSize(3212);
        drawCell(cr, 3212, stateColumn(3, m_dragging ? 2 : isEnabled() ? 1 : 0), 0, Rect{0, thumbTop(), thumb.w, thumb.h});
    }
    void mouseDown(Event &e) override
    {
        if (!isEnabled())
            return;
        const double top = thumbTop(), th = cellSize(3212).h;
        if (e.location.y < top || e.location.y >= top + th)
            set(valueAt(e.location.y - th / 2));   // off the thumb: its middle to the point
        m_dragging = true;
        m_from = e.location.y;
        m_start = value;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        if (!m_dragging)
            return;
        // FUN_004691d6: the value less the move times (max − min) / (len − thumb)
        const double travel = std::max(1.0, frame().h - cellSize(3212).h);
        set(int(std::lround(m_start - (e.location.y - m_from) * (Max - Min) / travel)));
    }
    void mouseUp(Event &) override
    {
        m_dragging = false;
        setNeedsDisplay();
    }

private:
    double thumbTop() const
    {
        // FUN_0002c454: (v − min)(len − thumb)/(max − min), from the bottom when vertical
        const double travel = frame().h - cellSize(3212).h;
        return std::round(travel - (value - Min) * travel / (Max - Min));
    }
    int valueAt(double top) const
    {
        const double travel = std::max(1.0, frame().h - cellSize(3212).h);
        return int(std::lround(Min + (travel - top) * (Max - Min) / travel));
    }
    void set(int v)
    {
        v = std::clamp(v, Min, Max);
        if (v == value)
            return;
        value = v;
        setNeedsDisplay();
        if (onChange)
            onChange(v);
    }
    bool m_dragging = false;
    double m_from = 0;
    int m_start = 0;
};

// The presets' popup: PBUT 786 (its columns disabled, normal, pressed; 4 and 12 kept), its title in
// TXTS 510
class PresetPopup : public View {
public:
    std::string title;
    std::function<void()> onPress;
    bool pressed = false;
    void draw(cairo_t *cr) override
    {
        drawCell(cr, 786, stateColumn(3, pressed ? 2 : 1), 0, bounds());
        AK::TextStyle t = txts(510);
        t.align = Align::Left;
        drawText(cr, title, Rect{9, 0, std::max(0.0, frame().w - 9 - 16), frame().h - 1}, t);
    }
    void mouseDown(Event &) override
    {
        pressed = true;
        setNeedsDisplay();
        if (onPress)
            onPress();
        pressed = false;
        setNeedsDisplay();
    }
};

} // namespace

// The window's content: the frame (one gradient over the whole height, GradientFrameView's flag 4), the
// title bar's title and buttons, then the nib's controls 16 points down
class Equalizer::Content : public FrameBand {
public:
    Content() { m_titleBar = UtilityTitleBar; }
    std::string title;
    FrameButtons *buttons = nullptr;
    CheckBox *on = nullptr;
    PresetPopup *popup = nullptr;
    EQSlider *sliders[11] = {};   // 0 the preamp, 1 to 10 the bands
    void draw(cairo_t *cr) override
    {
        const bool key = windowIsKey();
        const double w = frame().w;
        drawGradient(cr, bounds(), key ? 197 : 233, key ? 150 : 207, int(frame().h), 0);
        const Size end = cellSize(7001);
        drawCell(cr, 7001, 0, 0, Rect{0, 0, end.w, end.h});
        drawCell(cr, 7000, 0, 0, Rect{end.w, 0, std::max(0.0, w - 2 * end.w), end.h});
        drawCell(cr, 7002, 0, 0, Rect{w - end.w, 0, end.w, end.h});
        // TXTS 731: Txtr 102 (11 points), its shadow #C4C4C4 a point under
        AK::TextStyle t = txts(102);
        t.align = Align::Center;
        t.color = key ? Color::gray(0) : Color::gray(0, 0.5);
        const Rect r{64, 1, std::max(0.0, w - 128), 14};
        if (key)
            drawShadowed(cr, title, r, t, Color::rgb(0xc4, 0xc4, 0xc4));
        else
            drawText(cr, title, r, t);
    }
};

Equalizer::Equalizer(Player &player) : m_player(player)
{
    Settings s("itunes");
    m_on = s.boolean("EQOn", false);
    m_preset = s.integer("EQPreset", 7);
    std::istringstream in(s.string("EQBands"));
    std::array<int, 11> v{};
    int n = 0;
    while (n < 11 && in >> v[size_t(n)])
        ++n;
    if (n == 11)
        m_values = v;
    else if (m_preset >= 0 && m_preset < PresetCount)
        m_values = Presets[m_preset];
}

Equalizer::~Equalizer() = default;

std::array<int, 11> Equalizer::preset(int index) { return Presets[std::clamp(index, 0, PresetCount - 1)]; }

std::string Equalizer::presetName(int index)
{
    char key[16];
    std::snprintf(key, sizeof key, "600.%03d", index + 1);
    return tr(key);
}

void Equalizer::apply()
{
    double bands[10];
    for (int i = 0; i < 10; ++i)
        bands[i] = m_values[size_t(i) + 1] / 100.0;
    m_player.setEqualizer(m_on, m_values[0] / 100.0, bands);
}

void Equalizer::save()
{
    Settings s("itunes");
    s.set("EQOn", m_on);
    s.set("EQPreset", m_preset);
    std::string v;
    for (int x : m_values)
        v += (v.empty() ? "" : " ") + std::to_string(x);
    s.set("EQBands", v);
}

bool Equalizer::isVisible() const { return m_window && m_window->isVisible(); }

void Equalizer::show()
{
    if (!m_window)
        build();
    m_window->orderFront();
}

void Equalizer::build()
{
    const double T = UtilityTitleBar;
    m_window = std::make_unique<Window>(Size{440, 225 + T}, "Equalizer");
    m_window->setBorderless();
    m_window->setRoundedBottomCorners(true);
    m_window->onClose = [this] { App::shared().later([this] { m_window.reset(); m_content = nullptr; }); };
    View *root = m_window->contentView();
    m_content = root->add<Content>();
    m_content->setFrame({0, 0, 440, 225 + T});
    m_content->title = tr("kMenuItemEQ");
    m_content->onDoubleClickTitle = [this] { m_window->miniaturize(); };

    m_content->buttons = m_content->add<FrameButtons>(true);
    m_content->buttons->setFrame({5, 1, 2 * 18 + 13, 14});
    m_content->buttons->onClick = [this](int kind) {
        if (kind == 1)
            m_window->close();
        else if (kind == 2)
            m_window->miniaturize();
        // zoom (FUN_00155bde): the window's small state, not done
    };

    // On (EQcb 5000), TXTS 500
    m_content->on = m_content->add<CheckBox>("On");
    m_content->on->font = textStyle(500).font;
    m_content->on->small = true;
    m_content->on->setChecked(m_on);
    m_content->on->setFrame({36, 10 + T, 59, 14});
    m_content->on->onToggle = [this](CheckBox *c) {
        m_on = c->checked();
        apply();
        save();
    };

    // the presets (EQpu 5001)
    m_content->popup = m_content->add<PresetPopup>();
    m_content->popup->setFrame({123, 6 + T, 164, 20});
    m_content->popup->title = m_preset >= 0 ? presetName(m_preset) : tr("kEQPresetsMenuItemManual");
    m_content->popup->onPress = [this] {
        std::vector<MenuItem> items;
        items.emplace_back(tr("kEQPresetsMenuItemMakePreset"), false, false);
        items.emplace_back(tr("kEQPresetsMenuItemEditList"), false, false);
        items.push_back(MenuItem::line());
        items.emplace_back(tr("kEQPresetsMenuItemManual"), false, true, m_preset < 0);
        items.push_back(MenuItem::line());
        for (int i = 0; i < PresetCount; ++i)
            items.emplace_back(presetName(i), false, true, i == m_preset);
        const Rect f = m_content->popup->frame();
        Menu::popUp(m_window.get(), {f.x, f.y + f.h}, items, f.w, textStyle(510).font, [this](int choice) {
            if (choice == 3) {
                m_preset = -1;
                m_content->popup->title = tr("kEQPresetsMenuItemManual");
                m_content->popup->setNeedsDisplay();
                save();
            } else if (choice >= 5) {
                choosePreset(choice - 5);
            }
        });
    };

    // the user pane 1000 at (11, 32): the preamp, the ten bands, their names, the scale
    const double px = 11, py = 32 + T;
    static const char *const names[10] = {"32", "64", "125", "250", "500", "1K", "2K", "4K", "8K", "16K"};
    static const double nameX[10] = {102, 133, 163, 194, 225, 257, 289, 319, 350, 379};
    static const double nameW[10] = {24, 24, 24, 24, 24, 24, 24, 24, 24, 24};
    for (int i = 0; i < 11; ++i) {
        const double x = i == 0 ? 15 : 101 + 31 * (i - 1);
        EQSlider *sl = m_content->add<EQSlider>();
        sl->setFrame({px + x, py + 9, 25, 127});
        sl->value = m_values[size_t(i)];
        sl->onChange = [this, i](int v) { setValue(i, v, true); };
        m_content->sliders[i] = sl;
    }
    EQLabel *pre = m_content->add<EQLabel>("Preamp", 501);
    pre->setFrame({px + 5, py + 142, 45, 13});
    for (int i = 0; i < 10; ++i) {
        EQLabel *l = m_content->add<EQLabel>(names[i], 501);
        l->setFrame({px + nameX[i], py + 142, nameW[i], 13});
        l->onClick = [this, i] { setValue(i + 1, 0, true); };   // EQlc: that band to 0
    }
    EQLabel *top = m_content->add<EQLabel>("+12 dB", 502);
    top->setFrame({px + 46, py + 10, 40, 13});
    EQLabel *zero = m_content->add<EQLabel>("0 dB", 502);
    zero->setFrame({px + 48, py + 66, 38, 13});
    zero->onClick = [this] {   // Labl 12: all eleven to 0
        for (int i = 0; i < 11; ++i)
            setValue(i, 0, true);
    };
    EQLabel *bottom = m_content->add<EQLabel>("-12 dB", 502);
    bottom->setFrame({px + 48, py + 123, 38, 13});
    m_window->show();
}

void Equalizer::setValue(int slider, int hundredths, bool manual)
{
    m_values[size_t(slider)] = hundredths;
    if (m_content && m_content->sliders[slider]->value != hundredths) {
        m_content->sliders[slider]->value = hundredths;
        m_content->sliders[slider]->setNeedsDisplay();
    }
    if (manual && m_preset >= 0) {
        m_preset = -1;
        if (m_content) {
            m_content->popup->title = tr("kEQPresetsMenuItemManual");
            m_content->popup->setNeedsDisplay();
        }
    }
    apply();
    save();
}

void Equalizer::choosePreset(int index)
{
    m_preset = index;
    const std::array<int, 11> v = Presets[index];
    for (int i = 0; i < 11; ++i)
        setValue(i, v[size_t(i)], false);
    if (m_content) {
        m_content->popup->title = presetName(index);
        m_content->popup->setNeedsDisplay();
    }
    save();
}

} // namespace iTunes
