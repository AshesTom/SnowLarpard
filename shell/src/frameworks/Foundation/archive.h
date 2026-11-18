// SPDX-License-Identifier: LGPL-2.1-or-later
// Binary property lists (bplist00) and what NSKeyedArchiver writes in them: the compiled nibs of
// 10.6 (keyedobjects.nib) are keyed archives of the interface's objects.
//
//   bplist00, objects, an offset table, a 32 byte trailer: offset size, reference size, object count,
//   top object, offset table position (big endian)
//   markers: 0x0- null/bool, 0x1n int (2^n bytes), 0x2n real, 0x33 date, 0x4n data, 0x5n ASCII,
//   0x6n UTF-16BE, 0x8n UID, 0xAn array, 0xDn dictionary (n = 15: the count follows as an int)
//   keyed archive: {$archiver: NSKeyedArchiver, $top: {...}, $objects: [$null, ...]}; an object is a
//   dictionary whose values point to others by UID, its class {$classname, $classes} by $class
#pragma once

#include <string>
#include <vector>

namespace AK {

class Archive {
public:
    bool load(const std::string &path);
    // An NSArchiver typedstream (the older nibs' objects.nib), made into the same model as a keyed
    // archive's: each object a dictionary with the keys NSKeyedArchiver gives it (typedstream.cpp)
    bool loadTypedStream(const std::string &path);
    bool isNull() const { return m_objects < 0; }

    // An archived object: its index in $objects; -1 for none
    int top(const char *key) const;
    std::string className(int obj) const;
    bool isA(int obj, const char *cls) const;   // the class or one of its superclasses
    int object(int obj, const char *key) const;   // a key's value, followed through its UID
    bool has(int obj, const char *key) const;
    long long integer(int obj, const char *key, long long fallback = 0) const;
    double real(int obj, const char *key, double fallback = 0) const;
    bool boolean(int obj, const char *key, bool fallback = false) const;
    std::string string(int obj, const char *key) const;   // an NSString (or a raw string) by key
    std::string stringOf(int obj) const;                  // the object itself as a string
    std::vector<int> array(int obj, const char *key) const;   // an NSArray / NSSet's objects
    std::vector<int> arrayOf(int obj) const;
    std::string data(int obj, const char *key) const;

private:
    friend class TypedStreamTranslator;
    struct Value {
        enum Type { Null, Bool, Int, Real, Data, String, Uid, Array, Dict } type = Null;
        long long i = 0;
        double r = 0;
        std::string s;                 // String (UTF-8), Data
        std::vector<int> items, keys;  // Array: items; Dict: keys and items
    };
    int field(int value, const char *key) const;   // a plist dictionary's value index
    int follow(int value) const;                   // a UID to the archived object's value
    int parse(int index);
    std::string m_bytes;
    std::vector<Value> m_values;
    std::vector<size_t> m_offsets;
    int m_refSize = 1, m_root = -1, m_objects = -1;
};

} // namespace AK
