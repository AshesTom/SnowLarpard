// SPDX-License-Identifier: LGPL-2.1-or-later
// View Options as DLOG 144 has it (420 × 436): the source's icon (a user item, 20, 9, 20 × 20) and its
// name (44, 12); "Show Columns" (20, 39); 48 check boxes 120 × 20, three columns of 16 from (30, 60),
// 120 apart, 20 down each, their titles the code's (the columns, by title); Cancel (248, 396) and OK
// (330, 396).
#include "viewoptions.h"

#include "controls.h"
#include "resources.h"

#include <algorithm>

using namespace AK;

namespace iTunes {

const std::vector<ListField> &listFields()
{
    // CWid 9000: field, default width, justification
    static const std::vector<ListField> fields = {
        {3, "9006.002", 125, Align::Left, false},  {71, "9006.039", 125, Align::Left, false},
        {4, "9006.003", 125, Align::Left, false},  {35, "9006.025", 50, Align::Right, true},
        {5, "9006.013", 80, Align::Right, true},   {14, "9006.015", 250, Align::Left, false},
        {18, "9006.004", 125, Align::Left, false}, {16, "9006.017", 125, Align::Left, false},
        {24, "9006.010", 60, Align::Right, true},  {8, "9006.006", 80, Align::Left, false},
        {39, "9006.026", 125, Align::Left, false}, {9, "9006.007", 100, Align::Left, false},
        {2, "9006.001", 200, Align::Left, false},  {22, "9006.019", 82, Align::Right, true},
        {25, "9006.021", 79, Align::Left, true},   {6, "9006.014", 91, Align::Right, true},
        {12, "9006.008", 60, Align::Right, true},  {13, "9006.009", 59, Align::Right, true},
        {11, "9006.011", 70, Align::Right, true},  {7, "9006.012", 50, Align::Left, true},
        // the video and podcast sources' (section 29): CWid 9000's Description 0x36 (250), Category 0x37
        // (80), Release Date 0x22 (94); Show 0x3e, Season 0x3f, Episode ID 0x40 not in CWid 9000 (by eye)
        {54, "9006.029", 250, Align::Left, false}, {55, "9006.030", 80, Align::Left, false},
        {34, "9006.024", 94, Align::Left, false},  {62, "9006.033", 125, Align::Left, false},
        {63, "9006.034", 60, Align::Right, true},  {64, "9006.035", 80, Align::Left, false},
    };
    return fields;
}

const ListField *listField(int field)
{
    for (const ListField &f : listFields())
        if (f.field == field)
            return &f;
    return nullptr;
}

std::vector<int> defaultColumns() { return {2, 13, 4, 3, 8, 22, 25}; }

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string pair(int n, int of)
{
    if (!n)
        return {};
    return of ? std::to_string(n) + " of " + std::to_string(of) : std::to_string(n);
}

} // namespace

long long cellNumber(const Track &t, int field)
{
    switch (field) {
    case 35: return t.bpm;
    case 5: return t.bitRate;
    case 24: return t.discNumber;
    case 22: return t.playCount;
    case 25: return t.rating;
    case 6: return t.sampleRate;
    case 12: return t.size;
    case 13: return t.totalTime;
    case 11: return t.trackNumber;
    case 7: return t.year;
    case 63: return t.season;
    }
    return 0;
}

