// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dsstore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>

namespace Finder {
namespace DSStore {

namespace {

constexpr uint32_t PageSize = 4096;

uint32_t be32(const std::string &b, size_t at)
{
    if (at + 4 > b.size())
        return 0;
    const auto *p = reinterpret_cast<const unsigned char *>(b.data() + at);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t be64(const std::string &b, size_t at) { return uint64_t(be32(b, at)) << 32 | be32(b, at + 4); }

void put32(std::string &out, uint32_t v)
{
    const char bytes[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    out.append(bytes, 4);
}

void put64(std::string &out, uint64_t v)
{
    put32(out, uint32_t(v >> 32));
    put32(out, uint32_t(v));
}

std::u16string toUtf16(const std::string &s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c >> 5) == 6) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c >> 4) == 14) {
            cp = c & 0x0F;
            n = 3;
        } else {
            cp = c & 0x07;
            n = 4;
        }
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k)
            cp = cp << 6 | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
        i += size_t(n);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xD800 + (cp >> 10));
            out += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}

std::string toUtf8(const std::u16string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::u16string utf16be(const std::string &b, size_t at, size_t units)
{
    std::u16string out;
    for (size_t k = 0; k < units && at + 2 * k + 1 < b.size(); ++k)
        out += char16_t(static_cast<unsigned char>(b[at + 2 * k]) << 8 | static_cast<unsigned char>(b[at + 2 * k + 1]));
    return out;
}

void putUtf16(std::string &out, const std::u16string &s)
{
    for (char16_t c : s) {
        out += char(c >> 8);
        out += char(c & 0xFF);
    }
}

// the tree's order: the names without case (UTF-16 units folded), then the codes
bool before(const Record &a, const Record &b)
{
    const std::u16string x = toUtf16(a.name), y = toUtf16(b.name);
    for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
        const wint_t cx = std::towlower(x[i]), cy = std::towlower(y[i]);
        if (cx != cy)
            return cx < cy;
    }
    if (x.size() != y.size())
        return x.size() < y.size();
    return a.code < b.code;
}

std::string serialize(const Record &r)
{
    std::string out;
    const std::u16string name = toUtf16(r.name);
    put32(out, uint32_t(name.size()));
    putUtf16(out, name);
    out += (r.code + "    ").substr(0, 4);
    out += (r.type + "    ").substr(0, 4);
    if (r.type == "long" || r.type == "shor") {
        put32(out, uint32_t(r.number));
    } else if (r.type == "bool") {
        out += char(r.number ? 1 : 0);
    } else if (r.type == "type") {
        out += (r.data + "    ").substr(0, 4);
    } else if (r.type == "comp" || r.type == "dutc") {
        put64(out, uint64_t(r.number));
    } else if (r.type == "ustr") {
        const std::u16string s = toUtf16(r.data);
        put32(out, uint32_t(s.size()));
        putUtf16(out, s);
    } else {   // blob
        put32(out, uint32_t(r.data.size()));
        out += r.data;
    }
    return out;
}

// one record at `at`; false when it isn't one
bool parseRecord(const std::string &b, size_t &at, Record &r)
{
    const uint32_t units = be32(b, at);
    if (at + 4 + 2 * size_t(units) + 8 > b.size())
        return false;
    r.name = toUtf8(utf16be(b, at + 4, units));
    at += 4 + 2 * size_t(units);
    r.code = b.substr(at, 4);
    r.type = b.substr(at + 4, 4);
    at += 8;
    if (r.type == "long" || r.type == "shor") {
        r.number = be32(b, at);
        at += 4;
    } else if (r.type == "bool") {
        r.number = at < b.size() ? static_cast<unsigned char>(b[at]) : 0;
        at += 1;
    } else if (r.type == "type") {
        r.data = b.substr(at, 4);
        at += 4;
    } else if (r.type == "comp" || r.type == "dutc") {
        r.number = int64_t(be64(b, at));
        at += 8;
    } else if (r.type == "blob") {
        const uint32_t n = be32(b, at);
        r.data = b.substr(at + 4, n);
        at += 4 + n;
    } else if (r.type == "ustr") {
        const uint32_t n = be32(b, at);
        r.data = toUtf8(utf16be(b, at + 4, n));
        at += 4 + 2 * size_t(n);
    } else {
        return false;
    }
    return at <= b.size();
}

} // namespace

