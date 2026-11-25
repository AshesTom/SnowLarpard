// SPDX-License-Identifier: LGPL-2.1-or-later
#include "smartblobs.h"

#include "smartplaylist.h"

#include <algorithm>
#include <cstdint>

namespace iTunes {

namespace {

// Mac time (seconds from 1904) and the Unix days the port's date rules hold
constexpr long long MacEpoch = 2082844800;
// "in the last": the record's first and fourth values (iTunes' marker)
constexpr long long InTheLast = 0x2dae2dae2dae2daeLL;

void put32(std::string &b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b[at + size_t(i)] = char((v >> (24 - 8 * i)) & 0xff);
}
void put64(std::string &b, size_t at, long long v)
{
    put32(b, at, uint32_t(uint64_t(v) >> 32));
    put32(b, at + 4, uint32_t(uint64_t(v)));
}
uint32_t get32(const std::string &b, size_t at)
{
    if (at + 4 > b.size())
        return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | static_cast<unsigned char>(b[at + size_t(i)]);
    return v;
}
long long get64(const std::string &b, size_t at) { return (long long)((uint64_t(get32(b, at)) << 32) | get32(b, at + 4)); }

// The port's MENU 304 (minutes, hours, MB, GB, items) and Smart Info's codes (FUN_00192dfa: its
// internal 0–4 written 1–5: minutes, MB, items, hours, GB)
const int UnitCodes[5] = {1, 4, 2, 5, 3};
// MENU 305 without its lines (random, album, artist, genre, name, highest rating, lowest rating, most /
// least recently played, most / least often played, most / least recently added): the order's code and
// byte 13 (reversed)
const struct { int code, reversed; } Orders[13] = {
    {0x02, 0}, {0x06, 0}, {0x07, 0}, {0x09, 0}, {0x05, 0}, {0x1c, 0}, {0x01, 0},
    {0x1a, 0}, {0x1a, 1}, {0x19, 0}, {0x19, 1}, {0x15, 0}, {0x15, 1},
};

std::string utf16be(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3f);
        i += size_t(n);
        auto unit = [&](uint32_t u) {
            o += char(u >> 8);
            o += char(u & 0xff);
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 | (cp >> 10));
            unit(0xDC00 | (cp & 0x3ff));
        } else
            unit(cp);
    }
    return o;
}

