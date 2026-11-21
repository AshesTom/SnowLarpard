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
// The presets (the list at +0x1a9c4, after a head node the Edit List leaves out): kept in their names'
// order, the case ignored (FUN_000a5d60 inserts each after the last whose name sorts before); the
// popup's commands (FUN_00155310): mpre Make Preset… (FUN_000bd8b2 → FUN_000a79e0 with the current
// preset's name), edeq Edit List… (FUN_000a7d12), sleq a preset.
//   Make Preset / Rename (FUN_000a79e0): Get String (DLOG 10002) titled STR# 177.009 "Make Preset" or
//   177.011 "Rename", its prompt 177.010 "New Preset Name:", the field the preset's name; an empty name
//   does nothing; a name another preset has asks 177.006 (Yes, No: 23987.001 / .002; "Do not warn me
//   again", 23987.020, kept at +0x9f0) and deletes that one without asking (FUN_000a78b0 with 1); a new
//   preset takes the sliders' values and becomes the one chosen (FUN_000a770a, FUN_000a6b36).
//   Edit List (DLOG 148, "Edit Presets", 350 × 284): the List Manager's list in user item 2 (20, 10,
//   200 × 254, inset 1, a 15-point scroll bar at its right; one row chosen at a time: flags 0x8C),
//   Done (1), Rename… (3) and Delete (4) enabled when a row is chosen (FUN_000a639c).
//   Delete (FUN_000a78b0): asks 177.004 (Delete, Cancel: 23987.015 / .004; "Do not warn me again" kept
//   at +0x9ed); the preset chosen goes Manual with its values (FUN_000a783c). 177.005 and 177.008, the
//   questions about the songs set to a preset, are not asked: the port's songs have no preset of their
//   own.
#include "equalizer.h"

#include "controls.h"
#include "panels.h"
#include "foundation.h"
#include "frame.h"
#include "player.h"
#include "resources.h"

#include <algorithm>
#include <cctype>
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
    m_preset = -1;
    // the presets kept ("name<TAB>preamp band…"), else the built-in ones
    for (const std::string &line : s.list("EQPresets")) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        Preset p{line.substr(0, tab), {}};
        std::istringstream in(line.substr(tab + 1));
        int n = 0;
        while (n < 11 && in >> p.values[size_t(n)])
            ++n;
        if (n == 11)
            insert(p);
    }
    if (m_presets.empty())
        for (int i = 0; i < PresetCount; ++i)
            insert({presetName(i), Presets[i]});
    const std::string chosen = s.string("EQPresetName");
    m_preset = chosen.empty() ? s.integer("EQPreset", 7) < 0 ? -1 : find(presetName(s.integer("EQPreset", 7))) : find(chosen);
    if (chosen == "-")
        m_preset = -1;
    std::istringstream in(s.string("EQBands"));
    std::array<int, 11> v{};
    int n = 0;
    while (n < 11 && in >> v[size_t(n)])
        ++n;
    if (n == 11)
        m_values = v;
    else if (m_preset >= 0)
        m_values = m_presets[size_t(m_preset)].values;
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

