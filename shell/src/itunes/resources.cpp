// SPDX-License-Identifier: LGPL-2.1-or-later
#include "resources.h"

#include "foundation.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace AK;

namespace iTunes {

namespace {

unsigned be16(const std::string &d, size_t o) { return o + 2 <= d.size() ? (uint8_t(d[o]) << 8) | uint8_t(d[o + 1]) : 0; }
int16_t s16(const std::string &d, size_t o) { return int16_t(be16(d, o)); }
uint32_t be32(const std::string &d, size_t o) { return (uint32_t(be16(d, o)) << 16) | be16(d, o + 2); }
std::string pstring(const std::string &d, size_t o)
{
    if (o >= d.size())
        return {};
    const size_t n = uint8_t(d[o]);
    return d.substr(o + 1, std::min(n, d.size() - o - 1));
}

// Mac Roman's upper half as UTF-8 (the resources' Pascal strings: keys, font names)
std::string macRoman(const std::string &s)
{
    static const char16_t high[128] = {
        0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
        0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
        0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
        0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
        0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
        0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
        0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
        0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7};
    std::string out;
    for (unsigned char c : s) {
        unsigned cp = c < 0x80 ? c : high[c - 0x80];
        if (cp < 0x80)
            out += char(cp);
        else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string utf8(unsigned cp)
{
    std::string s;
    if (cp < 0x80)
        s += char(cp);
    else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
    return s;
}

std::string res(const std::string &name) { return std::string(Bundle) + "/Contents/Resources/" + name; }

} // namespace

bool ResourceFile::open(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string d = ss.str();
    if (d.size() < 16)
        return false;
    // header: data offset, map offset, data length, map length; the map: type list offset at 24, name
    // list offset at 26; the type list: count − 1, then type, count − 1, reference list offset
    const uint32_t dataOff = be32(d, 0), mapOff = be32(d, 4);
    if (mapOff + 28 > d.size())
        return false;
    const size_t typeList = mapOff + be16(d, mapOff + 24);
    const int types = int(be16(d, typeList)) + 1;
    for (int t = 0; t < types && types < 0x10000; ++t) {
        const size_t te = typeList + 2 + size_t(t) * 8;
        const std::string type = d.substr(te, 4);
        const int count = int(be16(d, te + 4)) + 1;
        const size_t refs = typeList + be16(d, te + 6);
        for (int r = 0; r < count; ++r) {
            const size_t re = refs + size_t(r) * 12;
            if (re + 12 > d.size())
                break;
            const int id = s16(d, re);
            const uint32_t off = (uint32_t(uint8_t(d[re + 5])) << 16) | be16(d, re + 6);
            const size_t at = dataOff + off;
            const uint32_t len = be32(d, at);
            if (at + 4 + len <= d.size())
                m_res[{type, id}] = d.substr(at + 4, len);
        }
    }
    return true;
}

const std::string *ResourceFile::get(const std::string &type, int id) const
{
    auto it = m_res.find({type, id});
    return it == m_res.end() ? nullptr : &it->second;
}

std::vector<int> ResourceFile::ids(const std::string &type) const
{
    std::vector<int> out;
    for (const auto &[k, v] : m_res)
        if (k.first == type)
            out.push_back(k.second);
    return out;
}

ResourceFile &art()
{
    static ResourceFile f = [] {
        ResourceFile r;
        r.open(Assets::path(res("iTunes.rsrc")));
        return r;
    }();
    return f;
}

ResourceFile &localized()
{
    static ResourceFile f = [] {
        ResourceFile r;
        r.open(Assets::path(res("English.lproj/Localized.rsrc")));
        return r;
    }();
    return f;
}

bool resourcesPresent() { return art().isOpen() && localized().isOpen(); }

std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }

namespace {

// A PICT's DirectBitsRect (opcode 0x009A): baseAddr, the PixMap's 46 bytes, the source and
// destination rects, the mode; then each row's byte count (one byte up to 250 row bytes, else two)
// and its PackBits, packType 4: one plane per component (cmpCount 3: red, green, blue), each as wide
// as the picture. The rows as RGB; empty when the PICT isn't one
struct Pixels {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};

Pixels directBits(const std::string &d)
{
    Pixels px;
    const auto u16 = [&](size_t at) { return at + 2 <= d.size() ? (uint8_t(d[at]) << 8) | uint8_t(d[at + 1]) : 0; };
    const size_t op = d.find(std::string("\x00\x9a\x00\x00\x00\xff", 6));
    if (op == std::string::npos)
        return px;
    size_t at = op + 2 + 4;
    const int rowBytes = u16(at) & 0x3fff;
    const int top = int16_t(u16(at + 2)), left = int16_t(u16(at + 4)), bottom = int16_t(u16(at + 6)), right = int16_t(u16(at + 8));
    const int packType = u16(at + 12), cmpCount = u16(at + 30);
    if (packType != 4 || cmpCount < 3 || right <= left || bottom <= top)
        return px;
    px.width = right - left;
    px.height = bottom - top;
    px.rgb.assign(size_t(px.width) * px.height * 3, 0);
    at += 46 + 8 + 8 + 2;
    for (int y = 0; y < px.height; ++y) {
        int count = 0;
        if (rowBytes > 250) {
            count = u16(at);
            at += 2;
        } else {
            count = at < d.size() ? uint8_t(d[at]) : 0;
            at += 1;
        }
        std::vector<uint8_t> row;
        for (size_t i = at, end = std::min(d.size(), at + count); i < end;) {
            const int c = uint8_t(d[i++]);
            if (c < 128) {
                for (int k = 0; k <= c && i < end; ++k)
                    row.push_back(uint8_t(d[i++]));
            } else if (c > 128 && i < end) {
                row.insert(row.end(), size_t(257 - c), uint8_t(d[i++]));
            }
        }
        at += count;
        for (int x = 0; x < px.width; ++x)
            for (int k = 0; k < 3; ++k) {
                const size_t from = size_t(k) * px.width + x;
                px.rgb[(size_t(y) * px.width + x) * 3 + k] = from < row.size() ? row[from] : 0;
            }
    }
    return px;
}

} // namespace

Image slider(int id)
{
    static std::map<int, Image> cache;
    auto it = cache.find(id);
    if (it != cache.end())
        return it->second;
    Image img;
    const std::string *psld = art().get("PSLD", id);
    if (psld && psld->size() >= 4) {
        const int pictureId = int16_t((uint8_t((*psld)[0]) << 8) | uint8_t((*psld)[1]));
        const int maskId = int16_t((uint8_t((*psld)[2]) << 8) | uint8_t((*psld)[3]));
        const std::string *pic = art().get("PICT", pictureId), *mask = art().get("PICT", maskId);
        const Pixels colour = pic ? directBits(*pic) : Pixels{}, alpha = mask ? directBits(*mask) : Pixels{};
        if (colour.width > 0 && colour.width == alpha.width && colour.height == alpha.height) {
            cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, colour.width, colour.height);
            cairo_surface_flush(s);
            unsigned char *data = cairo_image_surface_get_data(s);
            const int stride = cairo_image_surface_get_stride(s);
            for (int y = 0; y < colour.height; ++y)
                for (int x = 0; x < colour.width; ++x) {
                    const size_t i = (size_t(y) * colour.width + x) * 3;
                    // the mask: white leaves the picture out, black keeps it whole
                    const unsigned a = 255 - alpha.rgb[i];
                    auto *p = reinterpret_cast<uint32_t *>(data + y * stride) + x;
                    *p = (a << 24) | ((colour.rgb[i] * a / 255) << 16) | ((colour.rgb[i + 1] * a / 255) << 8) | (colour.rgb[i + 2] * a / 255);
                }
            cairo_surface_mark_dirty(s);
            img = Image(s);
        }
    }
    cache[id] = img;
    return img;
}

Image picture(int id)
{
    static std::map<int, Image> cache;
    auto it = cache.find(id);
    if (it != cache.end())
        return it->second;
    Image img;
    if (const std::string *png = art().get("PNG ", id)) {
        struct Reader {
            const std::string *data;
            size_t at = 0;
        } reader{png};
        cairo_surface_t *s = cairo_image_surface_create_from_png_stream(
            [](void *closure, unsigned char *out, unsigned int len) {
                auto *r = static_cast<Reader *>(closure);
                if (r->at + len > r->data->size())
                    return CAIRO_STATUS_READ_ERROR;
                std::memcpy(out, r->data->data() + r->at, len);
                r->at += len;
                return CAIRO_STATUS_SUCCESS;
            },
            &reader);
        if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS)
            img = Image(s);
        else
            cairo_surface_destroy(s);
    }
    cache[id] = img;
    return img;
}