bool read(const std::string &path, std::vector<Record> &records)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    const std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 36 || be32(b, 0) != 1 || b.compare(4, 4, "Bud1") != 0)
        return false;
    // the buddy allocator's offsets count from byte 4
    const uint32_t root = be32(b, 8);
    const size_t info = size_t(root) + 4;
    const uint32_t count = be32(b, info);
    if (count > 1 << 20)
        return false;
    std::vector<uint32_t> addresses;
    for (uint32_t i = 0; i < count; ++i)
        addresses.push_back(be32(b, info + 8 + 4 * i));
    size_t p = info + 8 + 4 * ((size_t(count) + 255) / 256 * 256);
    const uint32_t tocCount = be32(b, p);
    p += 4;
    uint32_t dsdb = UINT32_MAX;
    for (uint32_t i = 0; i < tocCount && p < b.size(); ++i) {
        const size_t len = static_cast<unsigned char>(b[p]);
        const std::string name = b.substr(p + 1, len);
        const uint32_t id = be32(b, p + 1 + len);
        p += 1 + len + 4;
        if (name == "DSDB")
            dsdb = id;
    }
    auto block = [&](uint32_t id) -> size_t { return id < addresses.size() ? size_t(addresses[id] & ~0x1Fu) + 4 : b.size(); };
    if (dsdb == UINT32_MAX)
        return false;
    const uint32_t rootNode = be32(b, block(dsdb));
    records.clear();
    int guard = 0;
    std::function<bool(uint32_t)> node = [&](uint32_t id) {
        if (++guard > 100000)
            return false;
        size_t at = block(id);
        const uint32_t next = be32(b, at), n = be32(b, at + 4);
        at += 8;
        for (uint32_t k = 0; k < n; ++k) {
            if (next) {   // an internal node: each record after its child
                const uint32_t child = be32(b, at);
                at += 4;
                if (!node(child))
                    return false;
            }
            Record r;
            if (!parseRecord(b, at, r))
                return false;
            records.push_back(std::move(r));
        }
        return next ? node(next) : true;
    };
    return node(rootNode);
}

