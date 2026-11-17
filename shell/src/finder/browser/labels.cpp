// SPDX-License-Identifier: LGPL-2.1-or-later
#include "labels.h"

#include "foundation.h"

#include <sys/xattr.h>

#include <cerrno>
#include <cstring>
#include <map>

namespace Finder {

namespace Labels {

namespace {

const char *const Attribute = "user.com.apple.FinderInfo";

// HIServices' defaults (_sCachedColors, 0x57a60): RGBColor, 16 bits a channel
const unsigned short Defaults[8][3] = {{0, 0, 0},          {0xa800, 0xa800, 0xa800}, {0xb300, 0xd600, 0x4400}, {0xbf00, 0x8c00, 0xd800},
                                       {0x5700, 0xa100, 0xff00}, {0xef00, 0xda00, 0x4400}, {0xfb00, 0x6200, 0x5900}, {0xf600, 0xa900, 0x4100}};
const char *const Names[8] = {"None", "Gray", "Green", "Purple", "Blue", "Yellow", "Red", "Orange"};   // Labels.strings

std::map<std::string, int> &cache()
{
    static std::map<std::string, int> c;
    return c;
}

} // namespace

int get(const std::string &path)
{
    auto it = cache().find(path);
    if (it != cache().end())
        return it->second;
    unsigned char info[32] = {};
    const ssize_t n = lgetxattr(path.c_str(), Attribute, info, sizeof info);   // an alias's own, not its original's
    const int label = n >= 10 ? ((info[9] >> 1) & 7) : 0;   // fdFlags' low byte: bits 1-3
    if (cache().size() > 20000)
        cache().clear();
    cache()[path] = label;
    return label;
}

bool set(const std::string &path, int index)
{
    unsigned char info[32] = {};
    const ssize_t n = getxattr(path.c_str(), Attribute, info, sizeof info);
    if (n > 0 && n < 32)
        std::memset(info + n, 0, size_t(32 - n));
    info[9] = static_cast<unsigned char>((info[9] & ~0x0E) | ((index & 7) << 1));
    bool empty = true;
    for (unsigned char c : info)
        empty = empty && c == 0;
    const int r = empty ? removexattr(path.c_str(), Attribute) : setxattr(path.c_str(), Attribute, info, sizeof info, 0);
    cache()[path] = index & 7;
    return r == 0 || (empty && errno == ENODATA);
}

void forget(const std::string &path) { cache().erase(path); }

namespace {

struct Table {
    AK::Color colors[8];
    std::string names[8];
};

Table &table()
{
    // read once: the user's own (finder.conf, as com.apple.Labels), else HIServices'
    static Table t;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        AK::Settings conf("finder");
        for (int i = 0; i < 8; ++i) {
            const std::string own = conf.string("Label_Color_" + std::to_string(i), "");
            t.colors[i] = !own.empty() ? AK::Color::parse(own)
                                       : AK::Color{Defaults[i][0] / 65535.0, Defaults[i][1] / 65535.0, Defaults[i][2] / 65535.0, 1};
            t.names[i] = conf.string("Label_Name_" + std::to_string(i), Names[i]);
        }
    }
    return t;
}

} // namespace

AK::Color color(int index) { return table().colors[index & 7]; }

std::string name(int index) { return table().names[index & 7]; }

void setName(int index, const std::string &name)
{
    AK::Settings conf("finder");
    conf.set("Label_Name_" + std::to_string(index & 7), name);
    table().names[index & 7] = name;
}

} // namespace Labels

} // namespace Finder