PictureInfo pictureInfo(int id)
{
    // iTunes' table at 0xa622e0 (FUN_0000ebe4): columns, rows, fixed left, fixed right, kind
    static const std::map<int, PictureInfo> table = {
        {153, {2, 1, 8, 8, 1}},    {154, {1, 1, 0, 0, 0}},   {316, {5, 1, 0, 0, 5}},   {318, {5, 1, 0, 0, 5}},
        {320, {5, 3, 0, 0, 5}},    {322, {5, 1, 0, 0, 5}},   {324, {5, 1, 0, 0, 5}},   {326, {5, 3, 0, 0, 5}},
        {328, {2, 3, 0, 0, 1}},    {350, {1, 1, 0, 0, 0}},   {352, {3, 1, 0, 0, 3}},   {431, {5, 2, 0, 0, 5}},
        {432, {5, 3, 0, 0, 5}},    {433, {5, 2, 0, 0, 5}},   {434, {5, 1, 23, 14, 5}}, {436, {5, 1, 0, 0, 5}},
        {439, {5, 1, 5, 5, 5}},    {460, {5, 1, 0, 0, 5}},   {469, {5, 3, 0, 0, 5}},   {470, {5, 2, 0, 0, 5}},
        {472, {5, 1, 0, 0, 5}},    {475, {1, 1, 3, 2, 0}},   {477, {1, 1, 0, 0, 0}},   {478, {1, 1, 0, 0, 0}},
        {540, {5, 2, 0, 0, 5}},    {541, {5, 2, 0, 0, 5}},   {542, {5, 2, 0, 0, 5}},   {654, {5, 2, 0, 0, 5}},
        {441, {2, 30, 0, 0, 0}},   {304, {3, 3, 1, 16, 3}},  {306, {13, 2, 0, 0, 0}},  {300, {2, 1, 0, 0, 0}},   {13004, {4, 2, 0, 0, 4}}, {13005, {4, 2, 0, 0, 4}}, {13006, {4, 2, 0, 0, 4}},
    };
    auto it = table.find(id);
    return it == table.end() ? PictureInfo{} : it->second;
}

