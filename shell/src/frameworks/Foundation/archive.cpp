// SPDX-License-Identifier: LGPL-2.1-or-later
#include "archive.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace AK {

namespace {

unsigned long long be(const std::string &b, size_t at, int n)
{
    unsigned long long v = 0;
    for (int i = 0; i < n && at + size_t(i) < b.size(); ++i)
        v = (v << 8) | static_cast<unsigned char>(b[at + size_t(i)]);
    return v;
}

void utf8(std::string &out, unsigned cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 0x3F));
    else if (cp < 0x10000) out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
    else out += char(0xF0 | cp >> 18), out += char(0x80 | (cp >> 12 & 0x3F)), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
}

} // namespace

bool Archive::load(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    m_bytes.assign(std::istreambuf_iterator<char>(in), {});
    m_values.clear();
    m_objects = m_root = -1;
    if (m_bytes.size() < 40 || m_bytes.compare(0, 8, "bplist00") != 0)
        return false;
    const size_t t = m_bytes.size() - 32;
    const int offsetSize = static_cast<unsigned char>(m_bytes[t + 6]);
    m_refSize = static_cast<unsigned char>(m_bytes[t + 7]);
    const size_t count = size_t(be(m_bytes, t + 8, 8)), top = size_t(be(m_bytes, t + 16, 8)), table = size_t(be(m_bytes, t + 24, 8));
    if (!count || count > 10000000 || table + count * size_t(offsetSize) > m_bytes.size())
        return false;
    m_offsets.resize(count);
    for (size_t i = 0; i < count; ++i)
        m_offsets[i] = size_t(be(m_bytes, table + i * size_t(offsetSize), offsetSize));
    m_values.assign(count, Value());
    for (size_t i = 0; i < count; ++i)
        parse(int(i));
    m_root = int(top);
    m_objects = field(m_root, "$objects");
    if (m_objects < 0 || m_values[size_t(m_objects)].type != Value::Array)
        m_objects = -1;
    return m_objects >= 0;
}

int Archive::parse(int index)
{
    Value &v = m_values[size_t(index)];
    size_t at = m_offsets[size_t(index)];
    if (at >= m_bytes.size())
        return index;
    const unsigned char marker = static_cast<unsigned char>(m_bytes[at]);
    const int kind = marker >> 4, low = marker & 0xF;
    auto length = [&]() -> size_t {
        // the count, or (0xF) an int object after the marker
        if (low != 0xF) {
            at += 1;
            return size_t(low);
        }
        const unsigned char m = static_cast<unsigned char>(m_bytes[at + 1]);
        const int n = 1 << (m & 0xF);
        const size_t c = size_t(be(m_bytes, at + 2, n));
        at += 2 + size_t(n);
        return c;
    };
    switch (kind) {
    case 0x0:
        v.type = low == 8 || low == 9 ? Value::Bool : Value::Null;
        v.i = low == 9;
        break;
    case 0x1: {
        const int n = 1 << low;
        v.type = Value::Int;
        v.i = (long long)be(m_bytes, at + 1, n);
        if (n == 1) v.i = (long long)(unsigned char)v.i;
        break;
    }
    case 0x2: {
        v.type = Value::Real;
        const unsigned long long bits = be(m_bytes, at + 1, 1 << low);
        if (low == 2) {
            float f;
            const uint32_t b = uint32_t(bits);
            std::memcpy(&f, &b, 4);
            v.r = f;
        } else {
            std::memcpy(&v.r, &bits, 8);
        }
        v.i = (long long)v.r;
        break;
    }
    case 0x3: {
        v.type = Value::Real;   // a date: seconds since 2001
        const unsigned long long bits = be(m_bytes, at + 1, 8);
        std::memcpy(&v.r, &bits, 8);
        break;
    }
    case 0x4: {
        const size_t n = length();
        v.type = Value::Data;
        v.s = m_bytes.substr(at, n);
        break;
    }
    case 0x5: {
        const size_t n = length();
        v.type = Value::String;
        // ASCII (bytes above 0x7F as Latin-1)
        for (size_t i = 0; i < n && at + i < m_bytes.size(); ++i)
            utf8(v.s, static_cast<unsigned char>(m_bytes[at + i]));
        break;
    }
    case 0x6: {
        const size_t n = length();
        v.type = Value::String;
        for (size_t i = 0; i < n; ++i) {
            unsigned cp = unsigned(be(m_bytes, at + 2 * i, 2));
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) {
                const unsigned lo = unsigned(be(m_bytes, at + 2 * (i + 1), 2));
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
            utf8(v.s, cp);
        }
        break;
    }
    case 0x8:
        v.type = Value::Uid;
        v.i = (long long)be(m_bytes, at + 1, low + 1);
        break;
    case 0xA:
    case 0xC: {
        const size_t n = length();
        v.type = Value::Array;
        for (size_t i = 0; i < n; ++i)
            v.items.push_back(int(be(m_bytes, at + i * size_t(m_refSize), m_refSize)));
        break;
    }
    case 0xD: {
        const size_t n = length();
        v.type = Value::Dict;
        for (size_t i = 0; i < n; ++i)
            v.keys.push_back(int(be(m_bytes, at + i * size_t(m_refSize), m_refSize)));
        for (size_t i = 0; i < n; ++i)
            v.items.push_back(int(be(m_bytes, at + (n + i) * size_t(m_refSize), m_refSize)));
        break;
    }
    default:
        break;
    }
    return index;
}

