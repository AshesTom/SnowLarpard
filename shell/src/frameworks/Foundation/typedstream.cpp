// SPDX-License-Identifier: LGPL-2.1-or-later
// NSArchiver's typedstream, as the older nibs (objects.nib) hold it, and its translation into a keyed
// archive's model so that AquaKit's Nib reads both formats the same way.
//
//   04, 0b "streamtyped" (little endian, Intel) or "typedstream" (big endian, PowerPC), the system's
//   version (1000); then groups: a type string (shared), its values
//   integers: a byte, or 0x81 + 2 bytes, 0x82 + 4 bytes; reals: 0x83 + 4 or 8 bytes,
//   or an integer; 0x84 a new entry, 0x85 nil, 0x86 an object's end; anything else a reference
//   (value + 110) to an earlier string, or object or class (one table for both)
//   an object: 0x84, its class (0x84, name, version, superclass... down to nil), its groups, 0x86
//
// What each class's groups hold (by place: NSArchiver's coding writes no names) was read from the 10.6
// nibs themselves, the keyed ones of the same classes beside them:
//   NSView      @ superview | i flags | @@@@ffffffff subviews, -, -, -, frame (from the bottom), bounds
//   NSControl   (the view's groups, 7) then icc@ tag, enabled, -, cell
//   NSCell      ii NSCellFlags, NSCellFlags2 | @@@@ contents, font, -, - | i: tag, action | @ | @ control
//   NSButtonCell (the cell's 5) ssii@@@@@ delay, interval, -, flags, then its images and key equivalent
//   NSSliderCell (the cell's 5) ddddiii maximum, minimum, value, -, tick marks, their position, only them
//   NSPopUpButtonCell (the button cell's 6) @@ selected item, - | i@i -, menu, -
//   NSMenu      i@@@ -, title, items | NSMenuItem @ menu | i@@IIi@@@@:i@ -, title, key, ...
//   NSBox       (the view's 7) ff@@ccc offsets, title cell, -, ...
//   NSTabView   (the view's 7) @@@icc items, selected item, font, ... | NSTabViewItem @@@@@@@ identifier, view, label
//   NSWindowTemplate iiffffi@@@@@c -, -, x, y, width, height, -, title, class, view class, content view
//   NSIBObjectData @ root | i n, n x @@ (object, parent) | i n, n x @@ (object, name) | @ | @ connections
//   connectors  @@@ source, destination, label;  NSClassSwapper @# its class's name, the class it stands for
#include "archive.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>

namespace AK {

namespace {

constexpr int New = -124, Nil = -123, End = -122, Int2 = -127, Int4 = -126, RealTag = -125, FirstRef = -110;

// what the stream holds
struct TValue {
    enum Kind { None, Int, Real, Bytes, Object, Class, List } kind = None;
    long long i = 0;
    double r = 0;
    std::string s;
    int ref = -1;   // Object, Class: the table's entry
    std::vector<TValue> list;
};

struct TEntry {
    bool isClass = false;
    std::string cls;                      // an object's class, a class's name
    std::vector<std::string> chain;       // an object's class and its superclasses
    int super = -1;                       // a class's superclass
    std::vector<std::pair<std::string, std::vector<TValue>>> groups;
};

class Reader {
public:
    explicit Reader(std::string b) : m_b(std::move(b)) {}
    std::vector<TEntry> table;
    bool ok = true;

