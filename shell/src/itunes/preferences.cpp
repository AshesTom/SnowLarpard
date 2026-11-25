// SPDX-License-Identifier: LGPL-2.1-or-later
// The Preferences as Localized.rsrc has them:
//   DLOG 134 (544 wide), DITL 134: the toolbar (a user item, 0, 0, 544 × 56), a line (0, 76, 544 × 1),
//   the help button (CNTL 1340, 24 from the left), Cancel and OK (372 and 454), 39 from the bottom; the
//   window as tall as the chosen pane's DLOG (General 256, Podcasts 254, Playback 257, Sharing 261,
//   Store 262, Parental 313, Apple TV 255, Devices 263, Advanced 260), its items in its DITL.
//   The toolbar: STR# 178 (General, Playback, Sharing, Store, Advanced, Parental, Apple TV, Devices),
//   the icons iTunes.rsrc's icns 300 (General), 309 (Podcasts), 301 (Playback), 302 (Sharing), 306
//   (Store), PBUT 142 (Parental's lock), 303 (Apple TV), 307 (Devices), 304 (Advanced); the chosen one
//   on PBUT 660 'Toolbar Selection' (55 × 56; its PICT not read: drawn by eye).
// A DITL's items made as AquaKit's controls: buttons, check boxes, radio buttons, static and edit text,
// and the CNTLs: 144 a separator, 401 / 402 a popup on the MENU its minimum names, 50 / 51 a slider. The
// items the port acts on (below) are enabled; the others are shown disabled.
#include "preferences.h"

#include "controls.h"
#include "foundation.h"
#include "library.h"
#include "resources.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace iTunes {

namespace {

struct Pane {
    const char *titleKey;
    int ditl;
    int icns;   // 0: PBUT 142, the lock
};

const Pane Panes[9] = {
    {"178.001", 256, 300}, {"128.005", 254, 309}, {"178.002", 257, 301}, {"178.003", 261, 302}, {"178.004", 262, 306},
    {"178.006", 313, 0},   {"178.007", 255, 303}, {"178.008", 263, 307}, {"178.005", 260, 304},
};

// the items the port keeps, by pane DITL and item number: their preference keys
struct Kept {
    int ditl, item;
    const char *key;
    int fallback = 0;   // its value until one is kept
};
const Kept KeptItems[] = {
    {256, 2, "LibraryName"},      // General: Library Name
    {256, 15, "ListTextLarge"},   // General: List Text (MENU 1350: Small, Large)
    // General's Show boxes: the LIBRARY sources (section 29; FUN_000dc5be's hide flags)
    {256, 4, "ShowMovies", 1},   {256, 5, "ShowTVShows", 1},       {256, 6, "ShowPodcasts", 1},
    {256, 7, "ShowAudiobooks", 0}, {256, 8, "ShowApplications", 0}, {256, 9, "ShowRadio", 1},
    {256, 11, "ShowRingtones", 0},
    // iTunes DJ (+0x2A85) and Genius (box 23; FUN_005790fe) under PLAYLISTS (section 30)
    {256, 10, "ShowiTunesDJ", 1}, {256, 23, "ShowGenius", 1},
    // Podcasts (DITL 254): Check for new episodes (MENU 1374: every hour, day, week, manually), When new
    // episodes are available (1375: download all, the most recent one, do nothing), Keep (1376: all,
    // all unplayed, the most recent, the last 2, 3, 4, 5, 10)
    {254, 2, "PodcastCheck", 1}, {254, 5, "PodcastNew", 1}, {254, 7, "PodcastKeep", 0},
    {257, 10, "SoundCheck"},      // Playback: Sound Check
    // Playback: Play Movies and TV Shows, Play Music Videos (MENU 1373: in artwork viewer, in the iTunes
    // window, in a separate window, full screen, full screen with visuals)
    {257, 14, "PlayMovies", 1}, {257, 16, "PlayMusicVideos", 1},
    {260, 17, "MiniOnTop"},       // Advanced: Keep MiniPlayer on top of all other windows
};

const Kept *kept(int ditl, int item)
{
    for (const Kept &k : KeptItems)
        if (k.ditl == ditl && k.item == item)
            return &k;
    return nullptr;
}

Image icon(int icns)
{
    if (const std::string *d = art().get("icns", icns))
        return Image::fromData(std::vector<unsigned char>(d->begin(), d->end()), {32, 32});
    return {};
}

} // namespace