std::string fromUtf16be(const std::string &b)
{
    std::string o;
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        uint32_t cp = (uint32_t(static_cast<unsigned char>(b[i])) << 8) | static_cast<unsigned char>(b[i + 1]);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < b.size()) {
            const uint32_t lo = (uint32_t(static_cast<unsigned char>(b[i + 2])) << 8) | static_cast<unsigned char>(b[i + 3]);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (cp < 0x80)
            o += char(cp);
        else if (cp < 0x800) {
            o += char(0xC0 | (cp >> 6));
            o += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += char(0xE0 | (cp >> 12));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        } else {
            o += char(0xF0 | (cp >> 18));
            o += char(0x80 | ((cp >> 12) & 0x3F));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

SmartField::Kind kindOf(int field)
{
    if (const SmartField *f = smartField(field))
        return f->kind;
    return SmartField::Number;
}

// The port's values (MB, seconds, Unix days) and iTunes' (bytes, milliseconds, Mac seconds)
long long toITunes(SmartField::Kind k, long long v)
{
    switch (k) {
    case SmartField::Size: return v * 1048576;
    case SmartField::Time: return v * 1000;
    case SmartField::Date: return v * 86400 + MacEpoch;
    default: return v;
    }
}
long long fromITunes(SmartField::Kind k, long long v)
{
    switch (k) {
    case SmartField::Size: return v / 1048576;
    case SmartField::Time: return v / 1000;
    case SmartField::Date: return v >= MacEpoch ? (v - MacEpoch) / 86400 : 0;
    default: return v;
    }
}

// The 0x44-byte record: from, 0, 1, to, 0, 1 (a range's ends; the same value twice otherwise); "in
// the last": the marker, minus the count, the unit in seconds, twice
std::string numeric(const SmartRule &r)
{
    std::string b(0x44, '\0');
    const SmartField::Kind k = kindOf(r.field);
    if ((r.op & 0xffff) == 0x200) {
        for (size_t at : {size_t(0), size_t(24)}) {
            put64(b, at, InTheLast);
            put64(b, at + 8, -r.from);
            put64(b, at + 16, (long long)r.unit * 86400);
        }
        return b;
    }
    const long long from = toITunes(k, r.from);
    const long long to = (r.op & 0xffff) == 0x100 ? toITunes(k, r.to) : from;
    put64(b, 0, from);
    put64(b, 16, 1);
    put64(b, 24, to);
    put64(b, 40, 1);
    return b;
}

void readNumeric(const std::string &d, SmartRule &r)
{
    const SmartField::Kind k = kindOf(r.field);
    if (get64(d, 0) == InTheLast) {
        r.from = -get64(d, 8);
        r.unit = std::max(1, int(get64(d, 16) / 86400));
        return;
    }
    r.from = fromITunes(k, get64(d, 0));
    r.to = fromITunes(k, get64(d, 24));
}

// one SLst (FUN_0037c8ba); false when it is not one
bool readList(const std::string &b, SmartInfo &info, int depth)
{
    if (b.size() < 0x88 || b.compare(0, 4, "SLst") != 0 || depth > 4)
        return false;
    const uint32_t count = get32(b, 8);
    if (depth == 0)
        info.any = b[15] != 0;
    size_t at = 0x88;
    for (uint32_t i = 0; i < count && at + 0x38 <= b.size(); ++i) {
        SmartRule r;
        r.field = int(get32(b, at));
        r.op = get32(b, at + 4);
        const bool sub = b[at + 8] != 0;
        const uint32_t len = get32(b, at + 0x34);
        if (at + 0x38 + len > b.size())
            return false;
        const std::string data = b.substr(at + 0x38, len);
        if (sub) {
            SmartInfo nested;   // a nested list: its rules not offered by the port's window
            readList(data, nested, depth + 1);
        } else if (depth == 0) {
            if (len == 0x44 && !(r.op & 0x01000000))
                readNumeric(data, r);
            else
                r.text = fromUtf16be(data);
            info.rules.push_back(r);
        }
        at += 0x38 + len + (len & 1);
    }
    return true;
}

} // namespace

std::string smartInfoBlob(const SmartInfo &info)
{
    std::string b(0x70, '\0');
    b[0] = info.live ? 1 : 0;
    b[1] = info.useRules ? 1 : 0;
    b[2] = info.limit ? 1 : 0;
    b[3] = char(UnitCodes[std::clamp(info.limitUnit, 0, 4)]);
    const auto &o = Orders[std::clamp(info.limitOrder, 0, 12)];
    put32(b, 4, uint32_t(o.code));
    put32(b, 8, uint32_t(info.limitValue));
    b[12] = info.checkedOnly ? 1 : 0;
    b[13] = char(o.reversed);
    b[14] = char(info.infoFlag);
    return b;
}

bool readSmartInfo(const std::string &b, SmartInfo &info)
{
    if (b.size() < 15)
        return false;
    info.live = b[0] != 0;
    info.useRules = b[1] != 0;
    info.limit = b[2] != 0;
    info.limitUnit = 4;
    for (int i = 0; i < 5; ++i)
        if (UnitCodes[i] == b[3])
            info.limitUnit = i;
    const int code = int(get32(b, 4)), reversed = b[13] != 0;
    info.limitOrder = 0;
    for (int i = 0; i < 13; ++i)
        if (Orders[i].code == code && Orders[i].reversed == reversed)
            info.limitOrder = i;
    info.limitValue = int(get32(b, 8));
    info.checkedOnly = b[12] != 0;
    info.infoFlag = static_cast<unsigned char>(b[14]);
    return true;
}

std::string smartCriteriaBlob(const SmartInfo &info)
{
    // the list read, unchanged: its own bytes
    if (!info.criteria.empty()) {
        SmartInfo was;
        if (readList(info.criteria, was, 0)) {
            SmartInfo now = info;
            now.criteria.clear();
            was.criteria.clear();
            was.live = now.live;
            was.useRules = now.useRules;
            if (was.any == now.any && was.rules.size() == now.rules.size() &&
                std::equal(was.rules.begin(), was.rules.end(), now.rules.begin(), [](const SmartRule &a, const SmartRule &b) {
                    return a.field == b.field && a.op == b.op && a.text == b.text && a.from == b.from && a.to == b.to && a.unit == b.unit;
                }))
                return info.criteria;
        }
    }
    std::string b(0x88, '\0');
    b.replace(0, 4, "SLst");
    b[5] = 1;   // 1, 1 (FUN_0037c336)
    b[7] = 1;
    put32(b, 8, uint32_t(info.rules.size()));
    b[15] = info.any ? 1 : 0;
    for (const SmartRule &r : info.rules) {
        const bool text = kindOf(r.field) == SmartField::Text || (r.op & 0x01000000);
        const std::string data = text ? utf16be(r.text) : numeric(r);
        std::string h(0x38, '\0');
        put32(h, 0, uint32_t(r.field));
        put32(h, 4, r.op);
        put32(h, 0x34, uint32_t(data.size()));
        b += h + data;
        if (data.size() & 1)
            b += '\0';
    }
    return b;
}

bool readSmartCriteria(const std::string &blob, SmartInfo &info)
{
    info.rules.clear();
    if (!readList(blob, info, 0))
        return false;
    info.criteria = blob;
    return true;
}

std::string base64Encode(const std::string &bytes)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t v = uint32_t(static_cast<unsigned char>(bytes[i])) << 16;
        if (i + 1 < bytes.size())
            v |= uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8;
        if (i + 2 < bytes.size())
            v |= static_cast<unsigned char>(bytes[i + 2]);
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += i + 1 < bytes.size() ? t[(v >> 6) & 63] : '=';
        o += i + 2 < bytes.size() ? t[v & 63] : '=';
    }
    return o;
}

std::string base64Decode(const std::string &text)
{
    std::string o;
    uint32_t v = 0;
    int bits = 0;
    for (char c : text) {
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (d < 0)
            continue;
        v = (v << 6) | uint32_t(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o += char((v >> bits) & 0xff);
        }
    }
    return o;
}

} // namespace iTunes