    TValue top()
    {
        if (m_b.size() < 16 || byte() != 4)
            return fail();
        const int n = byte();
        // the signature tells the byte order
        if (m_b.compare(m_p, size_t(n), "streamtyped") == 0)
            m_big = false;
        else if (m_b.compare(m_p, size_t(n), "typedstream") == 0)
            m_big = true;
        else
            return fail();
        m_p += size_t(n);
        integer(sbyte());   // the system's version
        const std::string types = sharedString();
        std::vector<TValue> v = values(types);
        return v.empty() ? TValue() : v[0];
    }

private:
    TValue fail()
    {
        ok = false;
        return {};
    }
    int byte() { return m_p < m_b.size() ? static_cast<unsigned char>(m_b[m_p++]) : (ok = false, 0); }
    int sbyte() { return static_cast<signed char>(byte()); }
    // n bytes as a number, in the stream's order
    unsigned long long bytesOf(int n)
    {
        unsigned long long v = 0;
        for (int i = 0; i < n; ++i) {
            const size_t at = m_p + size_t(m_big ? i : n - 1 - i);
            v = (v << 8) | (at < m_b.size() ? static_cast<unsigned char>(m_b[at]) : 0);
        }
        m_p += size_t(n);
        return v;
    }
    long long integer(int head)
    {
        if (head == Int2)
            return (short)bytesOf(2);
        if (head == Int4)
            return (int)bytesOf(4);
        return head;
    }
    double real(int size)
    {
        const int head = sbyte();
        if (head != RealTag)
            return double(integer(head));
        const unsigned long long bits = bytesOf(size);
        if (size == 4) {
            float f;
            const uint32_t b = uint32_t(bits);
            std::memcpy(&f, &b, 4);
            return f;
        }
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    std::string sharedString()
    {
        const int head = sbyte();
        if (head == Nil)
            return {};
        if (head == New) {
            const long long n = integer(sbyte());
            std::string v = n > 0 && m_p + size_t(n) <= m_b.size() ? m_b.substr(m_p, size_t(n)) : std::string();
            m_p += size_t(std::max(0LL, n));
            m_strings.push_back(v);
            return v;
        }
        const long long at = integer(head) - FirstRef;
        return at >= 0 && size_t(at) < m_strings.size() ? m_strings[size_t(at)] : (ok = false, std::string());
    }
    int readClass()
    {
        const int head = sbyte();
        if (head == Nil)
            return -1;
        if (head == New) {
            TEntry c;
            c.isClass = true;
            c.cls = sharedString();
            integer(sbyte());   // its version
            const int at = int(table.size());
            table.push_back(c);
            const int super = readClass();
            table[size_t(at)].super = super;
            return at;
        }
        const long long at = integer(head) - FirstRef;
        return at >= 0 && size_t(at) < table.size() ? int(at) : (ok = false, -1);
    }
    TValue readObject()
    {
        TValue v;
        const int head = sbyte();
        if (head == Nil)
            return v;
        v.kind = TValue::Object;
        if (head != New) {
            v.ref = int(integer(head) - FirstRef);
            if (v.ref < 0 || size_t(v.ref) >= table.size())
                ok = false;
            return v;
        }
        const int at = int(table.size());
        table.emplace_back();
        v.ref = at;
        const int cls = readClass();
        for (int c = cls; c >= 0 && ok; c = table[size_t(c)].super)
            table[size_t(at)].chain.push_back(table[size_t(c)].cls);
        table[size_t(at)].cls = cls >= 0 ? table[size_t(cls)].cls : std::string();
        while (ok && m_p < m_b.size()) {
            if (static_cast<signed char>(m_b[m_p]) == End) {
                ++m_p;
                break;
            }
            const std::string types = sharedString();
            std::vector<TValue> vals = values(types);
            table[size_t(at)].groups.emplace_back(types, std::move(vals));
        }
        return v;
    }
    std::vector<TValue> values(const std::string &t)
    {
        std::vector<TValue> out;
        for (size_t i = 0; i < t.size() && ok;)
            out.push_back(value(t, i));
        return out;
    }
    TValue value(const std::string &t, size_t &i)
    {
        TValue v;
        const char c = t[i];
        if (std::strchr("cCsSiIlLqQB", c)) {
            v.kind = TValue::Int;
            v.i = integer(sbyte());
            ++i;
        } else if (c == 'f' || c == 'd') {
            v.kind = TValue::Real;
            v.r = real(c == 'f' ? 4 : 8);
            v.i = (long long)v.r;
            ++i;
        } else if (c == '@') {
            v = readObject();
            ++i;
        } else if (c == '#') {
            v.kind = TValue::Class;
            v.ref = readClass();
            ++i;
        } else if (c == '*' || c == '%' || c == ':') {
            v.kind = TValue::Bytes;
            v.s = sharedString();
            ++i;
        } else if (c == '+') {
            v.kind = TValue::Bytes;
            const long long n = integer(sbyte());
            v.s = n > 0 && m_p + size_t(n) <= m_b.size() ? m_b.substr(m_p, size_t(n)) : std::string();
            m_p += size_t(std::max(0LL, n));
            ++i;
        } else if (c == '[') {
            size_t j = i + 1;
            long long n = 0;
            while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j])))
                n = n * 10 + (t[j++] - '0');
            const size_t close = t.find(']', j);
            if (t[j] == 'c' || t[j] == 'C') {
                v.kind = TValue::Bytes;
                v.s = m_p + size_t(n) <= m_b.size() ? m_b.substr(m_p, size_t(n)) : std::string();
                m_p += size_t(n);
            } else {
                v.kind = TValue::List;
                for (long long k = 0; k < n && ok; ++k) {
                    size_t e = j;
                    v.list.push_back(value(t, e));
                }
            }
            i = close == std::string::npos ? t.size() : close + 1;
        } else if (c == '{') {
            int depth = 0;
            size_t k = i;
            for (; k < t.size(); ++k) {
                if (t[k] == '{')
                    ++depth;
                else if (t[k] == '}' && --depth == 0)
                    break;
            }
            const size_t eq = t.find('=', i);
            v.kind = TValue::List;
            v.list = values(t.substr(eq + 1, k - eq - 1));
            i = k + 1;
        } else {
            ok = false;
            ++i;
        }
        return v;
    }
    std::string m_b;
    size_t m_p = 0;
    bool m_big = false;
    std::vector<std::string> m_strings;
};

} // namespace