class PreferencesWindow::Toolbar : public View {
public:
    std::function<void(int)> onSelect;
    int selected = 0;
    static constexpr double ItemW = 58, X0 = 8;
    void draw(cairo_t *cr) override
    {
        // the toolbar's grey (the window's own), its bottom line
        fill(cr, bounds(), Color::gray(232));
        fill(cr, Rect{0, frame().h - 1, frame().w, 1}, Color::gray(150));
        for (int i = 0; i < 9; ++i) {
            const Rect cell{X0 + i * ItemW, 0, ItemW, 56};
            if (i == selected) {
                // PBUT 660 'Toolbar Selection' is a PICT with its mask (660, 661), not read: a darker
                // rounded square of its size (55 × 56) by eye
                const Rect s{cell.x + std::floor((ItemW - 55) / 2), 0, 55, 56};
                cairo_new_path(cr);
                const double r = 4;
                cairo_arc(cr, s.x + s.w - r, s.y + r, r, -M_PI / 2, 0);
                cairo_arc(cr, s.x + s.w - r, s.y + s.h - r, r, 0, M_PI / 2);
                cairo_arc(cr, s.x + r, s.y + s.h - r, r, M_PI / 2, M_PI);
                cairo_arc(cr, s.x + r, s.y + r, r, M_PI, 3 * M_PI / 2);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
                cairo_fill(cr);
            }
            const Image img = Panes[i].icns ? icon(Panes[i].icns) : Image();
            if (!img.isNull())
                img.draw(cr, Rect{cell.x + (ItemW - 32) / 2, 5, 32, 32});
            else
                drawCell(cr, 142, 1, 0, Rect{cell.x + (ItemW - 33) / 2, 4, 33, 33});
            AK::TextStyle t;
            t.font = Font::system(11);
            t.align = Align::Center;
            drawText(cr, tr(Panes[i].titleKey), Rect{cell.x - 4, 38, ItemW + 8, 14}, t);
        }
    }
    void mouseDown(Event &e) override
    {
        const int i = int((e.location.x - X0) / ItemW);
        if (i >= 0 && i < 9 && onSelect)
            onSelect(i);
    }
};

PreferencesWindow::PreferencesWindow() = default;
PreferencesWindow::~PreferencesWindow() = default;

void PreferencesWindow::show()
{
    if (!m_window) {
        m_window = std::make_unique<Window>(Size{544, 134}, "General");
        m_window->setStyleMask(2);
        m_window->onClose = [this] { App::shared().later([this] { m_window.reset(); m_paneViews.clear(); m_items.clear(); }); };
        m_toolbar = m_window->contentView()->add<Toolbar>();
        m_toolbar->setFrame({0, 0, 544, 56});
        m_toolbar->onSelect = [this](int i) { App::shared().later([this, i] { select(i); }); };
        select(Settings("itunes").integer("PrefsPane", 0));
    }
    m_window->orderFront();
}

