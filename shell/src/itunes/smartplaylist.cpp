// SPDX-License-Identifier: LGPL-2.1-or-later
// The Smart Playlist window as SmartPlaylist.nib lays it out (656 × 229 with one rule):
//   "Match" (4001, 20, 19) with the all / any popup (4056, MENU 303) at 85 and "of the following
//   rules:" at 163; the rules' group box (4054) at (22, 47), 610 wide, a row (the nib's
//   singleRuleRow, a pane 610 × 36) per rule: the field's popup (4014: 10, 10, 162 × 20), the
//   operator's (4016: 180, 10, 157 × 20; MENU 301 text, 302 numeric, 308 date, 306 boolean, 307
//   enumerated), the value's pane (4058: 345, 4, 192 × 32: textCriterionView's field 6, 8, 180 × 16;
//   textRangeCriterionView's two 70 wide with "to"; betweenCriterionView's 45-point field and MENU 309's
//   days / weeks / months; sizeCriterionView's "MB", bitRateCriterionView's "kbps",
//   sampleRateCriterionView's "Hz", userRatingCriterionView's stars, PBUT 531), then − and + (PBUT
//   138: 32 × 32 cells, three columns disabled, normal, pressed; row 0 −, row 1 +) at 541 and 571.
//   Under the rules the pane 4053 (5, …, 656 × 137): "Limit to" (15, 11), its number (95, 12, 37), its
//   unit (143, 10, 120: MENU 304 minutes, hours, MB, GB, items), "selected by" (271, 12) and the order
//   (349, 10, 230: MENU 305), "Match only checked items" (15, 39), "Live updating" (15, 65), Cancel
//   (479, 97) and OK (561, 97).
// The fields are the nib's popup's (only those the library keeps are offered); their numbers are
// iTunes' (the search field's rules confirm 2 Name, 3 Album, 4 Artist, 0x47 Album Artist with
// 0x01000002 "contains": the search field's code at 0x1060e7 calling FUN_0037c820); the other operators' codes and fields'
// numbers are the format's as known, not read from this binary.
#include "smartplaylist.h"

#include "controls.h"
#include "resources.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <random>

using namespace AK;

