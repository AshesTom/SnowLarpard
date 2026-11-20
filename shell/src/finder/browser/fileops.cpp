// SPDX-License-Identifier: LGPL-2.1-or-later
#include "fileops.h"

#include "appkit.h"
#include "foundation.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "Localizable", key, fallback); }

std::string format(std::string f, const std::vector<std::string> &values)
{
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string mark = "^" + std::to_string(i);
        for (size_t at; (at = f.find(mark)) != std::string::npos;)
            f.replace(at, mark.size(), values[i]);
    }
    return f;
}

std::string join(const std::string &dir, const std::string &name) { return dir == "/" ? "/" + name : dir + "/" + name; }

bool exists(const std::string &p)
{
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

// a name's stem and extension (a folder's whole name is its stem)
void split(const std::string &name, bool isFolder, std::string &stem, std::string &ext)
{
    const size_t dot = name.rfind('.');
    if (isFolder || dot == std::string::npos || dot == 0) {
        stem = name;
        ext.clear();
    } else {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
}

// "name 2", "name 3"... (the Trash's own names, TUniqueNamer without "copy")
std::string uniqueName(const std::string &folder, const std::string &name, bool isFolder)
{
    if (!exists(join(folder, name)))
        return name;
    std::string stem, ext;
    split(name, isFolder, stem, ext);
    for (int n = 2;; ++n) {
        const std::string candidate = format(tr("N1", "^0 ^1"), {stem, std::to_string(n)}) + ext;
        if (!exists(join(folder, candidate)))
            return candidate;
    }
}

// the freedesktop Trash's info: where it came from, when
std::string percentEncoded(const std::string &path)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || std::strchr("/-_.~", c)) {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

} // namespace

std::shared_ptr<Operation> Operation::start(Kind kind, std::vector<std::string> sources, std::string destination)
{
    std::shared_ptr<Operation> op(new Operation(kind, std::move(sources), std::move(destination)));
    op->started = now();
    op->m_thread = std::thread([op] { op->run(); });
    op->m_thread.detach();
    return op;
}

Operation::Operation(Kind kind, std::vector<std::string> sources, std::string destination)
    : m_kind(kind), m_sources(std::move(sources)), m_destination(std::move(destination))
{
}

std::string Operation::trashFiles()
{
    const char *data = std::getenv("XDG_DATA_HOME");
    return (data && *data ? std::string(data) : home() + "/.local/share") + "/Trash/files";
}

std::string Operation::uniqueCopyName(const std::string &folder, const std::string &name, bool isFolder)
{
    // the Finder's (0x100175b42): "^0 copy" (N4) on the stem, then "^0 ^1" (N1) with a number; the
    // extension put back
    std::string stem, ext;
    split(name, isFolder, stem, ext);
    // a copy's copy: "notes copy 2", not "notes copy copy" (an ending "copy" or "copy N" taken off)
    const std::string word = format(tr("N4", "^0 copy"), {""});   // " copy"
    const size_t at = stem.rfind(word);
    if (at != std::string::npos && at > 0) {
        const std::string rest = stem.substr(at + word.size());
        if (rest.empty() || (rest[0] == ' ' && rest.size() > 1 && std::all_of(rest.begin() + 1, rest.end(), ::isdigit)))
            stem = stem.substr(0, at);
    }
    const std::string copy = format(tr("N4", "^0 copy"), {stem});
    if (!exists(join(folder, copy + ext)))
        return copy + ext;
    for (int n = 2;; ++n) {
        const std::string candidate = format(tr("N1", "^0 ^1"), {copy, std::to_string(n)}) + ext;
        if (!exists(join(folder, candidate)))
            return candidate;
    }
}

std::vector<std::string> Operation::results() const
{
    std::lock_guard<std::mutex> l(m_lock);
    return m_results;
}

std::string Operation::current() const
{
    std::lock_guard<std::mutex> l(m_lock);
    return m_current;
}

std::string Operation::error() const
{
    std::lock_guard<std::mutex> l(m_lock);
    return m_error;
}

bool Operation::conflict(Conflict &out) const
{
    std::lock_guard<std::mutex> l(m_lock);
    if (!m_conflictPending)
        return false;
    out = m_conflict;
    return true;
}

void Operation::answer(Answer a, bool applyToAll)
{
    std::lock_guard<std::mutex> l(m_lock);
    m_answer = a;
    if (applyToAll && a != Stop)
        m_always = a;
    m_conflictPending = false;
    m_wake.notify_all();
}

void Operation::cancel()
{
    cancelled = true;
    std::lock_guard<std::mutex> l(m_lock);
    m_answer = Stop;
    m_conflictPending = false;
    m_wake.notify_all();
}

void Operation::fail(const std::string &message)
{
    std::lock_guard<std::mutex> l(m_lock);
    if (m_error.empty())
        m_error = message;
}

Operation::Answer Operation::ask(const std::string &target, const std::string &source)
{
    // a name taken: the window asks (PE1, PE61, PE63...), this thread waits
    std::unique_lock<std::mutex> l(m_lock);
    if (m_always != None)
        return m_always;
    struct stat there {}, here {};
    lstat(target.c_str(), &there);
    lstat(source.c_str(), &here);
    m_conflict = {};
    std::string stem, ext;
    m_conflict.folder = S_ISDIR(there.st_mode);
    split(fileName(target), m_conflict.folder, stem, ext);
    m_conflict.name = fileName(target);
    m_conflict.extension = ext.empty() ? std::string() : ext.substr(1);
    m_conflict.age = there.st_mtime < here.st_mtime ? -1 : there.st_mtime > here.st_mtime ? 1 : 0;
    m_answer = None;
    m_conflictPending = true;
    m_wake.wait(l, [this] { return !m_conflictPending; });
    return m_answer;
}

void Operation::count(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return;
    ++itemsLeft;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(path.c_str())) {
            while (dirent *e = readdir(d))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    count(join(path, e->d_name));
            closedir(d);
        }
    } else if (S_ISREG(st.st_mode)) {
        bytesTotal += st.st_size;
    }
}