bool write(const std::string &path, std::vector<Record> records)
{
    std::stable_sort(records.begin(), records.end(), before);
    std::vector<std::string> bytes;
    for (const Record &r : records)
        bytes.push_back(serialize(r));

    // the nodes: one leaf when it fits a page, else leaves under one root (its records the ones
    // between the leaves)
    struct Node {
        std::vector<size_t> records;
        std::vector<int> children;   // internal: before each record, then the last
    };
    std::vector<Node> leaves;
    std::vector<size_t> separators;
    {
        Node cur;
        size_t size = 8;
        for (size_t i = 0; i < records.size(); ++i) {
            if (size + bytes[i].size() > PageSize && !cur.records.empty()) {
                leaves.push_back(cur);
                cur = Node();
                size = 8;
                separators.push_back(i);   // this one goes up into the root
                continue;
            }
            cur.records.push_back(i);
            size += bytes[i].size();
        }
        leaves.push_back(cur);
    }
    const bool deep = leaves.size() > 1;
    std::vector<std::string> nodeBytes;   // block 2 on: the root first
    auto leafBytes = [&](const Node &n) {
        std::string out;
        put32(out, 0);
        put32(out, uint32_t(n.records.size()));
        for (size_t i : n.records)
            out += bytes[i];
        return out;
    };
    if (!deep) {
        nodeBytes.push_back(leafBytes(leaves.front()));
    } else {
        std::string root;
        put32(root, uint32_t(2 + leaves.size()));   // the last leaf
        put32(root, uint32_t(separators.size()));
        for (size_t k = 0; k < separators.size(); ++k) {
            put32(root, uint32_t(3 + k));   // the leaf before it
            root += bytes[separators[k]];
        }
        if (root.size() > PageSize)
            return false;   // more than a two-level tree holds: not written
        nodeBytes.push_back(root);
        for (const Node &n : leaves)
            nodeBytes.push_back(leafBytes(n));
    }

    // the buddy allocator over 2^31 bytes: the header's 32 bytes first, then the blocks
    std::vector<std::vector<uint32_t>> free(32);
    free[31].push_back(0);
    auto alloc = [&](int log) -> uint32_t {
        int k = log;
        while (k < 32 && free[size_t(k)].empty())
            ++k;
        if (k >= 32)
            return UINT32_MAX;
        const uint32_t off = free[size_t(k)].front();
        free[size_t(k)].erase(free[size_t(k)].begin());
        while (k > log) {
            --k;
            free[size_t(k)].push_back(off + (1u << k));
            std::sort(free[size_t(k)].begin(), free[size_t(k)].end());
        }
        return off;
    };
    auto logFor = [](size_t n) {
        int log = 5;
        while ((size_t(1) << log) < n)
            ++log;
        return log;
    };
    alloc(5);   // the header
    const int infoLog = 11;
    std::vector<uint32_t> addresses;
    const uint32_t infoOff = alloc(infoLog);
    addresses.push_back(infoOff | uint32_t(infoLog));
    const uint32_t dsdbOff = alloc(5);
    addresses.push_back(dsdbOff | 5u);
    std::vector<uint32_t> nodeOff;
    for (const std::string &n : nodeBytes) {
        const int log = logFor(n.size());
        const uint32_t off = alloc(log);
        nodeOff.push_back(off);
        addresses.push_back(off | uint32_t(log));
    }

    std::string info;
    put32(info, uint32_t(addresses.size()));
    put32(info, 0);
    for (size_t i = 0; i < (addresses.size() + 255) / 256 * 256; ++i)
        put32(info, i < addresses.size() ? addresses[i] : 0);
    put32(info, 1);
    info += char(4);
    info += "DSDB";
    put32(info, 1);
    for (const auto &list : free) {
        put32(info, uint32_t(list.size()));
        for (uint32_t off : list)
            put32(info, off);
    }
    if (info.size() > (1u << infoLog))
        return false;

    std::string dsdb;
    put32(dsdb, 2);                                       // the root node's block
    put32(dsdb, deep ? 1 : 0);                            // the levels above the leaves
    put32(dsdb, uint32_t(records.size()));
    put32(dsdb, uint32_t(nodeBytes.size()));
    put32(dsdb, PageSize);

    uint32_t end = 0;
    for (uint32_t a : addresses)
        end = std::max(end, (a & ~0x1Fu) + (1u << (a & 0x1F)));
    std::string file(size_t(end) + 4, '\0');
    auto place = [&](uint32_t off, const std::string &data) { std::memcpy(&file[size_t(off) + 4], data.data(), data.size()); };
    std::string header;
    put32(header, 1);
    header += "Bud1";
    put32(header, infoOff);
    put32(header, 1u << infoLog);
    put32(header, infoOff);
    put32(header, addresses[2]);   // as the Finder's files have it: the root node's address
    std::memcpy(&file[0], header.data(), header.size());
    place(infoOff, info);
    place(dsdbOff, dsdb);
    for (size_t i = 0; i < nodeBytes.size(); ++i)
        place(nodeOff[i], nodeBytes[i]);

    const std::string tmp = path + ".new";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.write(file.data(), std::streamsize(file.size())))
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// ---- Store

Store::Store(const std::string &folder) : m_path(folder + (folder == "/" ? "" : "/") + ".DS_Store")
{
    std::vector<Record> records;
    if (read(m_path, records))
        for (Record &r : records)
            m_records[{r.name, r.code}] = std::move(r);
}

const Record *Store::get(const std::string &name, const std::string &code) const
{
    auto it = m_records.find({name, code});
    return it == m_records.end() ? nullptr : &it->second;
}

void Store::set(const Record &r)
{
    auto &slot = m_records[{r.name, r.code}];
    if (slot.type == r.type && slot.number == r.number && slot.data == r.data && slot.name == r.name)
        return;
    slot = r;
    m_changed = true;
}

void Store::remove(const std::string &name, const std::string &code)
{
    if (m_records.erase({name, code}))
        m_changed = true;
}

bool Store::save()
{
    if (!m_changed)
        return true;
    std::vector<Record> records;
    for (const auto &kv : m_records)
        records.push_back(kv.second);
    if (!write(m_path, records))
        return false;
    m_changed = false;
    return true;
}

Record blob(const std::string &name, const std::string &code, const std::string &bytes)
{
    Record r;
    r.name = name;
    r.code = code;
    r.type = "blob";
    r.data = bytes;
    return r;
}

Record fourCC(const std::string &name, const std::string &code, const std::string &value)
{
    Record r;
    r.name = name;
    r.code = code;
    r.type = "type";
    r.data = value;
    return r;
}