namespace iTunes {

// ---- the fields and operators

const std::vector<SmartField> &smartFields()
{
    using K = SmartField::Kind;
    static const std::vector<SmartField> fields = {
        {0x03, "Album", K::Text},        {0x47, "Album Artist", K::Text}, {0x04, "Artist", K::Text},
        {0x23, "BPM", K::Number},        {0x05, "Bit Rate", K::BitRate},  {0x0e, "Comment", K::Text},
        {0x12, "Composer", K::Text},     {0x10, "Date Added", K::Date},   {0x18, "Disc Number", K::Number},
        {0x08, "Genre", K::Text},        {0x27, "Grouping", K::Text},     {0x09, "Kind", K::Text},
        {0x19, "My Rating", K::Rating},  {0x02, "Name", K::Text},         {0x16, "Play Count", K::Number},
        {0x06, "Sample Rate", K::SampleRate}, {0x0c, "Size", K::Size},    {0x0d, "Time", K::Time},
        {0x0b, "Track Number", K::Number}, {0x07, "Year", K::Number},
    };
    return fields;
}

const SmartField *smartField(int id)
{
    for (const SmartField &f : smartFields())
        if (f.id == id)
            return &f;
    return nullptr;
}

namespace {

enum : uint32_t {
    Contains = 0x01000002, NotContains = 0x03000002, Is = 0x01000001, IsNot = 0x03000001,
    StartsWith = 0x01000004, EndsWith = 0x01000008,
    NumIs = 0x00000001, NumIsNot = 0x02000001, Greater = 0x00000010, Less = 0x00000040, Range = 0x00000100,
    InLast = 0x00000200, NotInLast = 0x02000200,
};

bool isText(SmartField::Kind k) { return k == SmartField::Text; }
bool isDate(SmartField::Kind k) { return k == SmartField::Date; }

} // namespace

const std::vector<uint32_t> &smartOperators(SmartField::Kind kind)
{
    static const std::vector<uint32_t> text = {Contains, NotContains, Is, IsNot, StartsWith, EndsWith};       // MENU 301
    static const std::vector<uint32_t> numeric = {NumIs, NumIsNot, Greater, Less, Range};                     // MENU 302
    static const std::vector<uint32_t> date = {NumIs, NumIsNot, Greater, Less, InLast, NotInLast, Range};     // MENU 308
    return isText(kind) ? text : isDate(kind) ? date : numeric;
}

std::vector<std::string> smartOperatorTitles(SmartField::Kind kind)
{
    if (isText(kind))
        return {"contains", "does not contain", "is", "is not", "starts with", "ends with"};
    if (isDate(kind))
        return {"is", "is not", "is after", "is before", "is in the last", "is not in the last", "is in the range"};
    return {"is", "is not", "is greater than", "is less than", "is in the range"};
}

// ---- the rules applied

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

long long dayOf(const std::string &iso)   // "2026-11-20T07:28:42Z" → days since 1970
{
    std::tm tm{};
    if (iso.size() < 10 || std::sscanf(iso.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (long long)(timegm(&tm) / 86400);
}

long long numberOf(const Track &t, const SmartField &f)
{
    switch (f.id) {
    case 0x23: return t.bpm;
    case 0x05: return t.bitRate;
    case 0x10: return dayOf(t.dateAdded);
    case 0x18: return t.discNumber;
    case 0x19: return t.rating;
    case 0x16: return t.playCount;
    case 0x06: return t.sampleRate;
    case 0x0c: return t.size / 1048576;      // MB
    case 0x0d: return t.totalTime / 1000;    // seconds
    case 0x0b: return t.trackNumber;
    case 0x07: return t.year;
    }
    return 0;
}

std::string textOf(const Track &t, int id)
{
    switch (id) {
    case 0x03: return t.album;
    case 0x47: return t.albumArtist;
    case 0x04: return t.artist;
    case 0x0e: return t.comments;
    case 0x12: return t.composer;
    case 0x08: return t.genre;
    case 0x27: return t.grouping;
    case 0x09: return t.kind;
    case 0x02: return t.name;
    }
    return {};
}

bool matches(const Track &t, const SmartRule &r)
{
    const SmartField *f = smartField(r.field);
    if (!f)
        return false;
    if (isText(f->kind)) {
        const std::string v = lower(textOf(t, r.field)), q = lower(r.text);
        switch (r.op) {
        case Contains: return v.find(q) != std::string::npos;
        case NotContains: return v.find(q) == std::string::npos;
        case Is: return v == q;
        case IsNot: return v != q;
        case StartsWith: return v.rfind(q, 0) == 0;
        case EndsWith: return v.size() >= q.size() && v.compare(v.size() - q.size(), q.size(), q) == 0;
        }
        return false;
    }
    const long long v = numberOf(t, *f);
    switch (r.op) {
    case NumIs: return v == r.from;
    case NumIsNot: return v != r.from;
    case Greater: return v > r.from;
    case Less: return v < r.from;
    case Range: return v >= std::min(r.from, r.to) && v <= std::max(r.from, r.to);
    case InLast:
    case NotInLast: {
        const long long today = (long long)(std::time(nullptr) / 86400);
        const bool in = v > 0 && v >= today - r.from * r.unit;
        return r.op == InLast ? in : !in;
    }
    }
    return false;
}

} // namespace

std::vector<int> Library::evaluate(const SmartInfo &info) const
{
    std::vector<int> out;
    for (const auto &[id, t] : m_tracks) {
        bool ok = true;
        if (info.useRules && !info.rules.empty()) {
            ok = !info.any;
            for (const SmartRule &r : info.rules) {
                const bool m = matches(t, r);
                if (info.any && m) {
                    ok = true;
                    break;
                }
                if (!info.any && !m) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok)
            out.push_back(id);
    }
    if (!info.limit)
        return out;
    // the order (MENU 305): random, album, artist, genre, name, highest / lowest rating, most / least
    // recently played (no play dates kept: the play count), most / least often played, most / least
    // recently added
    auto get = [&](int id) -> const Track & { return m_tracks.at(id); };
    switch (info.limitOrder) {
    case 0: std::shuffle(out.begin(), out.end(), std::mt19937(std::random_device{}())); break;
    case 1: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return lower(get(a).album) < lower(get(b).album); }); break;
    case 2: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return lower(get(a).artist) < lower(get(b).artist); }); break;
    case 3: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return lower(get(a).genre) < lower(get(b).genre); }); break;
    case 4: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return lower(get(a).name) < lower(get(b).name); }); break;
    case 5: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return get(a).rating > get(b).rating; }); break;
    case 6: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return get(a).rating < get(b).rating; }); break;
    case 7: case 9: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return get(a).playCount > get(b).playCount; }); break;
    case 8: case 10: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return get(a).playCount < get(b).playCount; }); break;
    case 11: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return get(a).dateAdded > get(b).dateAdded; }); break;
    case 12: std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return get(a).dateAdded < get(b).dateAdded; }); break;
    }
    // the limit: minutes, hours, MB, GB or items
    std::vector<int> kept;
    long long used = 0;
    const long long cap = info.limitUnit == 0 ? info.limitValue * 60000LL
                        : info.limitUnit == 1 ? info.limitValue * 3600000LL
                        : info.limitUnit == 2 ? info.limitValue * 1048576LL
                        : info.limitUnit == 3 ? info.limitValue * 1073741824LL
                                              : info.limitValue;
    for (int id : out) {
        const Track &t = get(id);
        const long long cost = info.limitUnit <= 1 ? t.totalTime : info.limitUnit <= 3 ? t.size : 1;
        if (used + cost > cap)
            break;
        used += cost;
        kept.push_back(id);
    }
    return kept;
}

