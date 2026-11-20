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

bool resourcesPresent() { return art().isOpen() && (eleven() || localized().isOpen()); }

int majorVersion()
{
    static const int v = std::atoi(Assets::plistStrings(std::string(Bundle) + "/Contents/Info.plist")["CFBundleShortVersionString"].c_str());
    return v;
}

std::string tr(const std::string &key)
{
    // 11.4 numbers its texts by the STR# resources 8.2.1 had them in: "9006.001" is (9006 << 16) | 1
    if (eleven()) {
        const size_t dot = key.find('.');
        if (dot != std::string::npos && dot > 0 && key.find_first_not_of("0123456789.") == std::string::npos) {
            const std::string t = text((uint32_t(std::stoul(key.substr(0, dot))) << 16) | uint32_t(std::stoul(key.substr(dot + 1))));
            return t.empty() ? key : t;
        }
    }
    return Assets::tr(Bundle, "Localizable", key, key);
}

std::string text(uint32_t number)
{
    // LocalizableStringsMap.plist: hashed key -> number (iTunes looks a number's key up in it)
    static const std::map<uint32_t, std::string> keys = [] {
        std::map<uint32_t, std::string> m;
        for (const auto &[key, n] : Assets::plistStrings(res("LocalizableStringsMap.plist")))
            m[uint32_t(std::strtoul(n.c_str(), nullptr, 10))] = key;
        return m;
    }();
    auto it = keys.find(number);
    return it == keys.end() ? std::string() : Assets::tr(Bundle, "Localizable", it->second, {});
}

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
    // 11.4's table (built by the initializer at 0xa79b59, read by FUN_00a76eb8): the entries the window
    // draws. Their art is new: the same numbers no longer name the same pictures.
    static const std::map<int, PictureInfo> table11 = {
        {150, {1, 1, 4, 4, 0}},    {151, {1, 1, 0, 0, 0}},   {160, {1, 3, 10, 10, 0}}, {161, {3, 1, 0, 0, 1}},
        {317, {5, 1, 0, 0, 5}},    {319, {5, 1, 0, 0, 5}},   {321, {5, 3, 0, 0, 5}},   {328, {3, 4, 0, 0, 3}},
        {350, {1, 2, 10, 10, 0}},  {352, {3, 1, 0, 0, 3}},   {441, {4, 53, 0, 0, 3}},  {5000, {2, 1, 4, 4, 1}},
        {5001, {2, 1, 4, 4, 1}},   {13004, {4, 2, 0, 0, 4}}, {13005, {4, 2, 0, 0, 4}}, {13006, {4, 2, 0, 0, 4}},
        // the nav bar's
        {869, {3, 1, 11, 11, 3}},  {870, {1, 1, 0, 0, 0}},   {871, {3, 1, 11, 11, 3}}, {876, {3, 2, 13, 13, 3}},
        {879, {1, 1, 0, 0, 0}},    {882, {3, 1, 0, 0, 3}},   {883, {3, 1, 0, 0, 3}},   {887, {1, 1, 0, 0, 0}},
        {8251, {2, 13, 0, 0, 3}},  {886, {4, 2, 11, 14, 4}},
        // the songs' header (304, 313 right to left) and status icons (FUN_00e5675c)
        {304, {3, 4, 2, 16, 3}},   {313, {3, 4, 16, 2, 3}},  {8455, {2, 10, 0, 0, 0}},
    };
    if (eleven()) {
        auto it = table11.find(id);
        return it == table11.end() ? PictureInfo{} : it->second;
    }
    // iTunes 8.2.1's table at 0xa622e0 (FUN_0000ebe4): columns, rows, fixed left, fixed right, kind
    static const std::map<int, PictureInfo> table = {
        {153, {2, 1, 8, 8, 1}},    {154, {1, 1, 0, 0, 0}},   {316, {5, 1, 0, 0, 5}},   {318, {5, 1, 0, 0, 5}},
        {320, {5, 3, 0, 0, 5}},    {322, {5, 1, 0, 0, 5}},   {324, {5, 1, 0, 0, 5}},   {326, {5, 3, 0, 0, 5}},
        {328, {2, 3, 0, 0, 1}},    {350, {1, 1, 0, 0, 0}},   {352, {3, 1, 0, 0, 3}},   {431, {5, 2, 0, 0, 5}},
        {432, {5, 3, 0, 0, 5}},    {433, {5, 2, 0, 0, 5}},   {434, {5, 1, 23, 14, 5}}, {436, {5, 1, 0, 0, 5}},
        {439, {5, 1, 5, 5, 5}},    {460, {5, 1, 0, 0, 5}},   {469, {5, 3, 0, 0, 5}},   {470, {5, 2, 0, 0, 5}},
        {472, {5, 1, 0, 0, 5}},    {475, {1, 1, 3, 2, 0}},   {477, {1, 1, 0, 0, 0}},   {478, {1, 1, 0, 0, 0}},
        {540, {5, 2, 0, 0, 5}},    {541, {5, 2, 0, 0, 5}},   {542, {5, 2, 0, 0, 5}},   {654, {5, 2, 0, 0, 5}},
        {441, {2, 30, 0, 0, 0}},   {304, {3, 3, 1, 16, 3}},  {306, {13, 2, 0, 0, 0}},  {300, {2, 1, 0, 0, 0}},   {13004, {4, 2, 0, 0, 4}}, {13005, {4, 2, 0, 0, 4}}, {13006, {4, 2, 0, 0, 4}},
        // the utility windows' buttons, the Equalizer's popup and sliders (EQWindow.nib)
        {13014, {4, 2, 0, 0, 4}},  {13015, {4, 2, 0, 0, 4}}, {13016, {4, 2, 0, 0, 4}}, {786, {3, 1, 4, 12, 3}},
        {3210, {1, 1, 0, 0, 0, 10, 10}},   {3212, {3, 1, 0, 0, 3}},
        // the browser's splitter; the user ratings (PBUT 528, 529, 531: eleven rows, a half star each)
        {312, {1, 1, 0, 0, 0}},    {313, {1, 1, 0, 0, 0}},   {528, {1, 11, 0, 0, 0}},  {529, {1, 11, 0, 0, 0}},
        {531, {1, 11, 0, 0, 0}},
        // the grid (Placards.nib's BrowseBar; PBUT 570, 578: their fixed sides)
        {552, {2, 2, 3, 3, 2}},    {553, {2, 2, 3, 3, 2}},   {554, {2, 2, 1, 1, 2}},   {556, {2, 1, 4, 4, 1}},
        {558, {3, 1, 0, 0, 3}},    {560, {3, 1, 0, 0, 3}},   {562, {3, 1, 0, 0, 3}},   {564, {1, 1, 0, 0, 0}},
        {570, {1, 1, 22, 22, 0, 22, 22}}, {578, {1, 1, 12, 12, 0, 12, 12}},
        // the smart playlist rules' − and + (PBUT 138: disabled, normal, pressed; − then +)
        {138, {3, 2, 0, 0, 3}},
        // the Preferences' Parental lock (PBUT 142: 33-point cells, kind 1; row 0 locked)
        {142, {3, 2, 0, 0, 1}},
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

int stateColumn11(int kind, int state)
{
    static const int m[9][7] = {
        {1, 1, 1, 1, 0, 1, 0}, {0, 1, 0, 0, 0, 0, 0}, {1, 2, 1, 1, 0, 1, 0}, {1, 3, 2, 1, 0, 1, 0}, {1, 2, 1, 1, 0, 4, 3},
        {1, 3, 2, 1, 0, 5, 4}, {0, 0, 0, 0, 1, 1, 1}, {0, 2, 1, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0},
    };
    return kind >= 1 && kind <= 9 && state >= 0 && state < 7 ? m[kind - 1][state] : 0;
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
    const double t = std::min<double>(info.fixedTop, dst.h / 2), b = std::min<double>(info.fixedBottom, dst.h / 2);
    if (t > 0 || b > 0) {
        // a nine-slice: the corners kept, the edges stretched along, the middle both ways
        const double xs[4] = {src.x, src.x + l, src.right() - r, src.right()}, xd[4] = {dst.x, dst.x + l, dst.right() - r, dst.right()};
        const double ys[4] = {src.y, src.y + t, src.bottom() - b, src.bottom()}, yd[4] = {dst.y, dst.y + t, dst.bottom() - b, dst.bottom()};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (xs[i + 1] > xs[i] && ys[j + 1] > ys[j] && xd[i + 1] > xd[i] && yd[j + 1] > yd[j])
                    img.draw(cr, {xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j]}, {xd[i], yd[j], xd[i + 1] - xd[i], yd[j + 1] - yd[j]}, alpha);
        return;
    }
    if ((l == 0 && r == 0) || dst.w == cell.w) {
        img.draw(cr, src, dst, alpha);
        return;
    }
    img.draw(cr, {src.x, src.y, l, src.h}, {dst.x, dst.y, l, dst.h}, alpha);
    img.draw(cr, {src.x + l, src.y, src.w - l - r, src.h}, {dst.x + l, dst.y, dst.w - l - r, dst.h}, alpha);
    img.draw(cr, {src.right() - r, src.y, r, src.h}, {dst.right() - r, dst.y, r, dst.h}, alpha);
}

