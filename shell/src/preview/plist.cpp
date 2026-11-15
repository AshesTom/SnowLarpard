// SPDX-License-Identifier: LGPL-2.1-or-later
#include "plist.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Preview {

namespace {

struct Parser {
    const std::string &s;
    size_t i = 0;

    void skip()
    {
        while (i < s.size()) {
            if (std::isspace((unsigned char)s[i])) {
                ++i;
            } else if (s.compare(i, 2, "/*") == 0) {
                const size_t end = s.find("*/", i + 2);
                i = end == std::string::npos ? s.size() : end + 2;
            } else if (s.compare(i, 2, "//") == 0) {
                const size_t end = s.find('\n', i);
                i = end == std::string::npos ? s.size() : end + 1;
            } else {
                break;
            }
        }
    }

    std::string word()
    {
        skip();
        std::string out;
        if (i < s.size() && s[i] == '"') {
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char c = s[++i];
                    out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
                } else {
                    out += s[i];
                }
            }
            ++i;
            return out;
        }
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || std::strchr("_.$:/-+", s[i])))
            out += s[i++];
        return out;
    }

    PList value()
    {
        skip();
        PList v;
        if (i >= s.size())
            return v;
        if (s[i] == '{') {
            ++i;
            v.type = PList::Dict;
            for (;;) {
                skip();
                if (i >= s.size() || s[i] == '}') {
                    ++i;
                    break;
                }
                const std::string key = word();
                skip();
                if (i < s.size() && s[i] == '=')
                    ++i;
                v.dict[key] = value();
                skip();
                if (i < s.size() && s[i] == ';')
                    ++i;
                if (key.empty() && i < s.size() && s[i] != '}')
                    ++i;   // something unreadable: on
            }
        } else if (s[i] == '(') {
            ++i;
            v.type = PList::Array;
            for (;;) {
                skip();
                if (i >= s.size() || s[i] == ')') {
                    ++i;
                    break;
                }
                v.array.push_back(value());
                skip();
                if (i < s.size() && s[i] == ',')
                    ++i;
            }
        } else {
            v.type = PList::String;
            v.string = word();
        }
        return v;
    }
};

} // namespace

const PList &PList::operator[](const std::string &key) const
{
    static const PList none;
    auto it = dict.find(key);
    return it == dict.end() ? none : it->second;
}

std::string PList::str(const std::string &key, const std::string &fallback) const
{
    const PList &v = (*this)[key];
    return v.type == String ? v.string : fallback;
}

int PList::integer(const std::string &key, int fallback) const
{
    const PList &v = (*this)[key];
    return v.type == String && !v.string.empty() ? std::atoi(v.string.c_str()) : fallback;
}

PList PList::parse(const std::string &text)
{
    Parser p{text};
    return p.value();
}

PList PList::load(const std::string &file)
{
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str());
}

} // namespace Preview
