// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's file operations (DesktopServicesPriv's TOperation: TCopyMoveOperation, TCopyWriter,
// TTrashOperation, TDeleteImmediatelyOperation), each on a thread of its own: copy, move (a rename on
// the same disk, else a copy then a delete), duplicate (TUniqueNamer's "name copy", "name copy 2"),
// move to the Trash (the freedesktop Trash, the Dock's and the desktop's: files/ and info/*.trashinfo)
// and Empty Trash. The progress window (progress.h) reads their state; a name already taken stops the
// thread until it is told what to do (Replace, Don't Replace, Stop; Apply to All).
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Finder {

class Operation {
public:
    enum Kind { Copy, Move, Duplicate, Trash, EmptyTrash, PutBack, Compress, SecureEmptyTrash };
    enum Answer { None, Replace, Skip, Stop };

    // sources: paths; destination: a folder (Duplicate: their own; Trash, EmptyTrash: none).
    // PutBack: items of the Trash to where they were (their .trashinfo); Compress: the items into
    // destination's "<name>.zip" or "Archive.zip" (Archive Utility's names, AR40), by zip(1);
    // SecureEmptyTrash: each file written over before it goes
    static std::shared_ptr<Operation> start(Kind kind, std::vector<std::string> sources, std::string destination = {});

    Kind kind() const { return m_kind; }
    const std::vector<std::string> &sources() const { return m_sources; }
    const std::string &destination() const { return m_destination; }
    std::vector<std::string> results() const;   // the new items' paths (Duplicate, Copy, Move, Trash, PutBack, Compress)
    // where an item of the Trash came from (its .trashinfo's Path), empty when unknown
    static std::string trashOrigin(const std::string &trashed);
    // a free name in the folder: "name 2", "name 3"... before the extension (TUniqueNamer)
    static std::string freeName(const std::string &folder, const std::string &name, bool isFolder);

    // the progress (its thread writes, the window reads)
    std::atomic<long long> bytesDone{0}, bytesTotal{0};
    std::atomic<int> itemsLeft{0};
    std::atomic<bool> counting{true}, finished{false}, cancelled{false};
    std::string current() const;   // the item being copied
    std::string error() const;     // why it stopped, if it did
    double started = 0;            // AK::now() when it started

    // a conflict: the name, whether the one there is newer or older (0 the same age), and the
    // answer it waits for
    struct Conflict {
        std::string name, extension;
        int age = 0;   // -1 the one there is older, 1 newer
        bool folder = false;
    };
    bool conflict(Conflict &out) const;
    void answer(Answer a, bool applyToAll);
    void cancel();

    // the Trash: its files/ folder
    static std::string trashFiles();
    // "name copy", "name copy 2" (the Finder's N4, N1; before the extension of a file)
    static std::string uniqueCopyName(const std::string &folder, const std::string &name, bool isFolder);

private:
    Operation(Kind kind, std::vector<std::string> sources, std::string destination);
    void run();
    void count(const std::string &path);
    bool copyItem(const std::string &from, const std::string &to);
    bool copyFile(const std::string &from, const std::string &to);
    bool moveItem(const std::string &from, const std::string &to);
    bool removeItem(const std::string &path);
    bool trashItem(const std::string &path);
    void overwrite(const std::string &path);
    Answer ask(const std::string &target, const std::string &source);
    void fail(const std::string &message);

    Kind m_kind;
    std::vector<std::string> m_sources;
    std::string m_destination;
    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    std::string m_current, m_error;
    std::vector<std::string> m_results;
    bool m_conflictPending = false;
    Conflict m_conflict;
    Answer m_answer = None, m_always = None;
    std::thread m_thread;
};

} // namespace Finder
