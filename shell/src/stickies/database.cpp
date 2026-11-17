// SPDX-License-Identifier: LGPL-2.1-or-later
#include "database.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <sys/time.h>

namespace Stickies {

namespace {

constexpr int Int2 = -127, Int4 = -126, RealTag = -125, New = -124, Nil = -123, End = -122, FirstRef = -110;

// ---- reading: every group of the stream, by its type string, whatever the classes

struct TValue {
    enum Kind { None, Int, Real, Bytes, Object, Class, List } kind = None;
    long long i = 0;
    double r = 0;
    std::string s;
    int ref = -1;   // Object: the object's index
    std::vector<TValue> list;
};

struct TGroup {
    std::string type;
    std::vector<TValue> values;
};

struct TObject {
    std::string cls;
    int version = 0;
    std::vector<TGroup> groups;
};

class Reader {
public:
    explicit Reader(const std::string &b) : m_b(b) {}

    bool header()
    {
        if (m_b.size() < 16 || uint8_t(m_b[0]) != 4 || uint8_t(m_b[1]) != 11)
            return false;
        if (m_b.compare(2, 11, "streamtyped") == 0)
            m_little = true;
        else if (m_b.compare(2, 11, "typedstream") == 0)
            m_little = false;
        else
            return false;
        m_p = 13;
        integer(byte());   // the system's version
        return ok();
    }
    bool ok() const { return !m_bad; }
    bool atEnd() const { return m_p >= m_b.size(); }

    // a group at the top level: its type string and values
    bool group(TGroup &g)
    {
        int head = byte();
        if (m_bad)
            return false;
        if (head == End) {
            m_end = true;
            return false;
        }
        g.type = shared(head);
        g.values.clear();
        size_t at = 0;
        while (at < g.type.size() && !m_bad)
            g.values.push_back(value(g.type, at));
        return !m_bad;
    }
    std::vector<TObject> objects;
    std::vector<int> table;   // the shared table's entries: an object's index, or -2 - class index
    std::vector<std::pair<std::string, int>> classes;

private:
    int byte()
    {
        if (m_p >= m_b.size()) {
            m_bad = true;
            return End;
        }
        return int8_t(m_b[m_p++]);
    }
    uint64_t raw(int n)
    {
        if (m_p + size_t(n) > m_b.size()) {
            m_bad = true;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            const uint64_t c = uint8_t(m_b[m_p + size_t(i)]);
            v |= m_little ? c << (8 * i) : c << (8 * (n - 1 - i));
        }
        m_p += size_t(n);
        return v;
    }
    long long integer(int head)
    {
        if (head == Int2)
            return int16_t(raw(2));
        if (head == Int4)
            return int32_t(raw(4));
        return head;
    }
    double real(int head, bool isDouble)
    {
        if (head != RealTag)
            return double(integer(head));
        if (isDouble) {
            const uint64_t v = raw(8);
            double d;
            std::memcpy(&d, &v, 8);
            return d;
        }
        const uint32_t v = uint32_t(raw(4));
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::string shared(int head)
    {
        if (head == Nil)
            return {};
        if (head == New) {
            const long long n = integer(byte());
            if (n < 0 || m_p + size_t(n) > m_b.size()) {
                m_bad = true;
                return {};
            }
            std::string s = m_b.substr(m_p, size_t(n));
            m_p += size_t(n);
            m_strings.push_back(s);
            return s;
        }
        const long long i = integer(head) - FirstRef;
        if (i < 0 || size_t(i) >= m_strings.size()) {
            m_bad = true;
            return {};
        }
        return m_strings[size_t(i)];
    }
    // a class: its index in `classes`, -1 for nil
    int readClass(int head)
    {
        if (head == Nil)
            return -1;
        if (head == New) {
            const std::string name = shared(byte());
            const int version = int(integer(byte()));
            const int index = int(classes.size());
            classes.push_back({name, version});
            table.push_back(-2 - index);
            readClass(byte());   // its superclass, down to nil
            return index;
        }
        const long long i = integer(head) - FirstRef;
        if (i < 0 || size_t(i) >= table.size() || table[size_t(i)] > -2) {
            m_bad = true;
            return -1;
        }
        return -2 - table[size_t(i)];
    }
    TValue object(int head)
    {
        TValue v;
        if (head == Nil)
            return v;
        if (head != New) {
            const long long i = integer(head) - FirstRef;
            if (i < 0 || size_t(i) >= table.size() || table[size_t(i)] < 0) {
                m_bad = true;
                return v;
            }
            v.kind = TValue::Object;
            v.ref = table[size_t(i)];
            return v;
        }
        const int index = int(objects.size());
        objects.emplace_back();
        table.push_back(index);
        const int cls = readClass(byte());
        if (cls >= 0) {
            objects[size_t(index)].cls = classes[size_t(cls)].first;
            objects[size_t(index)].version = classes[size_t(cls)].second;
        }
        if (++m_depth > 64) {
            m_bad = true;
            return v;
        }
        TGroup g;
        m_end = false;
        while (!m_bad && group(g))
            objects[size_t(index)].groups.push_back(g);
        m_end = false;
        --m_depth;
        v.kind = TValue::Object;
        v.ref = index;
        return v;
    }
    // one value of the type at `at` (which moves past it)
    TValue value(const std::string &type, size_t &at)
    {
        TValue v;
        const char t = type[at++];
        switch (t) {
        case '@':
            return object(byte());
        case '#':
            v.kind = TValue::Class;
            v.i = readClass(byte());
            return v;
        case ':':
        case '*':
        case '%':
            v.kind = TValue::Bytes;
            v.s = shared(byte());
            return v;
        case 'f':
        case 'd':
            v.kind = TValue::Real;
            v.r = real(byte(), t == 'd');
            return v;
        case '[': {
            size_t n = 0;
            while (at < type.size() && type[at] >= '0' && type[at] <= '9')
                n = n * 10 + size_t(type[at++] - '0');
            if (at < type.size() && (type[at] == 'c' || type[at] == 'C')) {
                ++at;
                if (m_p + n > m_b.size()) {
                    m_bad = true;
                    return v;
                }
                v.kind = TValue::Bytes;
                v.s = m_b.substr(m_p, n);
                m_p += n;
            } else {
                v.kind = TValue::List;
                const size_t start = at;
                for (size_t i = 0; i < n && !m_bad; ++i) {
                    at = start;
                    v.list.push_back(value(type, at));
                }
            }
            if (at < type.size() && type[at] == ']')
                ++at;
            return v;
        }
        case '{':
        case '(': {
            const char close = t == '{' ? '}' : ')';
            while (at < type.size() && type[at] != '=' && type[at] != close)
                ++at;
            if (at < type.size() && type[at] == '=')
                ++at;
            v.kind = TValue::List;
            while (at < type.size() && type[at] != close && !m_bad)
                v.list.push_back(value(type, at));
            if (at < type.size())
                ++at;
            return v;
        }
        case '^':
            return value(type, at);
        default:
            // the integers (c C s S i I l L q Q B)
            v.kind = TValue::Int;
            v.i = integer(byte());
            return v;
        }
    }

    const std::string &m_b;
    size_t m_p = 0;
    bool m_little = true, m_bad = false, m_end = false;
    int m_depth = 0;
    std::vector<std::string> m_strings;
};

void flatten(const TValue &v, std::vector<const TValue *> &out)
{
    if (v.kind == TValue::List)
        for (const TValue &x : v.list)
            flatten(x, out);
    else
        out.push_back(&v);
}

std::vector<const TValue *> valuesOf(const TObject &o)
{
    std::vector<const TValue *> out;
    for (const TGroup &g : o.groups)
        for (const TValue &v : g.values)
            flatten(v, out);
    return out;
}

double number(const TValue *v) { return v->kind == TValue::Real ? v->r : double(v->i); }

// ---- writing, as NSArchiver writes (little endian: an Intel Mac's)

class Writer {
public:
    Writer()
    {
        m_out += char(4);
        m_out += char(11);
        m_out += "streamtyped";
        integer(1000);
    }
    std::string bytes() const { return m_out; }