bool iconLocation(const Record &r, int &x, int &y)
{
    if (r.type != "blob" || r.data.size() < 8)
        return false;
    x = int(be32(r.data, 0));
    y = int(be32(r.data, 4));
    return true;
}

Record iconLocation(const std::string &name, int x, int y)
{
    std::string b;
    put32(b, uint32_t(x));
    put32(b, uint32_t(y));
    b += std::string("\xff\xff\xff\xff\xff\xff\x00\x00", 8);
    return blob(name, "Iloc", b);
}

} // namespace DSStore

// ---- binary plists

const PList *PList::get(const std::string &key) const
{
    for (const auto &kv : dict)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

void PList::set(const std::string &key, PList value)
{
    type = Dict;
    for (auto &kv : dict)
        if (kv.first == key) {
            kv.second = std::move(value);
            return;
        }
    dict.emplace_back(key, std::move(value));
}

double PList::number(double fallback) const
{
    switch (type) {
    case Integer: return double(i);
    case Real: return r;
    case Bool: return b ? 1 : 0;
    default: return fallback;
    }
}

bool PList::parse(const std::string &d, PList &out)
{
    // bplist00: the objects, an offset table, a 32-byte trailer (offset size, ref size, object count,
    // top object, the table's offset)
    if (d.size() < 40 || d.compare(0, 8, "bplist00") != 0)
        return false;
    const size_t t = d.size() - 32;
    const int offSize = static_cast<unsigned char>(d[t + 6]), refSize = static_cast<unsigned char>(d[t + 7]);
    const uint64_t count = DSStore::be64(d, t + 8), top = DSStore::be64(d, t + 16), table = DSStore::be64(d, t + 24);
    auto uint = [&](size_t at, int size) {
        uint64_t v = 0;
        for (int k = 0; k < size && at + size_t(k) < d.size(); ++k)
            v = v << 8 | static_cast<unsigned char>(d[at + size_t(k)]);
        return v;
    };
    if (count > 1 << 20 || table + count * uint64_t(offSize) > d.size())
        return false;
    int depth = 0;
    std::function<bool(uint64_t, PList &)> object = [&](uint64_t ref, PList &p) -> bool {
        if (ref >= count || ++depth > 64)
            return false;
        size_t at = size_t(uint(size_t(table + ref * uint64_t(offSize)), offSize));
        if (at >= t)
            return false;
        const unsigned char marker = static_cast<unsigned char>(d[at]);
        const int hi = marker >> 4, lo = marker & 0xF;
        auto length = [&]() -> uint64_t {
            if (lo != 0xF) {
                ++at;
                return uint64_t(lo);
            }
            const int intSize = 1 << (static_cast<unsigned char>(d[at + 1]) & 0xF);
            const uint64_t n = uint(at + 2, intSize);
            at += 2 + size_t(intSize);
            return n;
        };
        bool ok = true;
        switch (hi) {
        case 0x0:
            p.type = marker == 0x08 || marker == 0x09 ? Bool : Null;
            p.b = marker == 0x09;
            break;
        case 0x1: {
            const int size = 1 << lo;
            p.type = Integer;
            p.i = int64_t(uint(at + 1, size));
            if (size == 8)
                p.i = int64_t(uint(at + 1, 8));
            break;
        }
        case 0x2: {
            p.type = Real;
            const uint64_t bits = uint(at + 1, 1 << lo);
            if (lo == 2) {
                uint32_t b32 = uint32_t(bits);
                float f;
                std::memcpy(&f, &b32, 4);
                p.r = f;
            } else {
                std::memcpy(&p.r, &bits, 8);
            }
            break;
        }
        case 0x4: {
            const uint64_t n = length();
            p.type = Data;
            p.s = d.substr(at, size_t(n));
            break;
        }
        case 0x5: {
            const uint64_t n = length();
            p.type = String;
            p.s = d.substr(at, size_t(n));
            break;
        }
        case 0x6: {
            const uint64_t n = length();
            p.type = String;
            p.s = DSStore::toUtf8(DSStore::utf16be(d, at, size_t(n)));
            break;
        }
        case 0xA: {
            const uint64_t n = length();
            p.type = Array;
            for (uint64_t k = 0; k < n && ok; ++k) {
                PList v;
                ok = object(uint(at + size_t(k) * size_t(refSize), refSize), v);
                p.array.push_back(std::move(v));
            }
            break;
        }
        case 0xD: {
            const uint64_t n = length();
            p.type = Dict;
            for (uint64_t k = 0; k < n && ok; ++k) {
                PList key, v;
                ok = object(uint(at + size_t(k) * size_t(refSize), refSize), key) &&
                     object(uint(at + size_t(n + k) * size_t(refSize), refSize), v);
                p.dict.emplace_back(key.s, std::move(v));
            }
            break;
        }
        default:
            ok = false;   // dates, UIDs, sets: not in the Finder's settings
        }
        --depth;
        return ok;
    };
    return object(top, out);
}

std::string PList::serialize() const
{
    // every object written once (no sharing), references and offsets in the sizes they need
    std::vector<std::string> objects;
    std::vector<std::vector<size_t>> refs;   // per object: its references, filled after
    std::function<size_t(const PList &)> add = [&](const PList &p) -> size_t {
        const size_t id = objects.size();
        objects.emplace_back();
        refs.emplace_back();
        auto marker = [](int hi, size_t n) {
            std::string m;
            if (n < 15) {
                m += char(hi << 4 | int(n));
            } else {
                m += char(hi << 4 | 0xF);
                if (n < 256) {
                    m += char(0x10);
                    m += char(n);
                } else {
                    m += char(0x11);
                    m += char(n >> 8);
                    m += char(n & 0xFF);
                }
            }
            return m;
        };
        std::string o;
        switch (p.type) {
        case Null: o = std::string(1, '\0'); break;
        case Bool: o = std::string(1, char(p.b ? 0x09 : 0x08)); break;
        case Integer: {
            const uint64_t v = uint64_t(p.i);
            if (p.i >= 0 && v < 256) {
                o = {char(0x10), char(v)};
            } else if (p.i >= 0 && v < 65536) {
                o = {char(0x11), char(v >> 8), char(v)};
            } else if (p.i >= 0 && v < (1ull << 32)) {
                o = {char(0x12)};
                DSStore::put32(o, uint32_t(v));
            } else {
                o = {char(0x13)};
                DSStore::put64(o, v);
            }
            break;
        }
        case Real: {
            uint64_t bits;
            std::memcpy(&bits, &p.r, 8);
            o = {char(0x23)};
            DSStore::put64(o, bits);
            break;
        }
        case Data: o = marker(0x4, p.s.size()) + p.s; break;
        case String: {
            const bool ascii = std::all_of(p.s.begin(), p.s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
            if (ascii) {
                o = marker(0x5, p.s.size()) + p.s;
            } else {
                const std::u16string u = DSStore::toUtf16(p.s);
                o = marker(0x6, u.size());
                DSStore::putUtf16(o, u);
            }
            break;
        }
        case Array: {
            o = marker(0xA, p.array.size());
            std::vector<size_t> r;
            for (const PList &v : p.array)
                r.push_back(add(v));
            refs[id] = r;
            break;
        }
        case Dict: {
            o = marker(0xD, p.dict.size());
            std::vector<size_t> keys, values;
            for (const auto &kv : p.dict)
                keys.push_back(add(PList::string(kv.first)));
            for (const auto &kv : p.dict)
                values.push_back(add(kv.second));
            keys.insert(keys.end(), values.begin(), values.end());
            refs[id] = keys;
            break;
        }
        }
        objects[id] = o;
        return id;
    };
    add(*this);
    const int refSize = objects.size() < 256 ? 1 : 2;
    std::string out = "bplist00";
    std::vector<uint64_t> offsets;
    for (size_t k = 0; k < objects.size(); ++k) {
        offsets.push_back(out.size());
        out += objects[k];
        for (size_t r : refs[k]) {
            if (refSize == 2)
                out += char(r >> 8);
            out += char(r & 0xFF);
        }
    }
    const uint64_t table = out.size();
    const int offSize = table < 256 ? 1 : table < 65536 ? 2 : 4;
    for (uint64_t off : offsets)
        for (int k = offSize - 1; k >= 0; --k)
            out += char((off >> (8 * k)) & 0xFF);
    out += std::string(6, '\0');
    out += char(offSize);
    out += char(refSize);
    DSStore::put64(out, objects.size());
    DSStore::put64(out, 0);
    DSStore::put64(out, table);
    return out;
}

} // namespace Finder