// the stream's objects as NSKeyedArchiver would have archived them
class TypedStreamTranslator {
public:
    TypedStreamTranslator(Archive &a, std::vector<TEntry> &t) : m_a(a), m_t(t) {}

    bool run()
    {
        // $objects: one place per entry of the stream's table (classes stay empty)
        m_slots.assign(m_t.size(), -1);
        for (size_t i = 0; i < m_t.size(); ++i)
            m_slots[i] = newValue(Archive::Value::Dict);
        for (size_t i = 0; i < m_t.size(); ++i)
            if (!m_t[i].isClass)
                translate(int(i));
        const int objects = newValue(Archive::Value::Array);
        for (int s : m_slots)
            m_a.m_values[size_t(objects)].items.push_back(s);
        const int top = newValue(Archive::Value::Dict);
        put(top, "IB.objectdata", uid(0));
        const int root = newValue(Archive::Value::Dict);
        put(root, "$objects", objects);
        put(root, "$top", top);
        m_a.m_root = root;
        m_a.m_objects = objects;
        return !m_t.empty() && m_t[0].cls == "NSIBObjectData";
    }

private:
    int newValue(Archive::Value::Type type)
    {
        Archive::Value v;
        v.type = type;
        m_a.m_values.push_back(v);
        return int(m_a.m_values.size()) - 1;
    }
    int str(const std::string &s)
    {
        const int v = newValue(Archive::Value::String);
        m_a.m_values[size_t(v)].s = s;
        return v;
    }
    int num(long long n)
    {
        const int v = newValue(Archive::Value::Int);
        m_a.m_values[size_t(v)].i = n;
        return v;
    }
    int real(double r)
    {
        const int v = newValue(Archive::Value::Real);
        m_a.m_values[size_t(v)].r = r;
        return v;
    }
    int uid(int entry)
    {
        const int v = newValue(Archive::Value::Uid);
        m_a.m_values[size_t(v)].i = entry;
        return v;
    }
    void put(int dict, const std::string &key, int value)
    {
        if (value < 0)
            return;
        // the key made first: making a value may move the others (a reference into them would dangle)
        const int k = str(key);
        auto &d = m_a.m_values[size_t(dict)];
        d.keys.push_back(k);
        d.items.push_back(value);
    }
    void putObject(int dict, const std::string &key, const TValue &v)
    {
        if (v.kind == TValue::Object && v.ref >= 0)
            put(dict, key, uid(v.ref));
        else if (v.kind == TValue::Bytes)
            put(dict, key, str(v.s));
    }
    // the value at a group's place (empty when the stream has fewer)
    static const TValue &at(const TEntry &e, size_t group, size_t index)
    {
        static const TValue none;
        return group < e.groups.size() && index < e.groups[group].second.size() ? e.groups[group].second[index] : none;
    }
    bool is(const TEntry &e, const char *cls) const
    {
        for (const std::string &c : e.chain)
            if (c == cls)
                return true;
        return false;
    }
    std::string textOf(const TValue &v) const
    {
        if (v.kind == TValue::Bytes)
            return v.s;
        if (v.kind == TValue::Object && v.ref >= 0) {
            const TEntry &e = m_t[size_t(v.ref)];
            if (is(e, "NSString") && !e.groups.empty())
                return at(e, 0, 0).s;
        }
        return {};
    }
    std::string rect(double x, double y, double w, double h) const
    {
        auto n = [](double d) {
            char b[32];
            std::snprintf(b, sizeof b, "%g", d);
            return std::string(b);
        };
        return "{{" + n(x) + ", " + n(y) + "}, {" + n(w) + ", " + n(h) + "}}";
    }