std::string cellText(const Track &t, int field)
{
    char buf[64];
    switch (field) {
    case 3: return t.album;
    case 71: return t.albumArtist;
    case 4: return t.artist;
    case 35: return t.bpm ? std::to_string(t.bpm) : std::string();
    case 5: return t.bitRate ? std::to_string(t.bitRate) + " kbps" : std::string();
    case 14: return t.comments;
    case 18: return t.composer;
    case 16: {
        // the plist's date, as the list shows it: m/d/yy h:mm
        int y, mo, d, h, mi;
        if (std::sscanf(t.dateAdded.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5)
            return {};
        std::snprintf(buf, sizeof buf, "%d/%d/%02d %d:%02d %s", mo, d, y % 100, h % 12 ? h % 12 : 12, mi, h < 12 ? "AM" : "PM");
        return buf;
    }
    case 24: return pair(t.discNumber, t.discCount);
    case 8: return t.genre;
    case 39: return t.grouping;
    case 9: return t.kind;
    case 2: return t.name;
    case 22: return t.playCount ? std::to_string(t.playCount) : std::string();
    case 25: return std::to_string(t.rating);
    case 6: return t.sampleRate ? std::to_string(t.sampleRate) + " Hz" : std::string();
    case 12:
        std::snprintf(buf, sizeof buf, "%.1f MB", t.size / 1048576.0);
        return buf;
    case 13: return formatTime(t.totalTime);
    case 11: return pair(t.trackNumber, t.trackCount);
    case 7: return t.year ? std::to_string(t.year) : std::string();
    case 54: return t.description;
    case 55: return t.category;
    case 34: {
        int y, mo, d;   // the release date as the list shows it: m/d/yy
        if (std::sscanf(t.releaseDate.c_str(), "%d-%d-%d", &y, &mo, &d) != 3)
            return {};
        std::snprintf(buf, sizeof buf, "%d/%d/%02d", mo, d, y % 100);
        return buf;
    }
    case 62: return t.series;
    case 63: return t.season ? std::to_string(t.season) : std::string();
    case 64: return t.episode;
    }
    return {};
}

std::string cellKey(const Track &t, int field)
{
    const std::string track = std::to_string(1000 + t.discNumber) + std::to_string(1000 + t.trackNumber);
    switch (field) {
    case 4: return lower(t.artist) + "\x01" + lower(t.album) + "\x01" + track;
    case 71: return lower(t.albumArtist) + "\x01" + lower(t.album) + "\x01" + track;
    case 3: return lower(t.album) + "\x01" + track;
    case 8: return lower(t.genre) + "\x01" + lower(t.artist) + "\x01" + lower(t.album) + "\x01" + track;
    case 16: return t.dateAdded;
    case 34: return t.releaseDate;
    case 62: return lower(t.series) + "\x01" + std::to_string(1000 + t.season) + std::to_string(10000 + t.episodeOrder);
    }
    return lower(cellText(t, field));
}

ViewOptionsWindow::ViewOptionsWindow(const std::string &source, int icon, const std::vector<int> &columns,
                                     std::function<void(const std::vector<int> &)> done)
    : m_columns(columns), m_done(std::move(done)), m_source(source), m_icon(icon)
{
}

ViewOptionsWindow::~ViewOptionsWindow() = default;

namespace {

class SourceIcon : public View {
public:
    explicit SourceIcon(int row) : m_row(row) {}
    void draw(cairo_t *cr) override { drawCell(cr, 441, 0, m_row, Rect{2, 2, 16, 16}); }

private:
    int m_row;
};

} // namespace

void ViewOptionsWindow::show()
{
    if (m_window) {
        m_window->orderFront();
        return;
    }
    m_window = std::make_unique<Window>(Size{420, 436}, tr("kMenuItemViewOptions").substr(0, tr("kMenuItemViewOptions").find("…")));
    m_window->setStyleMask(2);
    m_window->onClose = [this] { App::shared().later([this] { m_window.reset(); }); };
    View *c = m_window->contentView();
    c->add<SourceIcon>(m_icon)->setFrame({20, 9, 20, 20});
    Label *name = c->add<Label>(m_source, Font::boldSystem(13));
    name->setFrame({44, 10, 221, 17});
    Label *show = c->add<Label>("Show Columns", Font::boldSystem(13));
    show->setFrame({20, 39, 165, 17});
    // the columns by title, down the first column of boxes then the next
    std::vector<ListField> fields = listFields();
    std::sort(fields.begin(), fields.end(), [](const ListField &a, const ListField &b) { return tr(a.titleKey) < tr(b.titleKey); });
    auto boxes = std::make_shared<std::vector<std::pair<int, CheckBox *>>>();
    int i = 0;
    for (const ListField &f : fields) {
        if (f.field == 2)   // the songs' names: always shown
            continue;
        CheckBox *b = c->add<CheckBox>(tr(f.titleKey));
        b->setFrame({30.0 + 120 * (i / 16), 60.0 + 20 * (i % 16), 120, 20});
        b->setChecked(std::find(m_columns.begin(), m_columns.end(), f.field) != m_columns.end());
        boxes->push_back({f.field, b});
        ++i;
    }
    Button *cancel = c->add<Button>("Cancel");
    cancel->setFrame({248, 390, 70, 32});
    cancel->action = [this] { m_window->close(); };
    Button *ok = c->add<Button>("OK");
    ok->isDefault = true;
    ok->setFrame({330, 390, 70, 32});
    ok->action = [this, boxes] {
        // the columns kept in their order, the new ones at the end
        std::vector<int> cols;
        for (int f : m_columns) {
            if (f == 2) {
                cols.push_back(f);
                continue;
            }
            for (const auto &[field, box] : *boxes)
                if (field == f && box->checked())
                    cols.push_back(f);
        }
        for (const auto &[field, box] : *boxes)
            if (box->checked() && std::find(cols.begin(), cols.end(), field) == cols.end())
                cols.push_back(field);
        if (std::find(cols.begin(), cols.end(), 2) == cols.end())
            cols.insert(cols.begin(), 2);
        if (m_done)
            m_done(cols);
        m_window->close();
    };
    m_window->show();
}

} // namespace iTunes