bool Operation::copyFile(const std::string &from, const std::string &to)
{
    const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return false;
    struct stat st;
    fstat(in, &st);
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        close(in);
        return false;
    }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (ssize_t n; (n = read(in, buf.data(), buf.size())) > 0;) {
        if (cancelled) {
            ok = false;
            break;
        }
        for (ssize_t w = 0; w < n;) {
            const ssize_t k = write(out, buf.data() + w, size_t(n - w));
            if (k < 0) {
                ok = false;
                break;
            }
            w += k;
        }
        if (!ok)
            break;
        bytesDone += n;
    }
    // the extended attributes (the FinderInfo, its label) and the dates kept, as the Finder keeps them
    char names[4096];
    const ssize_t len = flistxattr(in, names, sizeof names);
    for (ssize_t at = 0; at < len; at += ssize_t(std::strlen(names + at)) + 1) {
        char value[65536];
        const ssize_t vn = fgetxattr(in, names + at, value, sizeof value);
        if (vn >= 0)
            fsetxattr(out, names + at, value, size_t(vn), 0);
    }
    const struct timespec times[2] = {st.st_atim, st.st_mtim};
    futimens(out, times);
    close(in);
    close(out);
    if (!ok)
        unlink(to.c_str());
    return ok;
}

bool Operation::copyItem(const std::string &from, const std::string &to)
{
    if (cancelled)
        return false;
    {
        std::lock_guard<std::mutex> l(m_lock);
        m_current = fileName(from);
    }
    struct stat st;
    if (lstat(from.c_str(), &st) != 0)
        return false;
    bool ok = true;
    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        const ssize_t n = readlink(from.c_str(), target, sizeof target - 1);
        ok = n >= 0 && symlink(std::string(target, size_t(n)).c_str(), to.c_str()) == 0;
    } else if (S_ISDIR(st.st_mode)) {
        ok = mkdir(to.c_str(), (st.st_mode & 07777) | S_IWUSR | S_IXUSR) == 0 || errno == EEXIST;
        if (DIR *d = opendir(from.c_str())) {
            while (dirent *e = readdir(d)) {
                if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
                    continue;
                ok = copyItem(join(from, e->d_name), join(to, e->d_name)) && ok;
                if (cancelled)
                    break;
            }
            closedir(d);
        }
        chmod(to.c_str(), st.st_mode & 07777);
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        utimensat(AT_FDCWD, to.c_str(), times, 0);
    } else if (S_ISREG(st.st_mode)) {
        ok = copyFile(from, to);
    }
    --itemsLeft;
    return ok;
}

bool Operation::removeItem(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return true;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(path.c_str())) {
            while (dirent *e = readdir(d))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    removeItem(join(path, e->d_name));
            closedir(d);
        }
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