void PreferencesWindow::select(int pane)
{
    pane = std::clamp(pane, 0, 8);
    m_pane = pane;
    Settings("itunes").set("PrefsPane", pane);
    View *c = m_window->contentView();
    for (View *v : m_paneViews)
        c->removeSubview(v);
    m_paneViews.clear();
    m_items.clear();
    m_toolbar->selected = pane;
    m_toolbar->setNeedsDisplay();
    const int ditl = Panes[pane].ditl;
    const Size size = dialogSize(ditl);
    const double H = size.h > 0 ? size.h : 134;
    Settings s("itunes");
    int n = 0;
    for (const DialogItem &it : dialogItems(ditl)) {
        ++n;
        View *v = nullptr;
        const Kept *k = kept(ditl, n);
        const Font font = it.rect.h <= 13 ? Font::system(11) : Font::system(13);
        switch (it.type) {
        case DialogItem::Text: {
            Label *l = c->add<Label>(it.text, font);
            l->wrap = it.rect.h > 20;
            v = l;
            break;
        }
        case DialogItem::CheckBox:
        case DialogItem::Radio: {
            CheckBox *b = c->add<CheckBox>(it.text, it.type == DialogItem::Radio);
            if (k)
                b->setChecked(s.boolean(k->key, k->fallback != 0));
            v = b;
            break;
        }
        case DialogItem::Button: {
            Button *b = c->add<Button>(it.text);
            b->small = it.rect.h < 20;
            v = b;
            break;
        }
        case DialogItem::Edit: {
            TextField *f = c->add<TextField>();
            if (k)
                f->setText(s.string(k->key, ditl == 256 && n == 2 ? "Library" : ""));
            v = f;
            break;
        }
        case DialogItem::Control:
            if (it.proc == 144 || it.proc == 145) {
                Box *b = c->add<Box>();
                b->kind = Box::Separator;
                v = b;
            } else if (it.proc >= 400 && it.proc <= 415) {
                PopUpButton *p = c->add<PopUpButton>();
                for (const std::string &t : classicMenu(it.min))
                    p->items.push_back(t == "-" ? MenuItem::line() : MenuItem(t));
                if (k)
                    p->setSelected(s.integer(k->key, k->fallback));
                v = p;
            } else if (it.proc == 48 || it.proc == 49 || it.proc == 50 || it.proc == 51) {
                Slider *sl = c->add<Slider>();
                sl->setValue(it.max > it.min ? double(it.value - it.min) / (it.max - it.min) : 0);
                v = sl;
            }
            break;
        default:
            break;
        }
        if (!v)
            continue;
        Rect r = it.rect;
        if (it.type == DialogItem::Edit)
            r = Rect{r.x - 3, r.y - 3, r.w + 6, r.h + 6};
        v->setFrame(r);
        // what the port does nothing with stays shown, disabled
        if (it.type != DialogItem::Text && !k)
            v->setEnabled(false);
        m_items[n] = v;
        m_paneViews.push_back(v);
    }
    // Advanced: the music folder's place in its box (item 3, a user item)
    if (ditl == 260) {
        Label *where = c->add<Label>(Library::musicFolder() + "/iTunes/iTunes Music", Font::system(11));
        where->wrap = true;
        where->setFrame({85, 103, 304, 40});
        m_paneViews.push_back(where);
    }
    Button *help = c->add<Button>("", Button::Help);
    help->setFrame({24, H - 39, 20, 20});
    help->setEnabled(false);
    Button *cancel = c->add<Button>("Cancel");
    cancel->setFrame({372, H - 45, 70, 32});
    cancel->action = [this] { App::shared().later([this] { if (m_window) m_window->close(); }); };
    Button *ok = c->add<Button>("OK");
    ok->isDefault = true;
    ok->setFrame({454, H - 45, 70, 32});
    ok->action = [this] {
        save();
        if (onApply)
            onApply();
        App::shared().later([this] { if (m_window) m_window->close(); });
    };
    for (View *v : {static_cast<View *>(help), static_cast<View *>(cancel), static_cast<View *>(ok)})
        m_paneViews.push_back(v);
    m_window->setTitle(tr(Panes[pane].titleKey));
    m_window->setContentSize({544, H});
}

void PreferencesWindow::save()
{
    Settings s("itunes");
    const int ditl = Panes[m_pane].ditl;
    for (const auto &[n, v] : m_items) {
        const Kept *k = kept(ditl, n);
        if (!k)
            continue;
        if (auto *b = dynamic_cast<CheckBox *>(v))
            s.set(k->key, b->checked());
        else if (auto *f = dynamic_cast<TextField *>(v))
            s.set(k->key, f->text);
        else if (auto *p = dynamic_cast<PopUpButton *>(v))
            s.set(k->key, p->selected());
    }
}

} // namespace iTunes