// ---- the window

namespace {

constexpr double RowH = 36, BoxY = 47, PaneH = 137;

std::vector<MenuItem> menuOf(const std::vector<std::string> &titles)
{
    std::vector<MenuItem> items;
    for (const std::string &t : titles)
        items.push_back(t == "-" ? MenuItem::line() : MenuItem(t));
    return items;
}

// userRatingCriterionView: the stars (PBUT 531's rows, a half star each), a click sets them
class StarField : public View {
public:
    int rating = 0;
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(255));
        drawCell(cr, 531, 0, 0, Rect{0, 0, frame().w, frame().h});
        drawCell(cr, 528, 0, (rating + 5) / 10, Rect{3, std::floor((frame().h - 11) / 2), 68, 11});
    }
    void mouseDown(Event &e) override
    {
        rating = std::clamp(int(std::ceil((e.location.x - 3) / (68.0 / 5))), 0, 5) * 20;
        setNeedsDisplay();
    }
};

// the − and + buttons: PBUT 138
class PlusMinus : public View {
public:
    explicit PlusMinus(int row) : m_row(row) {}
    std::function<void()> onClick;
    void draw(cairo_t *cr) override { drawCell(cr, 138, isEnabled() ? (m_pressed ? 2 : 1) : 0, m_row, bounds()); }
    void mouseDown(Event &) override
    {
        if (!isEnabled())
            return;
        m_pressed = true;
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        const bool fire = m_pressed && bounds().contains(e.location);
        m_pressed = false;
        setNeedsDisplay();
        if (fire && onClick)
            onClick();
    }

private:
    int m_row;
    bool m_pressed = false;
};

} // namespace

struct SmartPlaylistWindow::RuleRow {
    View *pane = nullptr;
    PopUpButton *field = nullptr, *op = nullptr;
    View *value = nullptr;
    TextField *a = nullptr, *b = nullptr;
    PopUpButton *unit = nullptr;
    StarField *starsA = nullptr, *starsB = nullptr;
    PlusMinus *minus = nullptr, *plus = nullptr;
    SmartRule rule;
};

struct SmartPlaylistWindow::Controls {
    CheckBox *match = nullptr;
    PopUpButton *anyAll = nullptr;
    Label *following = nullptr;
    Box *box = nullptr;
    View *pane = nullptr;
    CheckBox *limit = nullptr, *checked = nullptr, *live = nullptr;
    TextField *limitValue = nullptr;
    PopUpButton *limitUnit = nullptr, *order = nullptr;
};

SmartPlaylistWindow::SmartPlaylistWindow(const SmartInfo &info, std::function<void(const SmartInfo &)> done)
    : m_info(info), m_done(std::move(done))
{
    if (m_info.rules.empty())
        m_info.rules.push_back(SmartRule{});
}

SmartPlaylistWindow::~SmartPlaylistWindow() = default;

void SmartPlaylistWindow::show()
{
    if (!m_window)
        build();
    m_window->orderFront();
}