    void translate(int i)
    {
        TEntry &e = m_t[size_t(i)];
        const int d = m_slots[size_t(i)];
        // its class: $classname and $classes (an NSClassSwapper's stand-in keeps that name)
        size_t first = 0;   // where the class's own coding starts (after an NSClassSwapper's)
        std::vector<std::string> chain = e.chain;
        std::string swapped;
        if (e.cls == "NSClassSwapper") {
            swapped = textOf(at(e, 0, 0));
            const TValue &original = at(e, 0, 1);
            chain.clear();
            for (int c = original.ref; c >= 0; c = m_t[size_t(c)].super)
                chain.push_back(m_t[size_t(c)].cls);
            first = 1;
        }
        const int cls = newValue(Archive::Value::Dict);
        put(cls, "$classname", str(e.cls));
        const int classes = newValue(Archive::Value::Array);
        {
            const int item = str(e.cls);
            m_a.m_values[size_t(classes)].items.push_back(item);
        }
        for (const std::string &c : chain)
            {
            const int item = str(c);
            m_a.m_values[size_t(classes)].items.push_back(item);
        }
        put(cls, "$classes", classes);
        put(d, "$class", cls);
        if (!swapped.empty()) {
            put(d, "NSClassName", str(swapped));
            put(d, "NSOriginalClassName", str(chain.empty() ? std::string("NSView") : chain[0]));
        }
        TEntry view = e;   // the groups from the class's own coding on
        view.groups.erase(view.groups.begin(), view.groups.begin() + long(std::min(first, view.groups.size())));
        view.chain = chain;

        auto has = [&](const char *c) { return std::find(chain.begin(), chain.end(), c) != chain.end(); };
        if (has("NSString")) {
            // a string: its bytes stand for it (Archive::stringOf reads a dictionary's NS.string)
            put(d, "NS.string", str(textOf(at(view, 0, 0))));
            return;
        }
        if (has("NSArray") || has("NSSet")) {
            const int list = newValue(Archive::Value::Array);
            for (size_t g = 1; g < view.groups.size(); ++g)
                for (const TValue &v : view.groups[g].second)
                    if (v.kind == TValue::Object && v.ref >= 0)
                        {
            const int item = uid(v.ref);
            m_a.m_values[size_t(list)].items.push_back(item);
        }
            put(d, "NS.objects", list);
            return;
        }
        if (e.cls == "NSIBObjectData") {
            putObject(d, "NSRoot", at(view, 0, 0));
            // the (object, parent) pairs: the top level objects with them
            const long long n = at(view, 1, 0).i;
            const int keys = newValue(Archive::Value::Array);
            for (long long k = 0; k < n; ++k) {
                const TValue &o = at(view, size_t(2 + k), 0);
                if (o.kind == TValue::Object && o.ref >= 0)
                    {
            const int item = uid(o.ref);
            m_a.m_values[size_t(keys)].items.push_back(item);
        }
            }
            const int list = newValue(Archive::Value::Dict);
            put(list, "NS.objects", keys);
            put(d, "NSObjectsKeys", list);
            // after the names' pairs: a set, then the connections
            const size_t names = size_t(2 + n);
            const long long m = at(view, names, 0).i;
            putObject(d, "NSConnections", at(view, names + 1 + size_t(m) + 1, 0));
            return;
        }
        if (e.cls == "NSNibOutletConnector" || e.cls == "NSNibControlConnector") {
            putObject(d, "NSSource", at(view, 0, 0));
            putObject(d, "NSDestination", at(view, 0, 1));
            put(d, "NSLabel", str(textOf(at(view, 0, 2))));
            return;
        }
        if (e.cls == "NSWindowTemplate") {
            put(d, "NSWindowRect", str(rect(at(view, 0, 2).r, at(view, 0, 3).r, at(view, 0, 4).r, at(view, 0, 5).r)));
            put(d, "NSWindowTitle", str(textOf(at(view, 0, 7))));
            putObject(d, "NSWindowView", at(view, 0, 10));
            return;
        }
        if (e.cls == "NSFont") {
            // its name as UTF-16 (after a byte order mark) in the [c] bytes, its size
            const std::string &raw = at(view, 1, 0).s;
            std::string name;
            const size_t bom = raw.find("\xff\xfe");
            for (size_t k = bom == std::string::npos ? raw.size() : bom + 2; k + 1 < raw.size(); k += 2)
                if (raw[k])
                    name += raw[k];
            put(d, "NSName", str(name));
            put(d, "NSSize", real(at(view, 2, 0).r));
            return;
        }
        if (e.cls == "NSColor") {
            if (at(view, 0, 0).i == 6)
                put(d, "NSColorName", str(textOf(at(view, 1, 1))));
            return;
        }
        if (e.cls == "NSButtonImageSource") {
            put(d, "NSImageName", str(textOf(at(view, 0, 0))));
            return;
        }
        if (e.cls == "NSCustomResource") {
            put(d, "NSClassName", str(textOf(at(view, 0, 0))));
            put(d, "NSResourceName", str(textOf(at(view, 0, 1))));
            return;
        }
        if (e.cls == "NSMenu") {
            put(d, "NSTitle", str(textOf(at(view, 0, 1))));
            putObject(d, "NSMenuItems", at(view, 0, 2));
            return;
        }
        if (e.cls == "NSMenuItem") {
            const std::string title = textOf(at(view, 1, 1));
            put(d, "NSTitle", str(title));
            putObject(d, "NSMenu", at(view, 0, 0));
            return;
        }
        if (e.cls == "NSTabViewItem") {
            putObject(d, "NSIdentifier", at(view, 0, 0));
            putObject(d, "NSView", at(view, 0, 1));
            put(d, "NSLabel", str(textOf(at(view, 0, 2))));
            return;
        }
        if (has("NSCell")) {
            put(d, "NSCellFlags", num(at(view, 0, 0).i));
            put(d, "NSCellFlags2", num(at(view, 0, 1).i));
            put(d, "NSContents", str(textOf(at(view, 1, 0))));
            putObject(d, "NSSupport", at(view, 1, 1));
            put(d, "NSTag", num(at(view, 2, 0).i));
            putObject(d, "NSControlView", at(view, 4, 0));
            if (has("NSButtonCell")) {
                // the images: NSSwitch, NSRadioButton among them make check boxes and radio buttons
                for (const TValue &v : view.groups.size() > 5 ? view.groups[5].second : std::vector<TValue>{})
                    if (v.kind == TValue::Object && v.ref >= 0 && m_t[size_t(v.ref)].cls == "NSButtonImageSource")
                        put(d, "NSAlternateImage", uid(v.ref));
                for (const TValue &v : view.groups.size() > 5 ? view.groups[5].second : std::vector<TValue>{})
                    if (textOf(v) == "\r")
                        put(d, "NSKeyEquivalent", str("\r"));
            }
            if (has("NSSliderCell")) {
                put(d, "NSMaxValue", real(at(view, 5, 0).r));
                put(d, "NSMinValue", real(at(view, 5, 1).r));
                put(d, "NSValue", real(at(view, 5, 2).r));
                put(d, "NSNumberOfTickMarks", num(at(view, 5, 4).i));
                put(d, "NSTickMarkPosition", num(at(view, 5, 5).i));
                put(d, "NSAllowsTickMarkValuesOnly", num(at(view, 5, 6).i));
            }
            if (has("NSPopUpButtonCell")) {
                putObject(d, "NSMenu", at(view, 7, 1));
                // the selected item's place in its menu
                const TValue &item = at(view, 6, 0), &menu = at(view, 7, 1);
                if (item.ref >= 0 && menu.ref >= 0) {
                    const TValue &items = at(m_t[size_t(menu.ref)], 0, 2);
                    if (items.ref >= 0) {
                        const TEntry &list = m_t[size_t(items.ref)];
                        for (size_t g = 1; g < list.groups.size(); ++g)
                            if (at(list, g, 0).ref == item.ref)
                                put(d, "NSSelectedIndex", num(long(g - 1)));
                    }
                }
            }
            if (has("NSTextFieldCell"))
                putObject(d, "NSTextColor", at(view, 5, 2));
            return;
        }
        if (has("NSView")) {
            putObject(d, "NSSuperview", at(view, 0, 0));
            put(d, "NSvFlags", num(at(view, 1, 0).i));
            putObject(d, "NSSubviews", at(view, 2, 0));
            put(d, "NSFrame", str(rect(at(view, 2, 4).r, at(view, 2, 5).r, at(view, 2, 6).r, at(view, 2, 7).r)));
            // the view's own coding is 7 groups; its subclass's follow
            if (has("NSControl")) {
                put(d, "NSTag", num(at(view, 7, 0).i));
                put(d, "NSEnabled", num(at(view, 7, 1).i));
                putObject(d, "NSCell", at(view, 7, 3));
            }
            if (e.cls == "NSCustomView")
                put(d, "NSClassName", str(textOf(at(view, 7, 0))));
            if (has("NSTabView")) {
                putObject(d, "NSTabViewItems", at(view, 7, 0));
                putObject(d, "NSSelectedTabViewItem", at(view, 7, 1));
            }
            if (has("NSBox")) {
                // a box a few points thin is a line
                const double w = at(view, 2, 6).r, h = at(view, 2, 7).r;
                put(d, "NSBoxType", num(w <= 5 || h <= 5 ? 2 : 0));
                putObject(d, "NSTitleCell", at(view, 7, 2));
                const TValue &cell = at(view, 7, 2);
                const std::string title = cell.ref >= 0 ? textOf(at(m_t[size_t(cell.ref)], 1, 0)) : std::string();
                put(d, "NSTitlePosition", num(title.empty() || title == "Title" || title == "Box" ? 0 : 2));
            }
        }
    }

    Archive &m_a;
    std::vector<TEntry> &m_t;
    std::vector<int> m_slots;
};

bool Archive::loadTypedStream(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    m_values.clear();
    m_objects = m_root = -1;
    Reader r(std::move(bytes));
    r.top();
    if (!r.ok || r.table.empty())
        return false;
    TypedStreamTranslator t(*this, r.table);
    return t.run();
}

} // namespace AK