Size dialogSize(int dlog)
{
    const std::string *d = localized().get("DLOG", dlog);
    if (!d || d->size() < 8)
        return {};
    return {double(s16(*d, 6) - s16(*d, 2)), double(s16(*d, 4) - s16(*d, 0))};
}

std::vector<DialogItem> dialogItems(int ditl)
{
    std::vector<DialogItem> items;
    const std::string *dp = localized().get("DITL", ditl);
    if (!dp || dp->size() < 2)
        return items;
    const std::string &d = *dp;
    const int n = s16(d, 0) + 1;
    size_t p = 2;
    for (int i = 0; i < n && p + 14 <= d.size(); ++i) {
        DialogItem it;
        const int top = s16(d, p + 4), left = s16(d, p + 6), bottom = s16(d, p + 8), right = s16(d, p + 10);
        it.rect = {double(left), double(top), double(right - left), double(bottom - top)};
        const unsigned type = uint8_t(d[p + 12]);
        it.type = int(type & 0x7F);
        it.disabled = type & 0x80;
        const size_t len = uint8_t(d[p + 13]);
        const std::string data = d.substr(p + 14, std::min(len, d.size() - p - 14));
        if (it.type == DialogItem::Control && data.size() >= 2) {
            // the CNTL: rect, value, visible, max, min, procID, refCon, title
            if (const std::string *cn = localized().get("CNTL", s16(data, 0)); cn && cn->size() >= 23) {
                it.value = s16(*cn, 8);
                it.max = s16(*cn, 12);
                it.min = s16(*cn, 14);
                it.proc = s16(*cn, 16);
                it.title = macRoman(pstring(*cn, 22));
            }
        } else {
            it.text = macRoman(data);
        }
        items.push_back(it);
        p += 14 + len + (len & 1);
    }
    return items;
}