namespace {

// a value's text: a number, a time "m:ss", a size in MB
std::string valueText(const SmartField &f, long long v)
{
    if (f.kind == SmartField::Time) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld:%02lld", v / 60, v % 60);
        return buf;
    }
    if (f.kind == SmartField::Date) {
        const std::time_t t = std::time_t(v * 86400);
        char buf[32];
        std::strftime(buf, sizeof buf, "%m/%d/%Y", std::gmtime(&t));
        return buf;
    }
    return std::to_string(v);
}

long long parseValue(const SmartField &f, const std::string &s)
{
    if (f.kind == SmartField::Time) {
        const size_t c = s.find(':');
        return c == std::string::npos ? std::atoll(s.c_str()) : std::atoll(s.c_str()) * 60 + std::atoll(s.c_str() + c + 1);
    }
    if (f.kind == SmartField::Date) {
        std::tm tm{};
        if (std::sscanf(s.c_str(), "%d/%d/%d", &tm.tm_mon, &tm.tm_mday, &tm.tm_year) != 3)
            return 0;
        tm.tm_mon -= 1;
        tm.tm_year -= 1900;
        return (long long)(timegm(&tm) / 86400);
    }
    return std::atoll(s.c_str());
}

} // namespace

void SmartPlaylistWindow::build()
{
    m_window = std::make_unique<Window>(Size{656, 229}, "Smart Playlist");
    m_window->setStyleMask(2);
    m_window->onClose = [this] { App::shared().later([this] { m_window.reset(); }); };
    View *c = m_window->contentView();
    m_c = std::make_unique<Controls>();
    Controls &k = *m_c;
    k.match = c->add<CheckBox>("Match");
    k.match->setFrame({20, 19, 62, 22});
    k.match->setChecked(m_info.useRules);
    k.anyAll = c->add<PopUpButton>();
    k.anyAll->items = menuOf({"all", "any"});   // MENU 303
    k.anyAll->setSelected(m_info.any ? 1 : 0);
    k.anyAll->setFrame({85, 18, 70, 20});
    k.following = c->add<Label>("of the following rules:", Font::system(13));
    k.following->setFrame({163, 20, 144, 18});
    k.box = c->add<Box>();
    k.box->kind = Box::Secondary;
    k.pane = c->add<View>();
    k.limit = k.pane->add<CheckBox>("Limit to");
    k.limit->setFrame({15, 11, 69, 18});
    k.limit->setChecked(m_info.limit);
    k.limitValue = k.pane->add<TextField>();
    k.limitValue->setFrame({92, 9, 43, 22});
    k.limitValue->setText(std::to_string(m_info.limitValue));
    k.limitUnit = k.pane->add<PopUpButton>();
    k.limitUnit->items = menuOf({"minutes", "hours", "MB", "GB", "items"});   // MENU 304
    k.limitUnit->setSelected(m_info.limitUnit);
    k.limitUnit->setFrame({143, 10, 120, 20});
    Label *by = k.pane->add<Label>("selected by", Font::system(13));
    by->setFrame({271, 12, 78, 16});
    k.order = k.pane->add<PopUpButton>();
    // MENU 305
    k.order->items = menuOf({"random", "-", "album", "artist", "genre", "name", "-", "highest rating", "lowest rating", "-",
                             "most recently played", "least recently played", "-", "most often played", "least often played", "-",
                             "most recently added", "least recently added"});
    static const int orderItem[13] = {0, 2, 3, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17};
    k.order->setSelected(orderItem[std::clamp(m_info.limitOrder, 0, 12)]);
    k.order->setFrame({349, 10, 230, 20});
    k.checked = k.pane->add<CheckBox>("Match only checked items");
    k.checked->setFrame({15, 39, 184, 18});
    k.checked->setChecked(m_info.checkedOnly);
    k.checked->setEnabled(false);   // no check boxes on the songs yet
    k.live = k.pane->add<CheckBox>("Live updating");
    k.live->setFrame({15, 65, 106, 18});
    k.live->setChecked(m_info.live);
    Button *cancel = k.pane->add<Button>("Cancel");
    cancel->setFrame({479, 91, 70, 32});
    cancel->action = [this] { m_window->close(); };
    Button *ok = k.pane->add<Button>("OK");
    ok->isDefault = true;
    ok->setFrame({561, 91, 70, 32});
    ok->action = [this] {
        const SmartInfo info = collect();
        if (m_done)
            m_done(info);
        m_window->close();
    };
    for (size_t i = 0; i < m_info.rules.size(); ++i) {
        m_rows.push_back(std::make_unique<RuleRow>());
        m_rows.back()->rule = m_info.rules[i];
    }
    layoutRows();
    m_window->show();
}