bool Operation::moveItem(const std::string &from, const std::string &to)
{
    // the same disk: a rename; another: a copy, then the original removed
    if (std::rename(from.c_str(), to.c_str()) == 0) {
        struct stat st;
        if (lstat(to.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            bytesDone += st.st_size;
        return true;
    }
    if (errno != EXDEV)
        return false;
    return copyItem(from, to) && !cancelled && removeItem(from);
}

// Secure Empty Trash: every file under the path written over with zeros, then synced, before it goes
void Operation::overwrite(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(path.c_str())) {
            std::vector<std::string> names;
            while (dirent *e = readdir(d))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    names.push_back(e->d_name);
            closedir(d);
            for (const std::string &n : names)
                overwrite(join(path, n));
        }
        return;
    }
    if (!S_ISREG(st.st_mode))
        return;
    const int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0)
        return;
    static const char zeros[65536] = {};
    for (off_t left = st.st_size; left > 0 && !cancelled;) {
        const ssize_t n = write(fd, zeros, size_t(std::min<off_t>(left, sizeof zeros)));
        if (n <= 0)
            break;
        left -= n;
    }
    fsync(fd);
    close(fd);
}

bool Operation::trashItem(const std::string &path)
{
    // files/ the item under a name of its own there, info/<name>.trashinfo where it came from
    const std::string files = trashFiles(), info = dirName(files) + "/info";
    makePath(files);
    makePath(info);
    struct stat st;
    lstat(path.c_str(), &st);
    std::string name = uniqueName(files, fileName(path), S_ISDIR(st.st_mode));
    while (exists(info + "/" + name + ".trashinfo"))
        name = uniqueName(files, name + " ", S_ISDIR(st.st_mode));
    char date[32];
    const time_t t = std::time(nullptr);
    struct tm tm {};
    localtime_r(&t, &tm);
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
    {
        std::ofstream out(info + "/" + name + ".trashinfo");
        out << "[Trash Info]\nPath=" << percentEncoded(path) << "\nDeletionDate=" << date << "\n";
    }
    if (moveItem(path, join(files, name))) {
        std::lock_guard<std::mutex> l(m_lock);
        m_results.push_back(join(files, name));
        return true;
    }
    std::remove((info + "/" + name + ".trashinfo").c_str());
    return false;
}

std::string Operation::freeName(const std::string &folder, const std::string &name, bool isFolder) { return uniqueName(folder, name, isFolder); }

