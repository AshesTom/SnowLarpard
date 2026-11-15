// SPDX-License-Identifier: LGPL-2.1-or-later
// .DS_Store, as DesktopServicesPriv keeps it (BuddyStorage, BTree; docs/reverse-status.md 13), read
// and written: a buddy allocator's file ("\0\0\0\1Bud1", the root block's address and size twice),
// its blocks' addresses (the offset, the size's log2 in the low 5 bits), a table of contents whose
// "DSDB" is the B-tree (its root node, levels, records, nodes, the page size 4096), 32 free lists;
// the tree's records sorted by file name (no case) then code: the name in UTF-16, a property code
// ('Iloc', 'vstl', 'bwsp'...), a type ('long', 'shor', 'bool', 'blob', 'type', 'ustr', 'comp', 'dutc')
// and its value. The properties the 10.6 Finder writes as binary plists (bwsp, icvp, lsvp) have
// their codec here too.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Finder {
namespace DSStore {

struct Record {
    std::string name;   // UTF-8 ("." the folder itself)
    std::string code;   // 4 characters
    std::string type;   // 4 characters
    int64_t number = 0;   // long, shor, bool, comp, dutc
    std::string data;     // blob (its bytes), type (4 characters), ustr (UTF-8)
};

// The records, sorted as the tree has them; false when there is no file or it isn't one
bool read(const std::string &path, std::vector<Record> &records);
// A new file with these records (sorted here), in one leaf or under one root when they don't fit
bool write(const std::string &path, std::vector<Record> records);

// A folder's .DS_Store held while the Finder works on it: the records by name and code
class Store {
public:
    explicit Store(const std::string &folder);
    const Record *get(const std::string &name, const std::string &code) const;
    void set(const Record &r);
    void remove(const std::string &name, const std::string &code);
    bool save();   // only when changed
    bool changed() const { return m_changed; }

private:
    std::string m_path;
    std::map<std::pair<std::string, std::string>, Record> m_records;
    bool m_changed = false;
};

Record blob(const std::string &name, const std::string &code, const std::string &bytes);
Record fourCC(const std::string &name, const std::string &code, const std::string &value);   // type

// Iloc: the icon's anchor (its centre), then ffffffffffff0000
bool iconLocation(const Record &r, int &x, int &y);
Record iconLocation(const std::string &name, int x, int y);

} // namespace DSStore

// A binary plist (bplist00), the types the Finder's view settings use
struct PList {
    enum Type { Null, Bool, Integer, Real, String, Data, Array, Dict } type = Null;
    bool b = false;
    int64_t i = 0;
    double r = 0;
    std::string s;   // String (UTF-8), Data
    std::vector<PList> array;
    std::vector<std::pair<std::string, PList>> dict;

    static PList boolean(bool v) { PList p; p.type = Bool; p.b = v; return p; }
    static PList integer(int64_t v) { PList p; p.type = Integer; p.i = v; return p; }
    static PList real(double v) { PList p; p.type = Real; p.r = v; return p; }
    static PList string(const std::string &v) { PList p; p.type = String; p.s = v; return p; }
    static PList dictionary() { PList p; p.type = Dict; return p; }

    const PList *get(const std::string &key) const;
    void set(const std::string &key, PList value);
    double number(double fallback = 0) const;   // Integer, Real, Bool

    static bool parse(const std::string &bytes, PList &out);
    std::string serialize() const;
};

} // namespace Finder
