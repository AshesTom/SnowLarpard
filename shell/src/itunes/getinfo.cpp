// SPDX-License-Identifier: LGPL-2.1-or-later
// The Info Window as Localized.rsrc has it: DLOG 135 (580 × 512), its items DITL 135: OK (490, 472,
// 70 × 20), Cancel (408, 472), Previous (20, 472, 81 × 20), Next (113, 472), the tabs CNTL 1358 at
// (20, 15), 540 × 441 (tab# 1358's seven labels). The Info pane, DITL 271 (in the dialog's coordinates):
// Name (40, 72, 500); Artist (40, 115, 404) and Year (464, 115, 76); Album Artist (40, 158, 404) and
// Track Number (464, 158, 24) "of" (516, 158, 24); Album (40, 201, 404) and Disc Number, of; Grouping
// (40, 244, 404) and BPM (464, 244, 76); Composer (40, 287, 500); Comments (40, 330, 500 × 64); Genre
// (40, 421, 251) with its popup (CNTL 2710 at 301, 419); "Part of a compilation" (351, 420). Each
// label 20 points over its field, 3 to its left; the labels the DITL's own text.
// The Summary pane is drawn by code not read here: its layout below is by eye (the artwork, the names,
// then the file's facts under the list's column names). Video DITL 275 (Show, Season Number, Episode
// ID, Episode Number, Description), Sorting DITL 276 (Name, Artist, Album Artist, Album, Composer, Show
// and their Sort fields: the tags' TITLESORT, ARTISTSORT, ALBUMARTISTSORT, ALBUMSORT, COMPOSERSORT,
// SHOWSORT), Options DITL 272 (Volume Adjustment on CNTL 2722, Equalizer Preset 2724, Media Kind 2725,
// VoiceOver Language 2726, Rating, Start and Stop Time, Remember playback position, Skip when
// shuffling, Part of a gapless album), each built from its DITL; what the library doesn't keep is
// shown disabled. Lyrics: the tags' lyrics; Artwork: the song's own picture.
#include "getinfo.h"

#include "controls.h"
#include "foundation.h"
#include "library.h"
#include "panels.h"
#include "resources.h"

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <cstdlib>

using namespace AK;