namespace {
std::string folded(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

int Equalizer::find(const std::string &name) const
{
    for (size_t i = 0; i < m_presets.size(); ++i)
        if (folded(m_presets[i].name) == folded(name))
            return int(i);
    return -1;
}

void Equalizer::insert(const Preset &p)
{
    const std::string key = folded(p.name);
    size_t at = 0;
    while (at < m_presets.size() && folded(m_presets[at].name) <= key)
        ++at;
    if (m_preset >= int(at))
        ++m_preset;
    m_presets.insert(m_presets.begin() + long(at), p);
}

void Equalizer::savePresets()
{
    std::vector<std::string> lines;
    for (const Preset &p : m_presets) {
        std::string v;
        for (int x : p.values)
            v += (v.empty() ? "" : " ") + std::to_string(x);
        lines.push_back(p.name + "\t" + v);
    }
    Settings("itunes").set("EQPresets", lines);
}

void Equalizer::updateTitle()
{
    if (!m_content)
        return;
    m_content->popup->title = m_preset >= 0 ? m_presets[size_t(m_preset)].name : tr("kEQPresetsMenuItemManual");
    m_content->popup->setNeedsDisplay();
}

void Equalizer::save()
{
    Settings s("itunes");
    s.set("EQOn", m_on);
    s.set("EQPresetName", m_preset >= 0 ? m_presets[size_t(m_preset)].name : std::string("-"));
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
    updateTitle();
    m_content->popup->onPress = [this] {
        std::vector<MenuItem> items;
        items.emplace_back(tr("kEQPresetsMenuItemMakePreset"), false, true);
        items.emplace_back(tr("kEQPresetsMenuItemEditList"), false, true);
        items.push_back(MenuItem::line());
        items.emplace_back(tr("kEQPresetsMenuItemManual"), false, true, m_preset < 0);
        items.push_back(MenuItem::line());
        for (size_t i = 0; i < m_presets.size(); ++i)
            items.emplace_back(m_presets[i].name, false, true, int(i) == m_preset);
        const Rect f = m_content->popup->frame();
        Menu::popUp(m_window.get(), {f.x, f.y + f.h}, items, f.w, textStyle(510).font, [this](int choice) {
            if (choice == 0) {
                makePreset();
            } else if (choice == 1) {
                editList();
            } else if (choice == 3) {
                m_preset = -1;
                updateTitle();
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
        updateTitle();
    }
    apply();
    save();
}

void Equalizer::choosePreset(int index)
{
    if (index < 0 || size_t(index) >= m_presets.size())
        return;
    m_preset = index;
    const std::array<int, 11> v = m_presets[size_t(index)].values;
    for (int i = 0; i < 11; ++i)
        setValue(i, v[size_t(i)], false);
    updateTitle();
    save();
}

// Get String (DLOG 10002, 360 × 103): the prompt (17, 8) 318 × 16, the field (20, 30) 320 × 16, Cancel
// (190, 63) and OK (270, 63), 70 × 20; done(the text) on OK
void Equalizer::getString(const std::string &title, const std::string &text, std::function<void(std::string)> done)
{
    const Size size = dialogSize(10002);
    m_askWindow = std::make_unique<Window>(Size{size.w > 0 ? size.w : 360, size.h > 0 ? size.h : 103}, title);
    m_askWindow->setStyleMask(2);
    m_askWindow->onClose = [this] { App::shared().later([this] { m_askWindow.reset(); }); };
    View *c = m_askWindow->contentView();
    TextField *field = nullptr;
    Button *ok = nullptr, *cancel = nullptr;
    int n = 0;
    for (const DialogItem &it : dialogItems(10002)) {
        ++n;
        if (n == 1 || n == 2) {
            Button *b = c->add<Button>(it.text);
            b->setFrame(it.rect);
            (n == 1 ? ok : cancel) = b;
        } else if (n == 3) {
            c->add<Label>(tr("177.010"), Font::system(13))->setFrame(it.rect);
        } else if (n == 4) {
            field = c->add<TextField>();
            field->setFrame(Rect{it.rect.x - 3, it.rect.y - 3, it.rect.w + 6, it.rect.h + 6});
        }
    }
    if (!ok || !cancel || !field)
        return;
    ok->isDefault = true;
    field->setText(text);
    field->selectAll();
    auto finish = [this, field, done] {
        const std::string t = field->text;
        m_askWindow->close();
        if (done)
            done(t);
    };
    ok->action = finish;
    field->onEnter = finish;
    cancel->action = [this] { m_askWindow->close(); };
    field->onCancel = [this] { m_askWindow->close(); };
    m_askWindow->show();
    m_askWindow->makeFirstResponder(field);
}

namespace {
std::string withName(std::string text, const std::string &one, const std::string &two = {})
{
    for (const auto &[mark, value] : {std::pair<std::string, std::string>{"^1", one}, {"^2", two}}) {
        const size_t at = text.find(mark);
        if (at != std::string::npos)
            text.replace(at, mark.size(), value);
    }
    return text;
}
Image appIcon() { return Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/iTunes.icns"), {64, 64}); }
} // namespace

void Equalizer::makePreset()
{
    const std::string current = m_preset >= 0 ? m_presets[size_t(m_preset)].name : tr("kEQPresetsMenuItemManual");
    getString(tr("177.009"), current, [this](std::string name) {
        if (name.empty())
            return;
        const std::array<int, 11> values = m_values;
        auto add = [this, name, values] {
            const int old = find(name);
            if (old >= 0) {
                if (m_preset == old)
                    m_preset = -1;
                else if (m_preset > old)
                    --m_preset;
                m_presets.erase(m_presets.begin() + old);
            }
            insert({name, values});
            savePresets();
            choosePreset(find(name));
            refreshList();
        };
        if (find(name) >= 0)
            Alert::run(appIcon(), withName(tr("177.006"), name), {}, {tr("23987.001"), tr("23987.002")}, [add](int b) {
                if (b == 0)
                    add();
            });
        else
            add();
    });
}

void Equalizer::renamePreset(int index)
{
    if (index < 0 || size_t(index) >= m_presets.size())
        return;
    const std::string oldName = m_presets[size_t(index)].name;
    getString(tr("177.011"), oldName, [this, oldName](std::string name) {
        if (name.empty() || name == oldName)
            return;
        auto rename = [this, oldName, name] {
            const int other = find(name);
            if (other >= 0 && folded(name) != folded(oldName)) {
                if (m_preset == other)
                    m_preset = -1;
                else if (m_preset > other)
                    --m_preset;
                m_presets.erase(m_presets.begin() + other);
            }
            const int at = find(oldName);
            if (at < 0)
                return;
            const bool chosen = m_preset == at;
            Preset p = m_presets[size_t(at)];
            p.name = name;
            if (m_preset > at)
                --m_preset;
            else if (chosen)
                m_preset = -1;
            m_presets.erase(m_presets.begin() + at);
            insert(p);
            if (chosen)
                m_preset = find(name);
            if (m_editList)
                m_editList->selected = find(name);
            savePresets();
            save();
            updateTitle();
            refreshList();
        };
        const int other = find(name);
        if (other >= 0 && folded(name) != folded(oldName))
            Alert::run(appIcon(), withName(tr("177.006"), name), {}, {tr("23987.001"), tr("23987.002")}, [rename](int b) {
                if (b == 0)
                    rename();
            });
        else
            rename();
    });
}

void Equalizer::deletePreset(int index)
{
    if (index < 0 || size_t(index) >= m_presets.size())
        return;
    const std::string name = m_presets[size_t(index)].name;
    Alert::run(appIcon(), withName(tr("177.004"), name), {}, {tr("23987.015"), tr("23987.004")}, [this, name](int b) {
        const int at = find(name);
        if (b != 0 || at < 0)
            return;
        if (m_preset == at)
            m_preset = -1;   // FUN_000a783c: the chosen one goes Manual, its values kept
        else if (m_preset > at)
            --m_preset;
        m_presets.erase(m_presets.begin() + at);
        savePresets();
        save();
        updateTitle();
        refreshList();
    });
}

void Equalizer::refreshList()
{
    if (!m_editList)
        return;
    m_editList->rows.clear();
    for (const Preset &p : m_presets)
        m_editList->rows.push_back({p.name});
    if (m_editList->selected >= int(m_presets.size()))
        m_editList->selected = -1;
    m_editList->reload();
    // FUN_000a639c: Rename… and Delete with one row chosen
    m_rename->setEnabled(m_editList->selected >= 0);
    m_delete->setEnabled(m_editList->selected >= 0);
}

// Edit Presets (DLOG 148, FUN_000a7d12)
void Equalizer::editList()
{
    if (m_editWindow) {
        m_editWindow->orderFront();
        return;
    }
    const Size size = dialogSize(148);
    m_editWindow = std::make_unique<Window>(Size{size.w > 0 ? size.w : 350, size.h > 0 ? size.h : 284}, "Edit Presets");
    m_editWindow->setStyleMask(2);
    m_editWindow->onClose = [this] {
        m_editList = nullptr;
        m_rename = m_delete = nullptr;
        App::shared().later([this] { m_editWindow.reset(); });
    };
    View *c = m_editWindow->contentView();
    int n = 0;
    Button *done = nullptr;
    for (const DialogItem &it : dialogItems(148)) {
        ++n;
        if (n == 2) {
            m_editList = c->add<TableView>();
            m_editList->header = false;
            m_editList->columns = {{"", it.rect.w - 2 - 15}};
            m_editList->setFrame(it.rect);
        } else if (n == 1 || n == 3 || n == 4) {
            Button *b = c->add<Button>(it.text);
            b->setFrame(it.rect);
            (n == 1 ? done : n == 3 ? m_rename : m_delete) = b;
        }
    }
    if (!m_editList || !done || !m_rename || !m_delete)
        return;
    done->isDefault = true;
    done->action = [this] { m_editWindow->close(); };
    m_rename->action = [this] { renamePreset(m_editList->selected); };
    m_delete->action = [this] { deletePreset(m_editList->selected); };
    m_editList->onSelect = [this](int) { refreshList(); };
    m_editList->onDoubleClick = [this](int r) { renamePreset(r); };
    refreshList();
    m_editWindow->show();
}

} // namespace iTunes