// The rows rebuilt (a field or an operator changed, a row added or removed), the window's height to fit
void SmartPlaylistWindow::layoutRows()
{
    Controls &k = *m_c;
    // keep what the rows' controls hold (their field and operator are the rows' own), then rebuild
    if (!m_rows.empty() && m_rows[0]->pane) {
        const std::vector<SmartRule> rules = collect().rules;
        for (size_t i = 0; i < m_rows.size(); ++i)
            m_rows[i]->rule = rules[i];
    }
    for (auto &r : m_rows)
        if (r->pane) {
            k.box->removeSubview(r->pane);
            r->pane = nullptr;
        }
    const int n = int(m_rows.size());
    const double boxH = 4 + RowH * n;
    k.box->setFrame({22, BoxY, 610, boxH});
    for (int i = 0; i < n; ++i) {
        RuleRow &row = *m_rows[size_t(i)];
        const SmartField *f = smartField(row.rule.field);
        if (!f) {
            row.rule.field = 0x04;
            f = smartField(0x04);
        }
        const auto &ops = smartOperators(f->kind);
        if (std::find(ops.begin(), ops.end(), row.rule.op) == ops.end())
            row.rule.op = ops[0];
        row.pane = k.box->add<View>();
        row.pane->setFrame({0, 2 + RowH * i, 610, RowH});
        row.field = row.pane->add<PopUpButton>();
        for (const SmartField &sf : smartFields())
            row.field->items.emplace_back(sf.title);
        const auto &all = smartFields();
        row.field->setSelected(int(std::find_if(all.begin(), all.end(), [&](const SmartField &x) { return x.id == f->id; }) - all.begin()));
        row.field->setFrame({10, 8, 162, 20});
        row.field->onSelect = [this, i](int sel) {
            m_rows[size_t(i)]->rule.field = smartFields()[size_t(sel)].id;
            App::shared().later([this] { layoutRows(); });   // the popup itself is rebuilt
        };
        row.op = row.pane->add<PopUpButton>();
        row.op->items = menuOf(smartOperatorTitles(f->kind));
        row.op->setSelected(int(std::find(ops.begin(), ops.end(), row.rule.op) - ops.begin()));
        row.op->setFrame({180, 8, 157, 20});
        row.op->onSelect = [this, i](int sel) {
            const SmartField *ff = smartField(m_rows[size_t(i)]->rule.field);
            m_rows[size_t(i)]->rule.op = smartOperators(ff->kind)[size_t(sel)];
            App::shared().later([this] { layoutRows(); });
        };
        // the value's pane at (345, 4), 192 × 32
        row.value = row.pane->add<View>();
        row.value->setFrame({345, 2, 192, 32});
        row.a = row.b = nullptr;
        row.unit = nullptr;
        row.starsA = row.starsB = nullptr;
        const bool range = row.rule.op == Range;
        const bool last = row.rule.op == InLast || row.rule.op == NotInLast;
        auto suffix = [&](const char *s, double x) {
            Label *l = row.value->add<Label>(s, Font::system(13));
            l->setFrame({x, 8, 40, 16});
        };
        if (f->kind == SmartField::Rating) {
            row.starsA = row.value->add<StarField>();
            row.starsA->rating = int(row.rule.from);
            row.starsA->setFrame({5, 8, 74, 17});
            if (range) {
                suffix("to", 85);
                row.starsB = row.value->add<StarField>();
                row.starsB->rating = int(row.rule.to);
                row.starsB->setFrame({110, 8, 74, 17});
            }
        } else if (last) {
            // betweenCriterionView: a number and days / weeks / months
            row.a = row.value->add<TextField>();
            row.a->setFrame({3, 5, 51, 22});
            row.a->setText(std::to_string(row.rule.from));
            row.unit = row.value->add<PopUpButton>();
            row.unit->items = menuOf({"days", "weeks", "months"});   // MENU 309
            row.unit->setSelected(row.rule.unit >= 30 ? 2 : row.rule.unit >= 7 ? 1 : 0);
            row.unit->setFrame({57, 6, 90, 20});
        } else if (isText(f->kind)) {
            row.a = row.value->add<TextField>();
            row.a->setFrame({3, 5, 186, 22});
            row.a->setText(row.rule.text);
        } else {
            const char *unit = f->kind == SmartField::Size ? "MB" : f->kind == SmartField::BitRate ? "kbps"
                             : f->kind == SmartField::SampleRate ? "Hz" : nullptr;
            row.a = row.value->add<TextField>();
            row.a->setText(valueText(*f, row.rule.from));
            if (range) {
                row.a->setFrame({3, 5, 70, 22});
                suffix("to", 84);
                row.b = row.value->add<TextField>();
                row.b->setFrame({105, 5, unit ? 54.0 : 80.0, 22});
                row.b->setText(valueText(*f, row.rule.to));
            } else {
                row.a->setFrame({3, 5, unit ? 157.0 : 186.0, 22});
            }
            if (unit)
                suffix(unit, 164);
        }
        row.minus = row.pane->add<PlusMinus>(0);
        row.minus->setFrame({541, 2, 32, 32});
        row.minus->setEnabled(n > 1);
        row.minus->onClick = [this, i] { App::shared().later([this, i] { removeRow(i); }); };
        row.plus = row.pane->add<PlusMinus>(1);
        row.plus->setFrame({571, 2, 32, 32});
        row.plus->onClick = [this, i] { App::shared().later([this, i] { addRow(i); }); };
    }
    k.pane->setFrame({5, BoxY + boxH + 5, 656, PaneH});
    m_window->setContentSize({656, BoxY + boxH + 5 + PaneH - 8});
    m_window->contentView()->setNeedsDisplay();
}