int stateColumn(int kind, int state)
{
    // FUN_0002b9f0's jump tables (0xa63d1c by kind, 0xa63d38 for kind 5)
    switch (kind) {
    case 1: return (state == 1 || state == 2 || state == 3 || state == 5) ? 1 : 0;
    case 2: return state == 2 ? 1 : 0;
    case 3: { static const int m[6] = {0, 1, 2, 1, 0, 1}; return state >= 0 && state < 6 ? m[state] : 0; }
    case 4: { static const int m[6] = {0, 1, 3, 2, 0, 1}; return state >= 0 && state < 6 ? m[state] : 0; }
    case 5: { static const int m[6] = {0, 1, 2, 1, 3, 4}; return state >= 0 && state < 6 ? m[state] : 0; }
    case 6: return state == 2 ? 2 : state == 3 ? 1 : 0;
    default: return 0;
    }
}

Size cellSize(int id)
{
    const Image img = picture(id);
    const PictureInfo info = pictureInfo(id);
    return {double(img.width()) / info.columns, double(img.height()) / info.rows};
}

void drawCell(cairo_t *cr, int id, int column, int row, const Rect &dst, double alpha)
{
    const Image img = picture(id);
    if (img.isNull() || dst.empty())
        return;
    const PictureInfo info = pictureInfo(id);
    const Size cell = cellSize(id);
    const Rect src{column * cell.w, row * cell.h, cell.w, cell.h};
    const double l = std::min<double>(info.fixedLeft, dst.w / 2), r = std::min<double>(info.fixedRight, dst.w / 2);
    if ((l == 0 && r == 0) || dst.w == cell.w) {
        img.draw(cr, src, dst, alpha);
        return;
    }
    img.draw(cr, {src.x, src.y, l, src.h}, {dst.x, dst.y, l, dst.h}, alpha);
    img.draw(cr, {src.x + l, src.y, src.w - l - r, src.h}, {dst.x + l, dst.y, dst.w - l - r, dst.h}, alpha);
    img.draw(cr, {src.right() - r, src.y, r, src.h}, {dst.right() - r, dst.y, r, dst.h}, alpha);
}