std::string Operation::trashOrigin(const std::string &trashed)
{
    const std::string info = dirName(trashFiles()) + "/info/" + fileName(trashed) + ".trashinfo";
    std::ifstream in(info);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("Path=", 0) == 0) {
            // percent-decoded
            std::string p, enc = line.substr(5);
            for (size_t i = 0; i < enc.size(); ++i)
                if (enc[i] == '%' && i + 2 < enc.size()) {
                    p += char(std::stoi(enc.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                } else {
                    p += enc[i];
                }
            if (!p.empty() && p[0] != '/')
                p = dirName(dirName(dirName(trashFiles()))) + "/" + p;   // relative to the Trash's volume
            return p;
        }
    return {};
}

void Operation::run()
{
    if (m_kind == PutBack || m_kind == Compress)
        itemsLeft = int(m_sources.size());
    else if (m_kind != Trash && m_kind != Move)
        for (const std::string &s : m_sources)
            count(s);
    else
        itemsLeft = int(m_sources.size());
    if (m_kind == EmptyTrash) {
        // every item in the Trash, folders too (the items left counts the top ones)
        if (DIR *d = opendir(trashFiles().c_str())) {
            while (dirent *e = readdir(d))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    ++itemsLeft;
            closedir(d);
        }
    }
    counting = false;

    switch (m_kind) {
    case Copy:
    case Move:
        for (const std::string &src : m_sources) {
            if (cancelled)
                break;
            std::string to = join(m_destination, fileName(src));
            if (to == src)
                continue;   // already there
            if ((m_destination + "/").rfind(src + "/", 0) == 0) {
                fail(format(tr("PE2", "The item “^0” can’t be copied into itself."), {fileName(src)}));
                break;
            }
            if (exists(to)) {
                const Answer a = ask(to, src);
                if (a == Stop) {
                    cancelled = true;
                    break;
                }
                if (a == Skip) {
                    --itemsLeft;
                    continue;
                }
                removeItem(to);
            }
            const bool ok = m_kind == Copy ? copyItem(src, to) : moveItem(src, to);
            if (m_kind == Move)
                --itemsLeft;
            if (!ok && !cancelled)
                fail(format(tr("PE5", "The operation can’t be completed because an unexpected error occurred (error code ^0)."),
                            {std::to_string(-errno)}));
            if (ok) {
                std::lock_guard<std::mutex> l(m_lock);
                m_results.push_back(to);
            }
        }
        break;
    case Duplicate:
        for (const std::string &src : m_sources) {
            if (cancelled)
                break;
            const std::string folder = dirName(src).empty() ? "/" : dirName(src);
            struct stat st;
            lstat(src.c_str(), &st);
            const std::string to = join(folder, uniqueCopyName(folder, fileName(src), S_ISDIR(st.st_mode)));
            if (copyItem(src, to)) {
                std::lock_guard<std::mutex> l(m_lock);
                m_results.push_back(to);
            }
        }
        break;
    case Trash:
        for (const std::string &src : m_sources) {
            if (cancelled)
                break;
            {
                std::lock_guard<std::mutex> l(m_lock);
                m_current = fileName(src);
            }
            if (!trashItem(src))
                fail(format(tr("MT2", "The item “^0” can’t be moved to the Trash because it can’t be deleted."), {fileName(src)}));
            --itemsLeft;
        }
        break;
    case PutBack:
        // each item to its .trashinfo's place (its folder made again if it went)
        for (const std::string &src : m_sources) {
            if (cancelled)
                break;
            {
                std::lock_guard<std::mutex> l(m_lock);
                m_current = fileName(src);
            }
            const std::string origin = trashOrigin(src);
            if (origin.empty()) {
                --itemsLeft;
                continue;
            }
            makePath(dirName(origin).empty() ? "/" : dirName(origin));
            std::string to = origin;
            if (exists(to)) {
                struct stat st;
                lstat(src.c_str(), &st);
                to = join(dirName(origin), uniqueName(dirName(origin), fileName(origin), S_ISDIR(st.st_mode)));
            }
            if (moveItem(src, to)) {
                std::remove((dirName(trashFiles()) + "/info/" + fileName(src) + ".trashinfo").c_str());
                std::lock_guard<std::mutex> l(m_lock);
                m_results.push_back(to);
            }
            --itemsLeft;
        }
        break;
    case Compress: {
        // Archive Utility's names: "<item>.zip" for one, "Archive.zip" for several (AR40), made free
        const std::string folder = m_destination;
        const std::string base = m_sources.size() == 1 ? fileName(m_sources.front()) : tr("AR40", "Archive");
        const std::string zip = join(folder, uniqueName(folder, base + ".zip", false));
        // zip(1), else libarchive's bsdtar writing a zip
        auto inPath = [](const char *prog) {
            const char *path = std::getenv("PATH");
            std::string all = path ? path : "/usr/bin:/bin";
            for (size_t start = 0; start <= all.size();) {
                const size_t end = all.find(':', start);
                const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!dir.empty() && access((dir + "/" + prog).c_str(), X_OK) == 0)
                    return true;
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return false;
        };
        std::vector<std::string> args = inPath("zip") ? std::vector<std::string>{"zip", "-r", "-y", "-q", zip}
                                                      : std::vector<std::string>{"bsdtar", "--format", "zip", "-cf", zip};
        for (const std::string &src : m_sources)
            args.push_back(fileName(src));
        {
            std::lock_guard<std::mutex> l(m_lock);
            m_current = base;
        }
        const pid_t pid = fork();
        if (pid == 0) {
            if (chdir(folder.c_str()) != 0)
                _exit(126);
            std::vector<char *> argv;
            for (std::string &a : args)
                argv.push_back(a.data());
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        int status = 0;
        if (pid > 0)
            waitpid(pid, &status, 0);
        if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            std::lock_guard<std::mutex> l(m_lock);
            m_results.push_back(zip);
        } else {
            std::remove(zip.c_str());
            fail(WIFEXITED(status) && WEXITSTATUS(status) == 127 ? tr("AR22", "A resource can’t be created for compressing.")
                                                                  : tr("AR24", "An error occurred while compressing a file."));
        }
        itemsLeft = 0;
        break;
    }
    case SecureEmptyTrash:
    case EmptyTrash: {
        const std::string files = trashFiles(), info = dirName(files) + "/info";
        DIR *d = opendir(files.c_str());
        std::vector<std::string> names;
        while (d)
            if (dirent *e = readdir(d)) {
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    names.push_back(e->d_name);
            } else {
                break;
            }
        if (d)
            closedir(d);
        for (const std::string &n : names) {
            if (cancelled)
                break;
            {
                std::lock_guard<std::mutex> l(m_lock);
                m_current = n;
            }
            struct stat st;
            if (lstat(join(files, n).c_str(), &st) == 0 && S_ISREG(st.st_mode))
                bytesDone += st.st_size;
            if (m_kind == SecureEmptyTrash)
                overwrite(join(files, n));
            removeItem(join(files, n));
            std::remove((info + "/" + n + ".trashinfo").c_str());
            --itemsLeft;
        }
        break;
    }
    }
    finished = true;
}

} // namespace Finder