namespace iTunes {

namespace {

constexpr double TabX = 20, TabY = 15;

Label *label(View *page, const std::string &text, Rect r)
{
    Label *l = page->add<Label>(text, Font::system(11));
    l->setFrame(r.translated(-TabX, -TabY));
    return l;
}

TextField *field(View *page, Rect r)
{
    TextField *f = page->add<TextField>();
    f->font = Font::system(11);
    // the DITL's rect is the text's: the field's bezel 3 out
    f->setFrame(Rect{r.x - 3, r.y - 3, r.w + 6, r.h + 6}.translated(-TabX, -TabY));
    return f;
}

std::string number(int n) { return n ? std::to_string(n) : std::string(); }

std::string lyricsOf(const std::string &path)
{
    TagLib::FileRef f(path.c_str());
    if (f.isNull() || !f.file())
        return {};
    const TagLib::PropertyMap p = f.file()->properties();
    return p.contains("LYRICS") && !p["LYRICS"].isEmpty() ? p["LYRICS"].front().to8Bit(true) : std::string();
}

void writeLyrics(const std::string &path, const std::string &text)
{
    TagLib::FileRef f(path.c_str());
    if (f.isNull() || !f.file())
        return;
    TagLib::PropertyMap p = f.file()->properties();
    if (text.empty())
        p.erase("LYRICS");
    else
        p.replace("LYRICS", TagLib::StringList(TagLib::String(text, TagLib::String::UTF8)));
    f.file()->setProperties(p);
    f.save();
}

// the Sorting pane's Sort fields (DITL 276's edit items 4, 8, …, 24) and their tags
const struct {
    int item;
    const char *tag;
} SortFields[] = {{4, "TITLESORT"}, {8, "ARTISTSORT"}, {12, "ALBUMARTISTSORT"}, {16, "ALBUMSORT"}, {20, "COMPOSERSORT"}, {24, "SHOWSORT"}};

std::map<std::string, std::string> tagsOf(const std::string &path)
{
    std::map<std::string, std::string> out;
    TagLib::FileRef f(path.c_str());
    if (f.isNull() || !f.file())
        return out;
    const TagLib::PropertyMap p = f.file()->properties();
    for (const auto &[k, v] : p)
        if (!v.isEmpty())
            out[k.to8Bit(true)] = v.front().to8Bit(true);
    return out;
}

void writeTags(const std::string &path, const std::map<std::string, std::string> &tags)
{
    TagLib::FileRef f(path.c_str());
    if (f.isNull() || !f.file())
        return;
    TagLib::PropertyMap p = f.file()->properties();
    for (const auto &[k, v] : tags) {
        if (v.empty())
            p.erase(TagLib::String(k, TagLib::String::UTF8));
        else
            p.replace(TagLib::String(k, TagLib::String::UTF8), TagLib::StringList(TagLib::String(v, TagLib::String::UTF8)));
    }
    f.file()->setProperties(p);
    f.save();
}

// A pane from its DITL (the dialog's coordinates, less the tab view's place): its texts, fields, check
// boxes, buttons, popups (CNTL 401–415 on MENU min) and sliders (48–51); all disabled but the fields
std::map<int, View *> ditlPage(View *page, int ditl)
{
    std::map<int, View *> items;
    int n = 0;
    for (const DialogItem &it : dialogItems(ditl)) {
        ++n;
        View *v = nullptr;
        Rect r = it.rect;
        switch (it.type) {
        case DialogItem::Text:
            v = page->add<Label>(it.text, Font::system(11));
            break;
        case DialogItem::Edit: {
            TextField *f = page->add<TextField>();
            f->font = Font::system(11);
            r = Rect{r.x - 3, r.y - 3, r.w + 6, r.h + 6};
            v = f;
            break;
        }
        case DialogItem::CheckBox:
        case DialogItem::Radio: {
            CheckBox *b = page->add<CheckBox>(it.text, it.type == DialogItem::Radio);
            b->small = true;
            v = b;
            break;
        }
        case DialogItem::Button:
            v = page->add<Button>(it.text);
            break;
        case DialogItem::Control:
            if (it.proc >= 400 && it.proc <= 415) {
                PopUpButton *pop = page->add<PopUpButton>();
                for (const std::string &t : classicMenu(it.min))
                    pop->items.push_back(t == "-" ? MenuItem::line() : MenuItem(t));
                v = pop;
            } else if (it.proc >= 48 && it.proc <= 51) {
                Slider *sl = page->add<Slider>();
                sl->setValue(it.max > it.min ? double(it.value - it.min) / (it.max - it.min) : 0.5);
                v = sl;
            }
            break;
        default:
            break;
        }
        if (!v)
            continue;
        v->setFrame(r.translated(-TabX, -TabY));
        if (it.type != DialogItem::Text && it.type != DialogItem::Edit)
            v->setEnabled(false);
        items[n] = v;
    }
    return items;
}

} // namespace

struct InfoWindow::Pane {
    std::map<int, View *> video, sorting, options;
    std::map<std::string, std::string> sortLoaded;
    TabView *tabs = nullptr;
    // Summary
    ImageView *art = nullptr;
    Label *title = nullptr, *artist = nullptr, *album = nullptr, *time = nullptr;
    std::vector<Label *> facts;
    // Info
    TextField *name = nullptr, *artistF = nullptr, *year = nullptr, *albumArtist = nullptr, *track = nullptr,
              *trackOf = nullptr, *albumF = nullptr, *disc = nullptr, *discOf = nullptr, *grouping = nullptr,
              *bpm = nullptr, *composer = nullptr, *comments = nullptr;
    ComboBox *genre = nullptr;
    CheckBox *compilation = nullptr;
    // Lyrics, Artwork
    TextField *lyrics = nullptr;
    ImageView *bigArt = nullptr;
    Button *previous = nullptr, *next = nullptr;
    std::string lyricsLoaded;
};

InfoWindow::InfoWindow(Library &library) : m_library(library) {}
InfoWindow::~InfoWindow() = default;

void InfoWindow::show(const std::vector<int> &ids, int index)
{
    m_ids = ids;
    m_index = std::clamp(index, 0, std::max(0, int(ids.size()) - 1));
    if (!m_window)
        build();
    load();
    m_window->orderFront();
}

void InfoWindow::build()
{
    m_window = std::make_unique<Window>(Size{580, 512}, "");
    m_window->onClose = [this] { App::shared().later([this] { m_window.reset(); m_pane.reset(); }); };
    m_pane = std::make_unique<Pane>();
    Pane &p = *m_pane;
    View *c = m_window->contentView();

    p.tabs = c->add<TabView>();
    p.tabs->keepPageFrames = true;
    p.tabs->setFrame({TabX, TabY, 540, 441});

    // tab# 1358
    View *summary = new View;
    View *info = new View;
    View *video = new View, *sorting = new View, *options = new View;
    View *lyrics = new View;
    View *artwork = new View;
    for (View *v : {summary, info, video, sorting, options, lyrics, artwork})
        v->setFrame({0, 0, 540, 441});

    // Summary (by eye)
    p.art = summary->add<ImageView>();
    p.art->setFrame({22, 40, 128, 128});
    p.title = summary->add<Label>("", Font::boldSystem(13));
    p.title->setFrame({170, 52, 350, 18});
    p.artist = summary->add<Label>("", Font::system(11));
    p.artist->setFrame({170, 74, 350, 15});
    p.album = summary->add<Label>("", Font::system(11));
    p.album->setFrame({170, 92, 350, 15});
    p.time = summary->add<Label>("", Font::system(11));
    p.time->setFrame({170, 110, 350, 15});
    for (int i = 0; i < 8; ++i) {
        Label *l = summary->add<Label>("", Font::system(11));
        l->setFrame({40, 200.0 + 20 * i, 470, 15});
        p.facts.push_back(l);
    }

    // Info: DITL 271
    label(info, "Name", {37, 52, 90, 13});
    p.name = field(info, {40, 72, 500, 16});
    label(info, "Artist", {37, 95, 90, 13});
    p.artistF = field(info, {40, 115, 404, 16});
    label(info, "Year", {461, 95, 54, 13});
    p.year = field(info, {464, 115, 76, 16});
    label(info, "Album Artist", {37, 138, 130, 13});
    p.albumArtist = field(info, {40, 158, 404, 16});
    label(info, "Track Number", {461, 138, 87, 13});
    p.track = field(info, {464, 158, 24, 16});
    label(info, "of", {495, 158, 15, 16});
    p.trackOf = field(info, {516, 158, 24, 16});
    label(info, "Album", {37, 181, 90, 13});
    p.albumF = field(info, {40, 201, 404, 16});
    label(info, "Disc Number", {461, 181, 83, 13});
    p.disc = field(info, {464, 201, 24, 16});
    label(info, "of", {495, 201, 15, 16});
    p.discOf = field(info, {516, 201, 24, 16});
    label(info, "Grouping", {37, 224, 90, 13});
    p.grouping = field(info, {40, 244, 404, 16});
    label(info, "BPM", {461, 224, 50, 13});
    p.bpm = field(info, {464, 244, 76, 16});
    label(info, "Composer", {37, 267, 90, 13});
    p.composer = field(info, {40, 287, 500, 16});
    label(info, "Comments", {37, 310, 90, 13});
    p.comments = field(info, {40, 330, 500, 16});
    label(info, "Genre", {37, 401, 90, 13});
    p.genre = info->add<ComboBox>();
    p.genre->font = Font::system(11);
    p.genre->setFrame(Rect{37, 418, 251 + 6 + 22, 22}.translated(-TabX, -TabY));
    p.compilation = info->add<CheckBox>("Part of a compilation");
    p.compilation->small = true;
    p.compilation->setFrame(Rect{351, 420, 160, 16}.translated(-TabX, -TabY));
    p.compilation->setEnabled(false);   // not kept in the library yet

    // Video, Sorting, Options: their DITLs
    p.video = ditlPage(video, 275);
    for (auto &[n, v] : p.video)
        if (dynamic_cast<TextField *>(v))
            v->setEnabled(false);   // not kept: no TV shows in the library
    p.sorting = ditlPage(sorting, 276);
    for (auto &[n, v] : p.sorting)
        if (dynamic_cast<TextField *>(v) && n % 4 == 2)
            v->setEnabled(false);   // the left column: the Info pane's own fields
    p.options = ditlPage(options, 272);
    for (auto &[n, v] : p.options)
        v->setEnabled(dynamic_cast<Label *>(v) != nullptr);

    // Lyrics: the tags'
    p.lyrics = lyrics->add<TextField>();
    p.lyrics->font = Font::system(11);
    p.lyrics->setFrame({20, 40, 500, 22});
    // Artwork: the song's own picture
    p.bigArt = artwork->add<ImageView>();
    p.bigArt->setFrame({20, 40, 300, 300});

    p.tabs->addTab("Summary", summary);
    p.tabs->addTab("Info", info);
    p.tabs->addTab("Video", video);
    p.tabs->addTab("Sorting", sorting);
    p.tabs->addTab("Options", options);
    p.tabs->addTab("Lyrics", lyrics);
    p.tabs->addTab("Artwork", artwork);
    p.tabs->select(1);

    Button *ok = c->add<Button>("OK");
    ok->isDefault = true;
    ok->setFrame({490, 472 - 6, 70, 32});
    ok->action = [this] {
        save();
        m_window->close();
    };
    Button *cancel = c->add<Button>("Cancel");
    cancel->setFrame({408, 472 - 6, 70, 32});
    cancel->action = [this] { m_window->close(); };
    p.previous = c->add<Button>("Previous");
    p.previous->setFrame({20, 472 - 6, 81, 32});
    p.previous->action = [this] { step(-1); };
    p.next = c->add<Button>("Next");
    p.next->setFrame({113, 472 - 6, 81, 32});
    p.next->action = [this] { step(1); };
    m_window->show();
}

void InfoWindow::load()
{
    if (m_ids.empty())
        return;
    const Track *t = m_library.track(m_ids[size_t(m_index)]);
    if (!t)
        return;
    Pane &p = *m_pane;
    m_window->setTitle(t->name);
    p.previous->setEnabled(m_index > 0);
    p.next->setEnabled(m_index + 1 < int(m_ids.size()));

    const Image art = Image::fromData(artworkData(t->path), {300, 300});
    p.art->image = art.isNull() ? Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/iTunes.icns"), {128, 128}) : art;
    p.art->setNeedsDisplay();
    p.bigArt->image = art;
    p.bigArt->setNeedsDisplay();
    p.title->setText(t->name);
    p.artist->setText(t->artist);
    p.album->setText(t->album);
    p.time->setText(formatTime(t->totalTime));
    const std::string facts[8] = {
        tr("9006.007") + ": " + t->kind,
        tr("9006.008") + ": " + (t->size >= 1048576 ? std::to_string(t->size * 10 / 1048576 / 10) + "." + std::to_string(t->size * 10 / 1048576 % 10) + " MB"
                                                     : std::to_string(t->size / 1024) + " KB"),
        tr("9006.013") + ": " + (t->bitRate ? std::to_string(t->bitRate) + " kbps" : std::string()),
        tr("9006.014") + ": " + (t->sampleRate ? std::to_string(t->sampleRate / 1000) + "." + std::to_string(t->sampleRate % 1000 / 100) + " kHz" : std::string()),
        tr("9006.017") + ": " + t->dateAdded,
        tr("9006.019") + ": " + std::to_string(t->playCount),
        tr("9006.021") + ": " + (t->rating ? std::string(size_t(t->rating / 20), '*') : std::string()),
        tr("9006.031") + ": " + t->path,
    };
    for (int i = 0; i < 8; ++i)
        p.facts[size_t(i)]->setText(facts[i]);

    p.name->setText(t->name);
    p.artistF->setText(t->artist);
    p.year->setText(number(t->year));
    p.albumArtist->setText(t->albumArtist);
    p.track->setText(number(t->trackNumber));
    p.trackOf->setText(number(t->trackCount));
    p.albumF->setText(t->album);
    p.disc->setText(number(t->discNumber));
    p.discOf->setText(number(t->discCount));
    p.grouping->setText(t->grouping);
    p.bpm->setText(number(t->bpm));
    p.composer->setText(t->composer);
    p.comments->setText(t->comments);
    p.genre->setText(t->genre);
    // the popup's genres: the library's, then iTunes' own list (genres.plist is the Store's)
    std::vector<std::string> genres;
    for (const auto &[id, s] : m_library.tracks())
        if (!s.genre.empty())
            genres.push_back(s.genre);
    for (const char *g : {"Alternative", "Blues", "Children's Music", "Classical", "Country", "Dance", "Easy Listening",
                          "Electronic", "Folk", "Hip Hop/Rap", "Holiday", "Jazz", "Latin", "New Age", "Pop", "R&B/Soul",
                          "Reggae", "Rock", "Singer/Songwriter", "Soundtrack", "Spoken Word", "Vocal", "World"})
        genres.push_back(g);
    std::sort(genres.begin(), genres.end());
    genres.erase(std::unique(genres.begin(), genres.end()), genres.end());
    p.genre->items = genres;
    // Sorting: the names on the left, their sort forms from the tags on the right
    const std::map<std::string, std::string> tags = tagsOf(t->path);
    const std::string left[6] = {t->name, t->artist, t->albumArtist, t->album, t->composer, ""};
    p.sortLoaded.clear();
    for (int i = 0; i < 6; ++i) {
        if (auto *f = dynamic_cast<TextField *>(p.sorting[SortFields[i].item - 2]))
            f->setText(left[i]);
        auto it = tags.find(SortFields[i].tag);
        p.sortLoaded[SortFields[i].tag] = it == tags.end() ? "" : it->second;
        if (auto *f = dynamic_cast<TextField *>(p.sorting[SortFields[i].item]))
            f->setText(p.sortLoaded[SortFields[i].tag]);
    }
    p.lyricsLoaded = lyricsOf(t->path);
    p.lyrics->setText(p.lyricsLoaded);
}

bool InfoWindow::save()
{
    if (m_ids.empty() || !m_pane)
        return false;
    const Track *old = m_library.track(m_ids[size_t(m_index)]);
    if (!old)
        return false;
    Pane &p = *m_pane;
    Track t = *old;
    t.name = p.name->text;
    t.artist = p.artistF->text;
    t.year = std::atoi(p.year->text.c_str());
    t.albumArtist = p.albumArtist->text;
    t.trackNumber = std::atoi(p.track->text.c_str());
    t.trackCount = std::atoi(p.trackOf->text.c_str());
    t.album = p.albumF->text;
    t.discNumber = std::atoi(p.disc->text.c_str());
    t.discCount = std::atoi(p.discOf->text.c_str());
    t.grouping = p.grouping->text;
    t.bpm = std::atoi(p.bpm->text.c_str());
    t.composer = p.composer->text;
    t.comments = p.comments->text;
    t.genre = p.genre->text;
    const bool changed = t.name != old->name || t.artist != old->artist || t.year != old->year || t.albumArtist != old->albumArtist ||
                         t.trackNumber != old->trackNumber || t.trackCount != old->trackCount || t.album != old->album ||
                         t.discNumber != old->discNumber || t.discCount != old->discCount || t.grouping != old->grouping ||
                         t.bpm != old->bpm || t.composer != old->composer || t.comments != old->comments || t.genre != old->genre;
    if (changed) {
        m_library.writeTags(t);
        m_library.save();
    }
    if (p.lyrics->text != p.lyricsLoaded)
        writeLyrics(t.path, p.lyrics->text);
    std::map<std::string, std::string> sorts;
    for (const auto &sf : SortFields)
        if (auto *f = dynamic_cast<TextField *>(p.sorting[sf.item]); f && f->text != p.sortLoaded[sf.tag])
            sorts[sf.tag] = f->text;
    if (!sorts.empty())
        writeTags(t.path, sorts);
    if (changed && onChanged)
        onChanged(t.id);
    return changed;
}

void InfoWindow::step(int by)
{
    save();
    const int i = m_index + by;
    if (i < 0 || i >= int(m_ids.size()))
        return;
    m_index = i;
    load();
}

// ---- Multiple Item Information (DITL 136, its Info pane DITL 280)

namespace {

// The rating's user item (46, 102 × 20): PBUT 528's stars, a click on them the rating
class StarsView : public View {
public:
    int rating = 0;
    std::function<void()> onChange;
    void draw(cairo_t *cr) override { drawCell(cr, 528, 0, (rating + 5) / 10, Rect{0, std::floor((frame().h - 11) / 2), 68, 11}); }
    void mouseDown(Event &e) override
    {
        rating = std::clamp(int(std::ceil(e.location.x / (68.0 / 5))), 0, 5) * 20;
        setNeedsDisplay();
        if (onChange)
            onChange();
    }
};

} // namespace

struct MultipleInfoWindow::Pane {
    // the Info pane's fields by their DITL 280 numbers: the box before each, and its field
    std::map<int, TextField *> fields;
    std::map<int, CheckBox *> boxes;
    ComboBox *genre = nullptr;
    StarsView *stars = nullptr;
    std::map<int, View *> video, sorting, options;
};

MultipleInfoWindow::MultipleInfoWindow(Library &library) : m_library(library) {}
MultipleInfoWindow::~MultipleInfoWindow() = default;

void MultipleInfoWindow::show(const std::vector<int> &ids)
{
    // FUN_000e9042: asked first (its "Do not ask me again", 23987.019, kept at +0x2AF: not shown here)
    Alert::run(Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/iTunes.icns"), {64, 64}), tr("9008.001"), {},
               {tr("23987.001"), tr("23987.004")}, [this, ids](int b) {
                   if (b != 0)
                       return;
                   m_ids = ids;
                   m_window.reset();
                   build();
               });
}

namespace {
// DITL 280's fields: the box's item, the field's item; a text or a number
struct MultiField {
    int box, edit;
    bool number;
};
const MultiField MultiFields[] = {
    {2, 3, false},   {5, 6, true},    {8, 9, false},   {11, 12, true},  {14, 15, true}, {17, 18, false},
    {20, 21, true},  {23, 24, true},  {26, 27, false}, {29, 30, true},  {32, 33, false}, {38, 39, false},
};
// a song's value of a field by its edit item
std::string multiValue(const Track &t, int edit)
{
    auto n = [](int v) { return v ? std::to_string(v) : std::string(); };
    switch (edit) {
    case 3: return t.artist;
    case 6: return n(t.year);
    case 9: return t.albumArtist;
    case 12: return n(t.trackNumber);
    case 15: return n(t.trackCount);
    case 18: return t.album;
    case 21: return n(t.discNumber);
    case 24: return n(t.discCount);
    case 27: return t.grouping;
    case 30: return n(t.bpm);
    case 33: return t.composer;
    case 39: return t.comments;
    case 42: return t.genre;
    default: return {};
    }
}
void setMultiValue(Track &t, int edit, const std::string &v)
{
    const int n = std::atoi(v.c_str());
    switch (edit) {
    case 3: t.artist = v; break;
    case 6: t.year = n; break;
    case 9: t.albumArtist = v; break;
    case 12: if (!v.empty()) t.trackNumber = n; break;   // empty: each song's own (FUN_000e9042)
    case 15: if (!v.empty()) t.trackCount = n; break;
    case 18: t.album = v; break;
    case 21: if (!v.empty()) t.discNumber = n; break;
    case 24: if (!v.empty()) t.discCount = n; break;
    case 27: t.grouping = v; break;
    case 30: t.bpm = n; break;
    case 33: t.composer = v; break;
    case 39: t.comments = v; break;
    case 42: t.genre = v; break;
    }
}
} // namespace

void MultipleInfoWindow::build()
{
    const Size size = dialogSize(136);
    m_window = std::make_unique<Window>(Size{size.w > 0 ? size.w : 598, size.h > 0 ? size.h : 471}, "Multiple Item Information");
    m_window->onClose = [this] { App::shared().later([this] { m_window.reset(); m_pane.reset(); }); };
    m_pane = std::make_unique<Pane>();
    Pane &p = *m_pane;
    View *c = m_window->contentView();
    TabView *tabs = c->add<TabView>();
    tabs->keepPageFrames = true;
    tabs->setFrame({TabX, TabY, 558, 400});
    View *info = new View, *video = new View, *sorting = new View, *options = new View;
    for (View *v : {info, video, sorting, options})
        v->setFrame({0, 0, 558, 400});

    // the songs' common values (FUN_000fac26)
    std::vector<const Track *> tracks;
    for (int id : m_ids)
        if (const Track *t = m_library.track(id))
            tracks.push_back(t);
    auto common = [&](int edit) {
        if (tracks.empty())
            return std::string();
        const std::string v = multiValue(*tracks.front(), edit);
        for (const Track *t : tracks)
            if (multiValue(*t, edit) != v)
                return std::string();
        return v;
    };

    int n = 0;
    for (const DialogItem &it : dialogItems(280)) {
        ++n;
        const Rect r = it.rect.translated(-TabX, -TabY);
        if (it.type == DialogItem::Text) {
            info->add<Label>(it.text, Font::system(11))->setFrame(r);
        } else if (it.type == DialogItem::CheckBox) {
            CheckBox *b = info->add<CheckBox>("");
            b->small = true;
            b->setFrame(r);
            p.boxes[n] = b;
        } else if (it.type == DialogItem::Edit && n != 42) {
            TextField *f = info->add<TextField>();
            f->font = Font::system(11);
            f->setFrame(Rect{r.x - 3, r.y - 3, r.w + 6, r.h + 6});
            f->setText(common(n));
            p.fields[n] = f;
            // typing in a field boxes it
            f->onChange = [&p, n](const std::string &) {
                if (p.boxes.count(n - 1))
                    p.boxes[n - 1]->setChecked(true);
            };
        } else if (n == 42) {
            // the genre: the field and its popup (CNTL 1369) as one combo box
            p.genre = info->add<ComboBox>();
            p.genre->font = Font::system(11);
            p.genre->setFrame(Rect{r.x - 3, r.y - 3, r.w + 6 + 34, r.h + 6});
            std::vector<std::string> genres;
            for (const auto &[id, t] : m_library.tracks())
                if (!t.genre.empty() && std::find(genres.begin(), genres.end(), t.genre) == genres.end())
                    genres.push_back(t.genre);
            std::sort(genres.begin(), genres.end());
            p.genre->items = genres;
            p.genre->setText(common(42));
            p.genre->onChange = [&p](const std::string &) { p.boxes[41]->setChecked(true); };
        } else if (n == 46) {
            p.stars = info->add<StarsView>();
            p.stars->setFrame(r);
            int rating = tracks.empty() ? 0 : tracks.front()->rating;
            for (const Track *t : tracks)
                if (t->rating != rating)
                    rating = 0;
            p.stars->rating = rating;
            p.stars->onChange = [&p] { p.boxes[45]->setChecked(true); };
        } else if (n == 36) {
            info->add<ImageView>()->setFrame(r);   // the artwork's well: not written here
        }
    }
    if (p.boxes.count(35))
        p.boxes[35]->setEnabled(false);

    p.video = ditlPage(video, 281);
    p.sorting = ditlPage(sorting, 282);
    p.options = ditlPage(options, 283);
    for (auto *page : {&p.video, &p.sorting, &p.options})
        for (auto &[k, v] : *page)
            v->setEnabled(dynamic_cast<Label *>(v) != nullptr);

    tabs->addTab("Info", info);
    tabs->addTab("Video", video);
    tabs->addTab("Sorting", sorting);
    tabs->addTab("Options", options);
    tabs->select(0);

    // OK (1) and Cancel (2) at their DITL places
    n = 0;
    for (const DialogItem &it : dialogItems(136)) {
        ++n;
        if (n > 2)
            break;
        Button *b = c->add<Button>(it.text);
        b->setFrame(Rect{it.rect.x, it.rect.y - 6, it.rect.w, 32});
        if (n == 1) {
            b->isDefault = true;
            b->action = [this] {
                save();
                m_window->close();
            };
        } else {
            b->action = [this] { m_window->close(); };
        }
    }
    m_window->show();
}

void MultipleInfoWindow::save()
{
    if (!m_pane)
        return;
    Pane &p = *m_pane;
    bool any = false;
    for (int id : m_ids) {
        const Track *old = m_library.track(id);
        if (!old)
            continue;
        Track t = *old;
        for (const MultiField &f : MultiFields)
            if (p.boxes.count(f.box) && p.boxes[f.box]->checked() && p.fields.count(f.edit))
                setMultiValue(t, f.edit, p.fields[f.edit]->text);
        if (p.genre && p.boxes.count(41) && p.boxes[41]->checked())
            setMultiValue(t, 42, p.genre->text);
        const bool rated = p.stars && p.boxes.count(45) && p.boxes[45]->checked();
        if (rated && m_library.track(id))
            m_library.track(id)->rating = p.stars->rating;
        const bool changed = t.artist != old->artist || t.year != old->year || t.albumArtist != old->albumArtist ||
                             t.trackNumber != old->trackNumber || t.trackCount != old->trackCount || t.album != old->album ||
                             t.discNumber != old->discNumber || t.discCount != old->discCount || t.grouping != old->grouping ||
                             t.bpm != old->bpm || t.composer != old->composer || t.comments != old->comments || t.genre != old->genre;
        if (changed) {
            t.rating = m_library.track(id)->rating;
            m_library.writeTags(t);
        }
        any = any || changed || rated;
    }
    if (any) {
        m_library.save();
        if (onChanged)
            onChanged();
    }
}

} // namespace iTunes