Color colour(int index)
{
    static const unsigned char table[44][3] = {
        {64, 64, 64},    {135, 135, 135}, {224, 224, 224}, {135, 135, 135}, {85, 85, 85},    {135, 135, 135},
        {147, 147, 147}, {240, 240, 240}, {224, 224, 224}, {224, 224, 224}, {61, 128, 223},  {192, 192, 192},
        {204, 204, 204}, {32, 32, 32},    {241, 245, 250}, {238, 238, 238}, {255, 255, 255}, {125, 170, 234},
        {224, 224, 224}, {52, 109, 190},  {172, 172, 172}, {0, 0, 0},       {255, 255, 255}, {0, 0, 0},
        {67, 135, 232},  {50, 161, 221},  {236, 236, 236}, {209, 215, 226}, {232, 232, 232}, {80, 93, 109},
        {93, 93, 93},    {243, 245, 248}, {0, 0, 0},       {255, 255, 255}, {27, 67, 121},   {83, 95, 120},
        {187, 206, 241}, {178, 178, 178}, {58, 117, 193},  {136, 153, 188}, {255, 255, 255}, {105, 108, 113},
        {255, 255, 255}, {0, 0, 0}};
    if (index < 0 || index >= 44)
        return Color::gray(0);
    return Color::rgb(table[index][0], table[index][1], table[index][2]);
}

TextStyle textStyle(int id)
{
    // Txtr: size, style (1 bold), justification (1 centred, −1 right), 1, red, green, blue, 0xFFFF,
    // the font's name
    TextStyle s;
    s.font = Font::system(11);
    const std::string *d = localized().get("Txtr", id);
    if (!d || d->size() < 17)
        return s;
    s.font.size = s16(*d, 0);
    s.font.bold = s16(*d, 2) & 1;
    const int just = s16(*d, 4);
    s.align = just == 1 ? Align::Center : just == -1 ? Align::Right : Align::Left;
    s.color = Color::rgb(be16(*d, 8) >> 8, be16(*d, 10) >> 8, be16(*d, 12) >> 8);
    const std::string family = macRoman(pstring(*d, 16));
    if (family != "Lucida Grande")
        s.font.family = family;
    return s;
}

