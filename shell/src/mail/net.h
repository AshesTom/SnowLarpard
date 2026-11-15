// SPDX-License-Identifier: LGPL-2.1-or-later
// Mail's network, with libcurl: IMAP to list mailboxes, fetch new messages and change flags, SMTP to
// send. The calls block: the Worker runs them on a thread of its own and hands the results back to
// AquaKit's run loop (a timer drains them), so the windows never wait.
#pragma once

#include "store.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Mail {

struct Result {
    bool ok = false;
    std::string error;
};

// IMAP (blocking)
struct ListedMailbox {
    std::string name, flags;
};
Result listMailboxes(const Account &a, const std::string &pw, std::vector<ListedMailbox> *out);
// Every message's UID and flags in a mailbox
Result uidsAndFlags(const Account &a, const std::string &pw, const std::string &mailbox,
                    std::vector<std::pair<unsigned, unsigned>> *out);
Result fetchMessage(const Account &a, const std::string &pw, const std::string &mailbox, unsigned uid, std::string *raw);
Result storeFlags(const Account &a, const std::string &pw, const std::string &mailbox, unsigned uid, bool add,
                  const std::string &imapFlags);
Result moveMessage(const Account &a, const std::string &pw, const std::string &mailbox, unsigned uid, const std::string &to);
Result appendMessage(const Account &a, const std::string &pw, const std::string &mailbox, const std::string &raw,
                     const std::string &imapFlags);
// SMTP (blocking)
Result sendMessage(const Account &a, const std::string &pw, const std::string &from, const std::vector<std::string> &rcpts,
                   const std::string &raw);
// Checks an account's servers as the assistant does before it saves it
Result checkIncoming(const Account &a, const std::string &pw);

// "user@host": the account's guessed servers (the assistant's simplified setup)
Account guessAccount(const std::string &fullName, const std::string &email);

class Worker {
public:
    Worker();
    ~Worker();
    // `job` runs on the worker's thread, `done` on the run loop's after it
    void run(std::function<void()> job, std::function<void()> done = {});
    bool busy() const;

private:
    void loop();
    void drain();
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<std::function<void()>, std::function<void()>>> m_jobs;
    std::vector<std::function<void()>> m_done;
    int m_running = 0;
    bool m_stop = false;
    int m_timer = 0;
    std::thread m_thread;
};

} // namespace Mail