int Archive::field(int value, const char *key) const
{
    if (value < 0 || size_t(value) >= m_values.size())
        return -1;
    const Value &v = m_values[size_t(value)];
    if (v.type != Value::Dict)
        return -1;
    for (size_t i = 0; i < v.keys.size(); ++i) {
        const Value &k = m_values[size_t(v.keys[i])];
        if (k.type == Value::String && k.s == key)
            return v.items[i];
    }
    return -1;
}

int Archive::follow(int value) const
{
    // a UID: the archived object at that place in $objects
    for (int guard = 0; value >= 0 && guard < 8; ++guard) {
        const Value &v = m_values[size_t(value)];
        if (v.type != Value::Uid)
            return value;
        const Value &objects = m_values[size_t(m_objects)];
        if (v.i < 0 || size_t(v.i) >= objects.items.size())
            return -1;
        value = objects.items[size_t(v.i)];
    }
    return value;
}

int Archive::top(const char *key) const { return follow(field(field(m_root, "$top"), key)); }

std::string Archive::className(int obj) const
{
    const int cls = follow(field(obj, "$class"));
    const int name = field(cls, "$classname");
    return name >= 0 ? m_values[size_t(name)].s : std::string();
}

bool Archive::isA(int obj, const char *cls) const
{
    const int c = follow(field(obj, "$class"));
    const int classes = field(c, "$classes");
    if (classes < 0)
        return false;
    for (int i : m_values[size_t(classes)].items)
        if (m_values[size_t(i)].s == cls)
            return true;
    return false;
}

int Archive::object(int obj, const char *key) const
{
    const int v = follow(field(obj, key));
    // $null is "no object"
    if (v >= 0 && m_values[size_t(v)].type == Value::String && m_values[size_t(v)].s == "$null")
        return -1;
    return v;
}

bool Archive::has(int obj, const char *key) const { return field(obj, key) >= 0; }

long long Archive::integer(int obj, const char *key, long long fallback) const
{
    const int v = object(obj, key);
    if (v < 0)
        return fallback;
    const Value &x = m_values[size_t(v)];
    return x.type == Value::Int || x.type == Value::Bool ? x.i : x.type == Value::Real ? (long long)x.r : fallback;
}

double Archive::real(int obj, const char *key, double fallback) const
{
    const int v = object(obj, key);
    if (v < 0)
        return fallback;
    const Value &x = m_values[size_t(v)];
    return x.type == Value::Real ? x.r : x.type == Value::Int || x.type == Value::Bool ? double(x.i) : fallback;
}

bool Archive::boolean(int obj, const char *key, bool fallback) const { return integer(obj, key, fallback) != 0; }

std::string Archive::stringOf(int v) const
{
    if (v < 0)
        return {};
    const Value &x = m_values[size_t(v)];
    if (x.type == Value::String || x.type == Value::Data)
        return x.s;
    if (x.type == Value::Dict) {
        // NSString, NSMutableString: NS.string; NS.bytes (UTF-8)
        for (const char *k : {"NS.string", "NS.bytes"}) {
            const int s = object(v, k);
            if (s >= 0)
                return stringOf(s);
        }
    }
    return {};
}

std::string Archive::string(int obj, const char *key) const { return stringOf(object(obj, key)); }

std::vector<int> Archive::arrayOf(int v) const
{
    std::vector<int> out;
    if (v < 0)
        return out;
    const Value &x = m_values[size_t(v)];
    int list = -1;
    if (x.type == Value::Array)
        list = v;
    else
        list = field(v, "NS.objects");
    if (list < 0)
        return out;
    for (int i : m_values[size_t(list)].items) {
        const int o = follow(i);
        if (o >= 0)
            out.push_back(o);
    }
    return out;
}

std::vector<int> Archive::array(int obj, const char *key) const { return arrayOf(object(obj, key)); }

std::string Archive::data(int obj, const char *key) const
{
    const int v = object(obj, key);
    return v >= 0 && m_values[size_t(v)].type == Value::Data ? m_values[size_t(v)].s : std::string();
}

} // namespace AK
