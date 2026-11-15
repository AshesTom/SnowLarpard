// SPDX-License-Identifier: LGPL-2.1-or-later
// The old NeXT text property lists (OpenStep format) Preview's CommonToolbar.proplist is written in:
// { key = value; }, ( a, b ), "quoted" or bare strings, /* */ and // comments.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace Preview {

struct PList {
    enum Type { Null, String, Array, Dict } type = Null;
    std::string string;
    std::vector<PList> array;
    std::map<std::string, PList> dict;

    const PList &operator[](const std::string &key) const;
    std::string str(const std::string &key, const std::string &fallback = {}) const;
    int integer(const std::string &key, int fallback = 0) const;
    bool isNull() const { return type == Null; }

    static PList parse(const std::string &text);
    static PList load(const std::string &file);
};

} // namespace Preview