    void integer(long long v)
    {
        if (v >= FirstRef && v <= 127) {
            m_out += char(int8_t(v));
        } else if (v >= INT16_MIN && v <= INT16_MAX) {
            m_out += char(Int2);
            raw(uint16_t(int16_t(v)), 2);
        } else {
            m_out += char(Int4);
            raw(uint32_t(int32_t(v)), 4);
        }
    }
    void real(double v, bool isDouble)
    {
        // a whole number goes as an integer
        if (std::floor(v) == v && std::fabs(v) < 2147483647.0) {
            integer((long long)v);
            return;
        }
        m_out += char(RealTag);
        if (isDouble) {
            uint64_t b;
            std::memcpy(&b, &v, 8);
            raw(b, 8);
        } else {
            const float f = float(v);
            uint32_t b;
            std::memcpy(&b, &f, 4);
            raw(b, 4);
        }
    }
    void exactDouble(double v)
    {
        m_out += char(RealTag);
        uint64_t b;
        std::memcpy(&b, &v, 8);
        raw(b, 8);
    }
    void shared(const std::string &s)
    {
        auto it = m_strings.find(s);
        if (it != m_strings.end()) {
            integer(it->second + FirstRef);
            return;
        }
        m_out += char(New);
        integer((long long)s.size());
        m_out += s;
        const int index = int(m_strings.size());
        m_strings[s] = index;
    }
    void group(const char *type) { shared(type); }
    // a new object of the class (its superclasses after it, each with its version)
    void beginObject(const std::vector<std::pair<const char *, int>> &chain)
    {
        m_out += char(New);
        ++m_entries;
        cls(chain, 0);
    }
    void endObject() { m_out += char(End); }
    void bytesOf(const std::string &b) { m_out += b; }

private:
    void cls(const std::vector<std::pair<const char *, int>> &chain, size_t i)
    {
        if (i >= chain.size()) {
            m_out += char(Nil);
            return;
        }
        auto it = m_classes.find(chain[i].first);
        if (it != m_classes.end()) {
            integer(it->second + FirstRef);
            return;
        }
        m_out += char(New);
        shared(chain[i].first);
        integer(chain[i].second);
        m_classes[chain[i].first] = m_entries++;
        cls(chain, i + 1);
    }
    void raw(uint64_t v, int n)
    {
        for (int i = 0; i < n; ++i)
            m_out += char((v >> (8 * i)) & 0xff);
    }
    std::string m_out;
    std::map<std::string, int> m_strings, m_classes;
    int m_entries = 0;
};

} // namespace

double nowSinceReferenceDate()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return double(tv.tv_sec) + tv.tv_usec / 1e6 - 978307200.0;   // 1 January 2001, UTC
}

bool readDatabase(const std::string &bytes, std::vector<Note> &notes, bool)
{
    notes.clear();
    Reader r(bytes);
    if (!r.header())
        return false;
    TGroup top;
    if (!r.group(top) || top.values.empty() || top.values[0].kind != TValue::Object)
        return false;
    const TObject &array = r.objects[size_t(top.values[0].ref)];
    for (const TGroup &g : array.groups) {
        if (g.type != "@" || g.values.empty() || g.values[0].kind != TValue::Object)
            continue;
        const TObject &doc = r.objects[size_t(g.values[0].ref)];
        if (doc.cls != "Document")
            continue;
        // the values in order: (c at version 0), the data, the flags, four floats, the colour, two dates
        std::vector<const TValue *> v = valuesOf(doc);
        size_t i = doc.version == 0 && !v.empty() && v[0]->kind == TValue::Int ? 1 : 0;
        if (v.size() < i + 9)
            continue;
        Note n;
        if (v[i]->kind == TValue::Object) {
            for (const TValue *d : valuesOf(r.objects[size_t(v[i]->ref)]))
                if (d->kind == TValue::Bytes)
                    n.rtfd += d->s;
        }
        n.flags = int(v[i + 1]->i);
        n.x = number(v[i + 2]);
        n.y = number(v[i + 3]);
        n.width = number(v[i + 4]);
        n.height = number(v[i + 5]);
        n.colour = int(v[i + 6]->i);
        auto date = [&](const TValue *d) {
            if (d->kind != TValue::Object)
                return 0.0;
            for (const TValue *x : valuesOf(r.objects[size_t(d->ref)]))
                if (x->kind == TValue::Real || x->kind == TValue::Int)
                    return number(x);
            return 0.0;
        };
        n.created = date(v[i + 7]);
        n.modified = date(v[i + 8]);
        notes.push_back(std::move(n));
    }
    return r.ok();
}

bool readDatabase(const std::string &path, std::vector<Note> &notes)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return readDatabase(bytes, notes, true);
}