std::vector<std::string> classicMenu(int id)
{
    std::vector<std::string> out;
    const std::string *dp = localized().get("MENU", id);
    if (!dp || dp->size() < 15)
        return out;
    const std::string &d = *dp;
    size_t p = 14;
    p += 1 + uint8_t(d[p]);   // the title
    while (p < d.size() && d[p]) {
        const std::string t = macRoman(pstring(d, p));
        out.push_back(t == "-" ? "-" : t);
        p += 1 + uint8_t(d[p]) + 4;
    }
    return out;
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

namespace {

// 11.4's TextStyles.plist (its JSON beside it): {"302": {"font": "HelveticaNeue-Medium", "size": 11,
// "justification": "center"}, ...}, the numbers the Txtr resources had
TextStyle textStyle11(int id)
{
    static const std::string json = [] {
        std::ifstream in(Assets::path(res("English.lproj/TextStyles.plist")) + ".json");
        return std::string((std::istreambuf_iterator<char>(in)), {});
    }();
    TextStyle s;
    s.font = Font::system(11);
    const size_t at = json.find("\"" + std::to_string(id) + "\": {");
    if (at == std::string::npos)
        return s;
    const std::string entry = json.substr(at, json.find('}', at) - at);
    auto value = [&](const char *key) {
        const size_t k = entry.find(std::string("\"") + key + "\": ");
        if (k == std::string::npos)
            return std::string();
        size_t v = k + std::strlen(key) + 4;
        if (entry[v] == '"')
            return entry.substr(v + 1, entry.find('"', v + 1) - v - 1);
        return entry.substr(v, entry.find_first_of(",}", v) - v);
    };
    const std::string font = value("font"), size = value("size"), just = value("justification");
    if (!size.empty())
        s.font.size = std::atof(size.c_str());
    // PostScript names: LucidaGrande(-Bold), HelveticaNeue(-Medium, -Bold), Helvetica(-Bold)
    const size_t dash = font.find('-');
    const std::string base = font.substr(0, dash), face = dash == std::string::npos ? "" : font.substr(dash + 1);
    s.font.bold = face == "Bold";
    if (base == "HelveticaNeue")
        s.font.family = "Helvetica Neue";
    else if (base == "Helvetica")
        s.font.family = "Helvetica";
    s.align = just == "center" ? Align::Center : just == "right" ? Align::Right : Align::Left;
    return s;
}

} // namespace

TextStyle textStyle(int id)
{
    if (eleven())
        return textStyle11(id);
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

namespace {

// ---- 11.4's menus: tables in the program (built by the initializer at 0x934e2a): each menu its ID,
// 4-char tag and title's text number, its items {command, submenu ID, text number}, "DIV0" / "DIV1"
// a separator. The texts carry their keys: "Minimize###M", "Show in Finder###R#shift",
// "Convert…####option" (no key: the Option alternate of the item before), and Controls ▸ Play's
// hand-made column ("Play      Space").
struct Item11 {
    const char *command;
    int submenu;
    uint32_t text;
};
struct Menu11 {
    int id;
    uint32_t title;
    std::vector<Item11> items;
};

const std::vector<Menu11> &menus11()
{
    static const std::vector<Menu11> menus = {
        // {menu ID, its title text's number, {command, submenu, text number}...}: iTunes 11.4's tables
        {129, 0x75300001, {{"abou", 0, 0x75300002}, {"DIV0", 0, 0x0}, {"pref", 0, 0x75300003}, {"DIV0", 0, 0x0}, {"Feed", 0, 0x75300004}, {"ckup", 0, 0x75300005}, {"DIV0", 0, 0x0}, {"serv", 0, 0x75300006}, {"DIV0", 0, 0x0}, {"hitn", 0, 0x75300007}, {"hoth", 0, 0x75300008}, {"shal", 0, 0x75300009}, {"DIV0", 0, 0x0}, {"quit", 0, 0x7530000a}}},
        {131, 0x75f80001, {{"ctxe", 225, 0x75f80002}, {"epls", 0, 0x75f80016}, {"CLOS", 0, 0x75f8000f}, {"DIV0", 0, 0x0}, {"add ", 0, 0x75f8000c}, {"ctxe", 210, 0x75f8000d}, {"ctxe", 212, 0x75f80021}, {"ctxe", 228, 0x75f8000e}, {"DIV0", 0, 0x0}, {"ourl", 0, 0x77880002}, {"pcsb", 0, 0x77880003}, {"DIV0", 0, 0x0}, {"geti", 0, 0x75f80013}, {"SRAT", 200, 0x75f80014}, {"revl", 0, 0x75f80019}, {"ctxe", 226, 0x75f80003}, {"DIV0", 0, 0x0}, {"pset", 0, 0x75f8001e}, {"prnt", 0, 0x75f8001f}}},
        {133, 0x765c0001, {{"undo", 0, 0x765c0002}, {"redo", 0, 0x765c0003}, {"DIV0", 0, 0x0}, {"cut ", 0, 0x765c0004}, {"copy", 0, 0x765c0005}, {"past", 0, 0x765c0006}, {"clea", 0, 0x765c0007}, {"sall", 0, 0x765c0008}, {"SNon", 0, 0x765c0009}}},
        {135, 0x77240001, {{"VOpt", 0, 0x7724001e}, {"COpt", 217, 0x77240003}, {"PQsh", 0, 0x77240064}, {"ShBF", 0, 0x77240062}, {"shsl", 0, 0x7724005f}, {"DIV0", 0, 0x0}, {"SDup", 0, 0x7724001b}, {"SDup", 0, 0x7724001c}, {"shct", 0, 0x7724000a}, {"DIV0", 0, 0x0}, {"vdsz", 211, 0x7724006c}, {"vtog", 0, 0x77240013}, {"viss", 204, 0x77240015}, {"DIV0", 0, 0x0}, {"eeFS", 0, 0x77240017}}},
        {134, 0x76c00001, {{"plps", 0, 0x76c00002}, {"gnsh", 0, 0x76c00003}, {"stop", 0, 0x76c00004}, {"ntrk", 0, 0x76c00005}, {"back", 0, 0x76c00006}, {"plyr", 218, 0x76c00007}, {"SCur", 0, 0x75f8001a}, {"DIV0", 0, 0x0}, {"nchp", 0, 0x76c00008}, {"pchp", 0, 0x76c00009}, {"DIV0", 0, 0x0}, {"audi", 209, 0x76c0000d}, {"volu", 0, 0x76c00012}, {"vold", 0, 0x76c00013}, {"DIV0", 0, 0x0}, {"shfm", 214, 0x76c0000a}, {"rptm", 215, 0x76c0000e}, {"DIV0", 0, 0x0}, {"ejct", 0, 0x76c00015}}},
        {141, 0x79180001, {{"Msgb", 0, 0x79180004}, {"Msgf", 0, 0x79180005}, {"Msgh", 0, 0x79180003}, {"Msrp", 0, 0x79180014}, {"DIV1", 0, 0x0}, {"TMtc", 0, 0x79180006}, {"UMtc", 0, 0x7918000d}, {"TGen", 0, 0x7918000a}, {"UGen", 0, 0x7918000c}, {"DIV0", 0, 0x0}, {"autA", 0, 0x79180010}, {"dauS", 0, 0x79180011}, {"daud", 0, 0x7788000d}, {"DIV0", 0, 0x0}, {"mssi", 0, 0x79180012}, {"Msvc", 0, 0x79180013}, {"SCDN", 0, 0x7918000f}}},
        {138, 0x77ec0001, {{"minw", 0, 0x77ec0002}, {"zoom", 0, 0x77ec0003}, {"DIV0", 0, 0x0}, {"brwn", 0, 0x77ec0004}, {"eqwn", 0, 0x77ec0005}, {"minp", 0, 0x77ec0006}, {"Dwld", 0, 0x77ec0007}, {"DIV0", 0, 0x0}, {"smin", 0, 0x77ec0008}, {"DIV0", 0, 0x0}, {"batf", 0, 0x77ec0009}}},
        {140, 0x78b40001, {{"ahlp", 0, 0x78b40002}, {"DIV0", 0, 0x0}, {"Wlcm", 0, 0x78b40009}, {"KeyB", 0, 0x78b40004}, {"DIV0", 0, 0x0}, {"podH", 0, 0x78b40005}, {"iPhH", 0, 0x78b40007}, {"iPdH", 0, 0x78b40008}, {"atvH", 0, 0x78b40006}, {"DIV0", 0, 0x0}, {"Supp", 0, 0x78b40003}}},
        {160, 0x797c0001, {{"TFnd", 0, 0x797c0002}, {"pref", 0, 0x797c0003}, {"mute", 0, 0x76c00014}, {"Sbti", 0, 0x797c0006}, {"getS", 0, 0x797c0009}, {"pitm", 0, 0x797c0008}, {"nitm", 0, 0x797c0007}, {"NbTB", 0, 0x797c000a}, {"RMus", 0, 0x797c000b}, {"RMov", 0, 0x797c000c}, {"RTVS", 0, 0x797c000d}, {"RPod", 0, 0x797c000e}, {"RITU", 0, 0x797c000f}, {"RBok", 0, 0x797c0010}, {"RApp", 0, 0x797c0011}, {"vtog", 0, 0x797c0004}, {"Brfs", 0, 0x797c0005}}},
        {139, 0x0, {{"", 0, 0x78500001}, {"sgnc", 0, 0x78500002}, {"MRAT", 201, 0x78500003}, {"DIV0", 0, 0x0}, {"fist", 0, 0x78500004}, {"DIV0", 0, 0x0}, {"rptm", 219, 0x78500005}, {"SHUF", 0, 0x78500006}, {"DIV0", 0, 0x0}, {"PLPS", 0, 0x78500007}, {"ntrk", 0, 0x78500008}, {"ptrk", 0, 0x78500009}, {"plyr", 220, 0x7850000a}}},
        {210, 0x0, {{"gath", 0, 0x77880008}, {"expl", 0, 0x75f80012}, {"DIV0", 0, 0x0}, {"impt", 0, 0x75f80010}, {"expr", 0, 0x75f80011}, {"DIV0", 0, 0x0}, {"gart", 0, 0x77880009}}},
        {212, 0x0, {{"sync", 0, 0x7e2c000d}, {"rsyn", 0, 0x7e2c0010}, {"DIV0", 0, 0x0}, {"bkud", 0, 0x7e2c0014}, {"resd", 0, 0x7e2c0015}}},
        {226, 0x0, {{"imph", 0, 0x77880006}, {"imat", 0, 0x77880007}, {"imps", 0, 0x77880004}, {"imps", 0, 0x77880005}}},
        {228, 0x0, {{"THSh", 0, 0x7788000e}, {"ATpp", 0, 0x77880011}}},
        {225, 0x0, {{"nply", 0, 0x75f80004}, {"npls", 0, 0x75f80005}, {"nspl", 0, 0x75f80006}, {"nfld", 0, 0x75f80007}}},
        {200, 0x0, {{"SRT0", 0, 0x79e00001}, {"SRT1", 0, 0x79e00002}, {"SRT2", 0, 0x79e00003}, {"SRT3", 0, 0x79e00004}, {"SRT4", 0, 0x79e00005}, {"SRT5", 0, 0x79e00006}}},
        {217, 0x0, {{"SBro", 0, 0x77240007}}},
        {213, 0x0, {{"dins", 0, 0x7b0c0003}, {"DIV0", 0, 0x0}, {"sins", 0, 0x7b0c0006}, {"hins", 0, 0x7b0c0004}, {"Hins", 0, 0x7b0c0005}}},
        {214, 0x0, {{"shuf", 0, 0x76c0000b}, {"DIV0", 0, 0x0}, {"shfs", 0, 0x76c00016}, {"shfa", 0, 0x76c00017}, {"shfg", 0, 0x76c00018}}},
        {215, 0x0, {{"rpt0", 0, 0x76c0000f}, {"rpt2", 0, 0x76c00010}, {"rpt1", 0, 0x76c00011}}},
        {209, 0x0, {}},
        {204, 0x0, {{"DIV0", 0, 0x0}, {"Vpvt", 0, 0x7aa80002}, {"VisO", 0, 0x7aa80001}}},
        {227, 0x0, {{"gnam", 0, 0x7788000a}, {"scdi", 0, 0x7788000b}, {"jsel", 0, 0x7788000c}}},
    };
    return menus;
}

const Menu11 *menu11(int id)
{
    for (const Menu11 &m : menus11())
        if (m.id == id)
            return &m;
    return nullptr;
}

std::vector<MainMenuItem> parseMenu11(int id, int depth)
{
    std::vector<MainMenuItem> items;
    const Menu11 *m = menu11(id);
    if (!m || depth > 4)
        return items;
    for (const Item11 &i : m->items) {
        MainMenuItem it;
        const std::string cmd = i.command;
        if (cmd == "DIV0" || cmd == "DIV1") {
            it.separator = true;
            items.push_back(it);
            continue;
        }
        std::string t = text(i.text);
        const size_t gap = t.find("  ");
        if (gap != std::string::npos)
            t = t.substr(0, gap);
        const size_t keys = t.find("###");
        if (keys != std::string::npos) {
            std::string rest = t.substr(keys + 3);
            t = t.substr(0, keys);
            // the key (none when "#" follows at once), then "#shift", "#option", "#control"
            if (!rest.empty() && rest[0] != '#') {
                it.key = std::string(1, char(std::tolower((unsigned char)rest[0])));
                rest = rest.substr(1);
            }
            it.modifiers = it.key.empty() ? 0 : unsigned(Command);
            if (rest.find("#shift") != std::string::npos)
                it.modifiers |= Shift;
            if (rest.find("#option") != std::string::npos)
                it.modifiers |= Option;
            if (rest.find("#control") != std::string::npos)
                it.modifiers |= Control;
            if (it.key.empty() && (it.modifiers & Option) && !items.empty() && !items.back().separator)
                it.alternate = true;
        }
        it.title = t;
        it.action = cmd;
        if (i.submenu) {
            it.hasSubmenu = true;
            it.items = parseMenu11(i.submenu, depth + 1);
            it.action.clear();
        }
        if (cmd.empty())
            it.action.clear();   // the Dock menu's Now Playing: a title
        items.push_back(it);
    }
    return items;
}

std::vector<MainMenuItem> menuBar11()
{
    // the menu bar's order: iTunes, File, Edit, View, Controls, Store, Window, Help
    std::vector<MainMenuItem> bar;
    for (int id : {129, 131, 133, 135, 134, 141, 138, 140}) {
        const Menu11 *m = menu11(id);
        if (!m)
            continue;
        MainMenuItem menu;
        menu.hasSubmenu = true;
        menu.title = id == 129 ? std::string("iTunes") : text(m->title);
        menu.items = parseMenu11(id, 0);
        for (MainMenuItem &it : menu.items) {
            // the application menu's own: Services, Hide, Quit as AppKit names them
            if (it.action == "serv") {
                it.hasSubmenu = true;
                it.action.clear();
            } else if (it.action == "hitn")
                it.action = "hide:";
            else if (it.action == "hoth")
                it.action = "hideOtherApplications:";
            else if (it.action == "shal")
                it.action = "unhideAllApplications:";
            else if (it.action == "quit")
                it.action = "terminate:";
        }
        bar.push_back(menu);
    }
    MainMenuItem hidden;   // "Commands" (160): its keys work, it isn't shown
    hidden.title = text(0x797c0001);
    hidden.hasSubmenu = true;
    hidden.hidden = true;
    hidden.items = parseMenu11(160, 0);
    bar.push_back(hidden);
    return bar;
}

} // namespace

std::vector<MainMenuItem> menuItems(int id) { return eleven() ? parseMenu11(id, 0) : parseMenu(id, 0); }

std::vector<MainMenuItem> menuBar()
{
    if (eleven())
        return menuBar11();
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
