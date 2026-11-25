// SPDX-License-Identifier: LGPL-2.1-or-later
#include "cdui.h"

#include "controls.h"
#include "foundation.h"
#include "resources.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

using namespace AK;

namespace iTunes {

namespace {

// A DITL's items as AquaKit's controls, by item number (1 up): static text, buttons, check boxes, radio
// buttons; the CNTLs 144 / 145 and the 1-high user items separators, 164 a group box, 112 the chasing
// arrows, 401–415 popups on the MENU their minimum names, 0 with CNTL 1340 the help button (disabled);
// the other user items popups the caller fills (the encoders' dialogs fill theirs by code)
std::map<int, View *> build(View *c, int ditl)
{
    std::map<int, View *> items;
    int n = 0;
    for (const DialogItem &it : dialogItems(ditl)) {
        ++n;
        View *v = nullptr;
        Rect r = it.rect;
        switch (it.type) {
        case DialogItem::Text: {
            Label *l = c->add<Label>(it.text, it.rect.h <= 13 ? Font::system(11) : Font::system(13));
            l->wrap = it.rect.h > 20;
            v = l;
            break;
        }
        case DialogItem::Button:
            v = c->add<Button>(it.text);
            break;
        case DialogItem::CheckBox:
        case DialogItem::Radio:
            v = c->add<CheckBox>(it.text, it.type == DialogItem::Radio);
            break;
        case DialogItem::User:
            if (it.rect.h <= 1) {
                Box *b = c->add<Box>();
                b->kind = Box::Separator;
                v = b;
            } else {
                v = c->add<PopUpButton>();
                r = Rect{r.x, r.y - 2, r.w, 20};
            }
            break;
        case DialogItem::Control:
            if (it.proc == 144 || it.proc == 145) {
                Box *b = c->add<Box>();
                b->kind = Box::Separator;
                v = b;
            } else if (it.proc == 164) {
                v = c->add<Box>();
            } else if (it.proc == 112) {
                auto *p = c->add<ProgressIndicator>();
                p->spinning = true;
                p->indeterminate = true;
                v = p;
            } else if (it.proc >= 400 && it.proc <= 415) {
                auto *p = c->add<PopUpButton>();
                for (const std::string &t : classicMenu(it.min))
                    p->items.push_back(t == "-" ? MenuItem::line() : MenuItem(t));
                v = p;
            } else if (it.proc == 0) {
                Button *b = c->add<Button>("", Button::Help);
                b->setEnabled(false);
                v = b;
            }
            break;
        default:
            break;
        }
        if (!v)
            continue;
        v->setFrame(r);
        items[n] = v;
    }
    return items;
}

template <class T> T *item(const std::map<int, View *> &items, int n)
{
    const auto it = items.find(n);
    return it == items.end() ? nullptr : dynamic_cast<T *>(it->second);
}

// A dialog window of its own in `holder`, dropped when closed
Window *dialog(std::unique_ptr<Window> &holder, int dlog, const std::string &title)
{
    const Size size = dialogSize(dlog);
    holder = std::make_unique<Window>(Size{size.w > 0 ? size.w : 400, size.h > 0 ? size.h : 200}, title);
    Window *w = holder.get();
    w->setStyleMask(2);
    std::unique_ptr<Window> *slot = &holder;
    w->onClose = [slot, w] {
        App::shared().later([slot, w] {
            if (slot->get() == w)
                slot->reset();
        });
    };
    return w;
}

void fill(PopUpButton *p, int menu)
{
    if (!p)
        return;
    p->items.clear();
    for (const std::string &t : classicMenu(menu))
        p->items.push_back(t == "-" ? MenuItem::line() : MenuItem(t));
}

// The popups' items read as numbers: "128 kbps" 128, "44.100 kHz" 44100, "8 bit" 8; "Auto" 0; Mono 1,
// Stereo 2
int valueOf(const std::string &t)
{
    if (t.find("kHz") != std::string::npos)
        return int(std::lround(std::atof(t.c_str()) * 1000));
    if (t == "Mono")
        return 1;
    if (t == "Stereo")
        return 2;
    return std::atoi(t.c_str());
}

void selectValue(PopUpButton *p, int value)
{
    if (!p)
        return;
    for (size_t i = 0; i < p->items.size(); ++i)
        if (!p->items[i].separator && valueOf(p->items[i].title) == value) {
            p->setSelected(int(i));
            return;
        }
    p->setSelected(0);
}

int selectedValue(const PopUpButton *p)
{
    return p && p->selected() >= 0 && size_t(p->selected()) < p->items.size() ? valueOf(p->items[size_t(p->selected())].title) : 0;
}

std::unique_ptr<Window> g_custom;

// The encoder's Custom… dialog: AAC Encoder (DLOG 5000), MP3 Encoder (3000), AIFF / WAV Encoder (3250);
// done(true) on OK with `e` changed
void customDialog(std::shared_ptr<EncoderSettings> e, std::function<void(bool)> done)
{
    const int dlog = e->encoder == AAC ? 5000 : e->encoder == MP3 ? 3000 : 3250;
    // the DLOGs' own titles: "AAC Encoder", "MP3 Encoder", "AIFF/WAV Encoder"
    Window *w = dialog(g_custom, dlog, e->encoder == AAC ? "AAC Encoder" : e->encoder == MP3 ? "MP3 Encoder" : "AIFF/WAV Encoder");
    const std::map<int, View *> items = build(w->contentView(), dlog);
    auto popup = [&](int n) { return item<PopUpButton>(items, n); };
    auto box = [&](int n) { return item<CheckBox>(items, n); };
    std::function<void(const EncoderSettings &)> show;
    std::function<void(EncoderSettings &)> read;
    if (e->encoder == AAC) {
        show = [=](const EncoderSettings &s) {
            selectValue(popup(5), s.bitrate);
            selectValue(popup(7), s.sampleRate);
            selectValue(popup(9), s.channels);
            box(10)->setChecked(s.vbr);
            box(11)->setChecked(s.voice);
        };
        read = [=](EncoderSettings &s) {
            s.bitrate = selectedValue(popup(5));
            s.sampleRate = selectedValue(popup(7));
            s.channels = selectedValue(popup(9));
            s.vbr = box(10)->checked();
            s.voice = box(11)->checked();
        };
    } else if (e->encoder == MP3) {
        // the user items' popups: 4 the bit rate (MENU 3001), 7 the VBR quality (3003's Lowest … Highest),
        // 9 the sample rate (3000), 11 the channels (3003), 13 the stereo mode (3004)
        fill(popup(4), 3001);
        if (PopUpButton *q = popup(7))
            for (int i = 1; i <= 7; ++i)
                q->items.emplace_back(tr("3003.00" + std::to_string(i)));
        fill(popup(9), 3000);
        fill(popup(11), 3003);
        fill(popup(13), 3004);
        show = [=](const EncoderSettings &s) {
            selectValue(popup(4), s.bitrate);
            box(6)->setChecked(s.vbr);
            popup(7)->setSelected(std::clamp(s.quality, 0, 6));
            popup(7)->setEnabled(s.vbr);
            selectValue(popup(9), s.sampleRate);
            selectValue(popup(11), s.channels);
            popup(13)->setSelected(s.joint ? 1 : 0);
            box(15)->setChecked(s.smart);
            box(16)->setChecked(s.filter);
        };
        read = [=](EncoderSettings &s) {
            s.bitrate = selectedValue(popup(4));
            s.vbr = box(6)->checked();
            s.quality = popup(7)->selected();
            s.sampleRate = selectedValue(popup(9));
            s.channels = selectedValue(popup(11));
            s.joint = popup(13)->selected() == 1;
            s.smart = box(15)->checked();
            s.filter = box(16)->checked();
        };
        if (CheckBox *vbr = box(6))
            vbr->onToggle = [=](CheckBox *b) { popup(7)->setEnabled(b->checked()); };
    } else {
        // 4 the sample rate (MENU 3250), 6 the sample size (3251), 8 the channels (3252)
        fill(popup(4), 3250);
        fill(popup(6), 3251);
        fill(popup(8), 3252);
        show = [=](const EncoderSettings &s) {
            selectValue(popup(4), s.sampleRate);
            selectValue(popup(6), s.sampleSize);
            selectValue(popup(8), s.channels);
        };
        read = [=](EncoderSettings &s) {
            s.sampleRate = selectedValue(popup(4));
            s.sampleSize = selectedValue(popup(6));
            s.channels = selectedValue(popup(8));
        };
    }
    const std::vector<int> needed = e->encoder == AAC ? std::vector<int>{5, 7, 9, 10, 11}
                                    : e->encoder == MP3 ? std::vector<int>{4, 6, 7, 9, 11, 13, 15, 16}
                                                        : std::vector<int>{4, 6, 8};
    for (int n : needed)
        if (!items.count(n))
            return;
    show(*e);
    Button *ok = item<Button>(items, 1), *cancel = item<Button>(items, 2), *defaults = item<Button>(items, 3);
    ok->isDefault = true;
    auto answered = std::make_shared<bool>(false);
    ok->action = [=] {
        read(*e);
        *answered = true;
        w->close();
        done(true);
    };
    cancel->action = [=] { w->close(); };
    w->onClose = [=] {
        if (!*answered) {
            *answered = true;
            done(false);
        }
        App::shared().later([w] {
            if (g_custom.get() == w)
                g_custom.reset();
        });
    };
    // Default Settings (AAC: FUN_00200586's 256 kbps VBR; MP3: High Quality's 160 kbps, joint stereo;
    // PCM: all automatic)
    defaults->action = [=] {
        EncoderSettings d;
        d.encoder = e->encoder;
        d.preset = 1;   // AAC's iTunes Plus, MP3's High Quality
        d.applyPreset();
        show(d);
    };
    w->show();
}

} // namespace

void importSettings(std::unique_ptr<Window> &holder, std::function<void()> done)
{
    if (holder) {
        holder->orderFront();
        return;
    }
    Window *w = dialog(holder, 183, "Import Settings");   // DLOG 183's name (its title empty, set by code)
    const std::map<int, View *> items = build(w->contentView(), 183);
    PopUpButton *encoder = item<PopUpButton>(items, 4), *setting = item<PopUpButton>(items, 6);
    Label *details = item<Label>(items, 9);
    CheckBox *correction = item<CheckBox>(items, 10);
    Button *ok = item<Button>(items, 1), *cancel = item<Button>(items, 2);
    if (!encoder || !setting || !details || !correction || !ok || !cancel)
        return;
    auto e = std::make_shared<EncoderSettings>(EncoderSettings::load());
    // MENU 1342 filled with the encoders by name (iTunes' importers: AAC, AIFF, Apple Lossless, MP3, WAV)
    encoder->items.clear();
    for (const char *k : {"5001.001", "3250.001", "5001.003", "3001.001", "3250.003"})
        encoder->items.emplace_back(tr(k));
    auto shown = std::make_shared<int>(0);   // the setting chosen before Custom… was
    auto refresh = [=] {
        setting->items.clear();
        for (const std::string &p : EncoderSettings::presets(e->encoder))
            setting->items.emplace_back(p);
        setting->setSelected(e->preset);
        setting->setEnabled(setting->items.size() > 1);
        *shown = e->preset;
        details->setText(e->details());
        details->setNeedsDisplay();
    };
    encoder->setSelected(e->encoder);
    correction->setChecked(e->errorCorrection);
    refresh();
    encoder->onSelect = [=](int i) {
        const bool ec = correction->checked();
        *e = EncoderSettings::load(i);
        e->errorCorrection = ec;
        refresh();
    };
    setting->onSelect = [=](int i) {
        const int custom = int(setting->items.size()) - 1;
        if (i == custom && custom > 0) {
            // Custom…: its dialog; Cancel puts the popup back
            auto edited = std::make_shared<EncoderSettings>(*e);
            if (*shown != custom)
                edited->applyPreset();
            customDialog(edited, [=](bool accepted) {
                if (accepted) {
                    *e = *edited;
                    e->preset = custom;
                }
                setting->setSelected(e->preset);
                *shown = e->preset;
                details->setText(e->details());
                details->setNeedsDisplay();
            });
            return;
        }
        e->preset = i;
        e->applyPreset();
        *shown = i;
        details->setText(e->details());
        details->setNeedsDisplay();
    };
    ok->isDefault = true;
    ok->action = [=] {
        e->errorCorrection = correction->checked();
        e->save();
        w->close();
        if (done)
            done();
    };
    cancel->action = [w] { w->close(); };
    w->show();
}

void burnSettings(std::unique_ptr<Window> &holder, const std::string &playlist, std::function<void(const BurnSettings &)> burn)
{
    if (holder) {
        holder->orderFront();
        return;
    }
    (void)playlist;
    Window *w = dialog(holder, 185, "Burn Settings");   // DLOG 185's name
    const std::map<int, View *> items = build(w->contentView(), 185);
    Label *searching = item<Label>(items, 4);
    auto *spinner = item<ProgressIndicator>(items, 5);
    PopUpButton *burner = item<PopUpButton>(items, 7), *speed = item<PopUpButton>(items, 9), *gap = item<PopUpButton>(items, 13);
    CheckBox *audio = item<CheckBox>(items, 11), *mp3 = item<CheckBox>(items, 15), *data = item<CheckBox>(items, 16);
    CheckBox *soundCheck = item<CheckBox>(items, 14), *cdText = item<CheckBox>(items, 18);
    Button *ok = item<Button>(items, 1), *cancel = item<Button>(items, 2);
    if (!searching || !spinner || !burner || !speed || !gap || !audio || !mp3 || !data || !soundCheck || !cdText || !ok || !cancel)
        return;
    const BurnSettings b = BurnSettings::load();
    speed->setSelected(std::clamp(b.speed, 0, int(speed->items.size()) - 1));
    gap->setSelected(std::clamp(b.gap, 0, int(gap->items.size()) - 1));
    soundCheck->setChecked(b.soundCheck);
    cdText->setChecked(b.cdText);
    CheckBox *formats[3] = {audio, mp3, data};
    auto format = std::make_shared<int>(std::clamp(b.format, 0, 2));
    auto setFormat = [=](int f) {
        *format = f;
        for (int i = 0; i < 3; ++i)
            formats[i]->setChecked(i == f);
        // the Audio CD's own items only with it
        gap->setEnabled(f == 0);
        soundCheck->setEnabled(f == 0);
        cdText->setEnabled(f == 0);
        if (items.count(12))
            items.at(12)->setEnabled(f == 0);
    };
    for (int i = 0; i < 3; ++i)
        formats[i]->onToggle = [=](CheckBox *) { setFormat(i); };
    setFormat(*format);
    // the burners searched for while "Searching for CD burners…" (item 4) shows with the arrows (5); the
    // popup over them once found; none: 162.004 without a burning program, else 162.002
    burner->setHidden(true);
    ok->setEnabled(false);
    spinner->start();
    auto found = std::make_shared<std::vector<Burner>>();
    const bool program = !Burning::program().empty();
    std::unique_ptr<Window> *slot = &holder;
    Burning::findBurners([=](std::vector<Burner> list) {
        if (slot->get() != w)
            return;
        *found = list;
        spinner->stop();
        spinner->setHidden(true);
        if (list.empty()) {
            searching->setText(tr(program ? "162.002" : "162.004"));
            searching->setNeedsDisplay();
            return;
        }
        searching->setHidden(true);
        burner->items.clear();
        int chosen = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            burner->items.emplace_back(list[i].name);
            if (list[i].device == b.burner)
                chosen = int(i);
        }
        burner->setSelected(chosen);
        burner->setHidden(false);
        ok->setEnabled(true);
    });
    ok->isDefault = true;
    ok->action = [=] {
        BurnSettings s;
        s.speed = speed->selected();
        s.format = *format;
        s.gap = gap->selected();
        s.soundCheck = soundCheck->checked();
        s.cdText = cdText->checked();
        if (burner->selected() >= 0 && size_t(burner->selected()) < found->size())
            s.burner = (*found)[size_t(burner->selected())].device;
        s.save();
        w->close();
        if (burn)
            burn(s);
    };
    cancel->action = [w] { w->close(); };
    w->show();
}

} // namespace iTunes