namespace {

// An xMNU item's text: the key's Localizable.strings value ("##" and the hand-made " space" column
// dropped)
std::string itemTitle(const std::string &key)
{
    std::string k = key.rfind("##", 0) == 0 ? key.substr(2) : key;
    std::string t = tr(k);
    const size_t gap = t.find("  ");
    if (gap != std::string::npos)
        t = t.substr(0, gap);
    return t;
}

std::string fourCC(const std::string &d, size_t o)
{
    std::string s = d.substr(o, 4);
    return s == "????" || s == std::string(4, '\0') ? std::string() : s;
}

std::vector<MainMenuItem> parseMenu(int id, int depth)
{
    std::vector<MainMenuItem> items;
    const std::string *dp = localized().get("xMNU", id);
    if (!dp || dp->size() < 0x154 || depth > 4)
        return items;
    const std::string &d = *dp;
    const int count = s16(d, 0x152);
    size_t p = 0x154;
    for (int i = 0; i < count && p + 0x62 <= d.size(); ++i) {
        const int len = s16(d, p);
        const std::string key = macRoman(pstring(d, p + 0x60));
        MainMenuItem it;
        if (key == "kMenuItemDivider") {
            it.separator = true;
        } else {
            it.title = itemTitle(key);
            // the command: iTunes' own at +2, else HIToolbox's at +0xC
            it.action = fourCC(d, p + 2);
            if (it.action.empty())
                it.action = fourCC(d, p + 0xC);
            const unsigned ch = be16(d, p + 6);
            const unsigned mark = uint8_t(d[p + 8]);
            const unsigned glyph = be16(d, p + 0xA);
            const unsigned mods = uint8_t(d[p + 0x14]);
            if (ch == 0x1B && mark != 0x20 && mark != 0) {
                // hMenuCmd: the mark is the submenu's xMNU
                it.hasSubmenu = true;
                it.items = parseMenu(int(mark), depth + 1);
            } else if (ch == 0xF0 && glyph) {
                // the arrow glyphs (kMenu…ArrowDashedGlyph): ← 24, ↑ 25, → 26, ↓ 16
                const unsigned arrow = glyph == 24 ? 0xF702 : glyph == 25 ? 0xF700 : glyph == 26 ? 0xF703 : glyph == 16 ? 0xF701 : 0;
                if (arrow)
                    it.key = utf8(arrow);
            } else if (ch > 0x20 && ch < 0x7F) {
                it.key = std::string(1, char(std::tolower(int(ch))));
            }
            if (!it.key.empty()) {
                // SetMenuItemModifiers: 1 Shift, 2 Option, 4 Control, 8 no Command
                it.modifiers = (mods & 8) ? 0 : unsigned(Command);
                if (mods & 1)
                    it.modifiers |= Shift;
                if (mods & 2)
                    it.modifiers |= Option;
                if (mods & 4)
                    it.modifiers |= Control;
            }
            if (uint8_t(d[p + 0x1A]))
                it.action.clear();   // DisableMenuItem: a title (the Dock menu's Now Playing)
            // the same command again with Option: shown in place of the one before with Option
            if (!items.empty() && !items.back().separator && items.back().action == it.action && !it.action.empty() &&
                (mods & 2))
                it.alternate = true;
        }
        items.push_back(it);
        p += size_t(len) + 2;
    }
    return items;
}

} // namespace

std::vector<MainMenuItem> menuItems(int id) { return parseMenu(id, 0); }

std::vector<MainMenuItem> menuBar()
{
    std::vector<MainMenuItem> bar;
    const std::string *mbar = localized().get("MBAR", 129);
    if (!mbar)
        return bar;
    const int n = s16(*mbar, 0);
    for (int i = 0; i < n; ++i) {
        const int id = s16(*mbar, 2 + 2 * size_t(i));
        const std::string *d = localized().get("xMNU", id);
        if (!d)
            continue;
        MainMenuItem m;
        m.hasSubmenu = true;
        m.items = parseMenu(id, 0);
        if (i == 0) {
            // the Apple menu's items, which Mac OS X puts in the application's menu, with its own
            m.title = "iTunes";
            MainMenuItem sep;
            sep.separator = true;
            MainMenuItem services;
            services.title = "Services";
            services.hasSubmenu = true;
            auto item = [](const std::string &title, const std::string &key, unsigned mods, const std::string &action) {
                MainMenuItem it;
                it.title = title;
                it.key = key;
                it.modifiers = mods;
                it.action = action;
                return it;
            };
            m.items.push_back(sep);
            m.items.push_back(services);
            m.items.push_back(sep);
            m.items.push_back(item("Hide iTunes", "h", Command, "hide:"));
            m.items.push_back(item("Hide Others", "h", Command | Option, "hideOtherApplications:"));
            m.items.push_back(item("Show All", "", 0, "unhideAllApplications:"));
            m.items.push_back(sep);
            m.items.push_back(item("Quit iTunes", "q", Command, "terminate:"));
        } else {
            m.title = tr(macRoman(pstring(*d, 0)));
        }
        bar.push_back(m);
    }
    // the hidden "Commands" menu: its keys (⌘↑ ⌘↓, ⌥⌘F...) work, it isn't shown
    MainMenuItem hidden;
    hidden.title = tr("kHiddenMenuTitle");
    hidden.hasSubmenu = true;
    hidden.hidden = true;
    hidden.items = parseMenu(160, 0);
    bar.push_back(hidden);
    return bar;
}

} // namespace iTunes