std::string writeDatabase(const std::vector<Note> &notes)
{
    Writer w;
    w.group("@");
    w.beginObject({{"NSMutableArray", 0}, {"NSArray", 0}, {"NSObject", 0}});
    w.group("i");
    w.integer((long long)notes.size());
    for (const Note &n : notes) {
        w.group("@");
        w.beginObject({{"Document", 1}, {"NSObject", 0}});
        // -[Document encodeWithCoder:]
        w.group("@");
        w.beginObject({{"NSMutableData", 0}, {"NSData", 0}, {"NSObject", 0}});
        w.group("i");
        w.integer((long long)n.rtfd.size());
        w.group(("[" + std::to_string(n.rtfd.size()) + "c]").c_str());
        w.bytesOf(n.rtfd);
        w.endObject();
        w.group("i");
        w.integer(n.flags);
        w.group("{?={?=ff}{?=ff}}");
        w.real(n.x, false);
        w.real(n.y, false);
        w.real(n.width, false);
        w.real(n.height, false);
        w.group("i");
        w.integer(n.colour);
        for (double d : {n.created, n.modified}) {
            w.group("@");
            w.beginObject({{"NSDate", 0}, {"NSObject", 0}});
            w.group("d");
            w.exactDouble(d);
            w.endObject();
        }
        w.endObject();
    }
    w.endObject();
    return w.bytes();
}

bool saveDatabase(const std::string &path, const std::vector<Note> &notes)
{
    const std::string bytes = writeDatabase(notes);
    const std::string tmp = path + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(bytes.data(), std::streamsize(bytes.size()));
        if (!f)
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace Stickies
