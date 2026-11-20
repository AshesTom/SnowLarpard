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
// then the file's facts under the list's column names). Video, Sorting and Options have no tab: what
// they edit isn't kept. Lyrics: the tags' lyrics; Artwork: the song's own picture.
#include "getinfo.h"

#include "controls.h"
#include "foundation.h"
#include "library.h"
#include "resources.h"

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
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

} // namespace

struct InfoWindow::Pane {
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
    for (int i : {2, 3, 4})
        p.tabs->setTabLabel(i, "");
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

} // namespace iTunes