void SmartPlaylistWindow::addRow(int after)
{
    const std::vector<SmartRule> rules = collect().rules;
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_rows[i]->rule = rules[i];
    for (auto &r : m_rows)
        if (r->pane) {
            m_c->box->removeSubview(r->pane);
            r->pane = nullptr;
        }
    auto row = std::make_unique<RuleRow>();
    row->rule = m_rows[size_t(after)]->rule;   // a new rule takes its field from the one above
    row->rule.text.clear();
    m_rows.insert(m_rows.begin() + after + 1, std::move(row));
    layoutRows();
}

void SmartPlaylistWindow::removeRow(int index)
{
    if (m_rows.size() <= 1)
        return;
    std::vector<SmartRule> rules = collect().rules;
    rules.erase(rules.begin() + index);
    for (auto &r : m_rows)
        if (r->pane) {
            m_c->box->removeSubview(r->pane);
            r->pane = nullptr;
        }
    m_rows.erase(m_rows.begin() + index);
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_rows[i]->rule = rules[i];
    layoutRows();
}

SmartInfo SmartPlaylistWindow::collect() const
{
    SmartInfo info = m_info;
    const Controls &k = *m_c;
    info.useRules = k.match->checked();
    info.any = k.anyAll->selected() == 1;
    info.limit = k.limit->checked();
    info.limitValue = std::max(0, std::atoi(k.limitValue->text.c_str()));
    info.limitUnit = k.limitUnit->selected();
    static const int orderOf[18] = {0, 0, 1, 2, 3, 4, 4, 5, 6, 6, 7, 8, 8, 9, 10, 10, 11, 12};
    info.limitOrder = orderOf[std::clamp(k.order->selected(), 0, 17)];
    info.checkedOnly = k.checked->checked();
    info.live = k.live->checked();
    info.rules.clear();
    for (const auto &r : m_rows) {
        SmartRule rule = r->rule;
        const SmartField *f = smartField(rule.field);
        if (r->pane && f) {
            if (r->starsA)
                rule.from = r->starsA->rating;
            if (r->starsB)
                rule.to = r->starsB->rating;
            if (r->a && isText(f->kind))
                rule.text = r->a->text;
            else if (r->a && r->unit)
                rule.from = std::atoll(r->a->text.c_str());
            else if (r->a)
                rule.from = parseValue(*f, r->a->text);
            if (r->b)
                rule.to = parseValue(*f, r->b->text);
            if (r->unit)
                rule.unit = r->unit->selected() == 2 ? 30 : r->unit->selected() == 1 ? 7 : 1;
        }
        info.rules.push_back(rule);
    }
    return info;
}

} // namespace iTunes
