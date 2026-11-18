// SPDX-License-Identifier: LGPL-2.1-or-later
#include "screencapture.h"

#include "foundation.h"
#include "launchservices.h"
#include "runloop.h"
#include "windows.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace AK;

namespace ScreenCapture {

namespace {

const std::string Bundle = "/System/Library/CoreServices/SystemUIServer.app";

std::string replaceAll(std::string s, const std::string &from, const std::string &to)
{
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
        s.replace(at, from.size(), to);
    return s;
}

// -[NSString stringWithFormat:] for %@ and %n$@
std::string format(std::string f, const std::vector<std::string> &args)
{
    for (size_t i = 0; i < args.size(); ++i)
        f = replaceAll(f, "%" + std::to_string(i + 1) + "$@", args[i]);
    size_t next = 0;
    for (size_t at = 0; (at = f.find("%@", at)) != std::string::npos && next < args.size();) {
        f.replace(at, 2, args[next]);
        at += args[next++].size();
    }
    return f;
}

// FUN_100028af4: the type, png by default
std::string type()
{
    const std::string t = Settings("screencapture").string("type");
    return t.empty() ? "png" : t;
}

// FUN_100028d71: `location` when it is a folder, else the Desktop (else /tmp), with its '/'
std::string folder()
{
    std::string dir = Settings("screencapture").string("location");
    if (!dir.empty() && dir[0] == '~')
        dir = home() + dir.substr(1);
    if (dir.empty() || !isDir(dir))
        dir = isDir(desktopDir()) ? desktopDir() : "/tmp";
    if (dir.back() != '/')
        dir += '/';
    return dir;
}

// FUN_100028b2e: "Screen shot 2009-08-28 at 10.13.52 AM"
std::string baseName()
{
    Settings prefs("screencapture");
    std::string name = prefs.string("name");
    if (name.empty())
        name = Assets::tr(Bundle, "ScreenCapture", "Screen shot", "Screen shot");
    if (!prefs.boolean("include-date", true))
        return name;
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char date[32], time[64];
    std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
    // the medium time style: the locale's (7:05:32 PM, 19:05:32), the hour without a leading zero
    std::strftime(time, sizeof time, "%X", &tm);
    std::string clock = time;
    if (clock.size() > 1 && clock[0] == '0' && (clock.find("AM") != std::string::npos || clock.find("PM") != std::string::npos))
        clock.erase(0, 1);
    std::string s = format(Assets::tr(Bundle, "Localizable", "%@ %@ at %@", "%1$@ %2$@ at %3$@"), {name, date, clock});
    s = replaceAll(s, ":", Assets::tr(Bundle, "ScreenCapture", ".", "."));
    s = replaceAll(s, "/", Assets::tr(Bundle, "ScreenCapture", "-", "-"));
    return s;
}

// FUN_10002907d's file: the name, or "name (n)" while one is there
std::string fileName()
{
    const std::string dir = folder(), base = baseName(), ext = type();
    std::string path = dir + base + "." + ext;
    for (int n = 1; isFile(path) || isDir(path); ++n)
        path = dir + format(Assets::tr(Bundle, "ScreenCapture", "%@ (%@)", "%@ (%@)"), {base, std::to_string(n)}) + "." + ext;
    return path;
}

// FUN_100028f2d: /usr/sbin/screencapture, the shell's (apps/screencapture)
void run(std::vector<std::string> args)
{
    args.insert(args.begin(), LaunchServices::appsDir() + "/screencapture");
    if (!Task::launchDetached(args))
        std::fprintf(stderr, "SystemUIServer: screencapture failed\n");
}

void hotKey(int which)
{
    Settings prefs("screencapture");
    const bool cursor = prefs.boolean("capture_cursor"), noShadow = prefs.boolean("disable-shadow");
    const std::string t = "-t" + type();
    switch (which) {
    case 28:   // 200
        run({cursor ? "-fdC" : "-fd", t, fileName()});
        break;
    case 29:   // 201
        run({cursor ? "-cdC" : "-cd", t});
        break;
    case 30:   // 300
        run({noShadow ? "-ido" : "-id", t, fileName()});
        break;
    case 31:   // 301
        run({noShadow ? "-idco" : "-idc", t});
        break;
    default:
        std::fprintf(stderr, "unknown hot key: %d\n", which);
    }
}

} // namespace

void install()
{
    Windows::registerSymbolicHotKeys({28, 29, 30, 31}, hotKey);
}

} // namespace ScreenCapture
