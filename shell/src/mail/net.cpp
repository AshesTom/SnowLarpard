// SPDX-License-Identifier: LGPL-2.1-or-later
#include "net.h"

#include "appkit.h"
#include "mime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <sstream>

namespace Mail {

namespace {

size_t collect(char *data, size_t size, size_t n, void *out)
{
    static_cast<std::string *>(out)->append(data, size * n);
    return size * n;
}

struct Upload {
    const std::string *data;
    size_t pos = 0;
};

size_t feed(char *buf, size_t size, size_t n, void *in)
{
    auto *u = static_cast<Upload *>(in);
    const size_t len = std::min(size * n, u->data->size() - u->pos);
    std::memcpy(buf, u->data->data() + u->pos, len);
    u->pos += len;
    return len;
}

// An IMAP mailbox name in a URL: percent-encoded (curl quotes it for the server)
std::string escape(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            o += char(c);
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

std::string imapURL(const Account &a, const std::string &mailbox = {})
{
    std::string url = std::string(a.imapSSL ? "imaps://" : "imap://") + a.imapHost + ":" + std::to_string(a.imapPort) + "/";
    if (!mailbox.empty())
        url += escape(mailbox);
    return url;
}

class Session {
public:
    Session(const Account &a, const std::string &pw, bool smtp = false)
    {
        m_curl = curl_easy_init();
        const std::string &user = smtp ? (a.smtpUser.empty() ? a.imapUser : a.smtpUser) : a.imapUser;
        curl_easy_setopt(m_curl, CURLOPT_USERNAME, user.c_str());
        curl_easy_setopt(m_curl, CURLOPT_PASSWORD, pw.c_str());
        // STARTTLS when the server offers it on a plain port; required for a non-local server
        const bool local = (smtp ? a.smtpHost : a.imapHost) == "localhost" || (smtp ? a.smtpHost : a.imapHost) == "127.0.0.1";
        curl_easy_setopt(m_curl, CURLOPT_USE_SSL, long(local ? CURLUSESSL_TRY : CURLUSESSL_ALL));
        curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &m_body);
        curl_easy_setopt(m_curl, CURLOPT_ERRORBUFFER, m_error);
        // every line the server sends (curl keeps some untagged answers of a custom command, FETCH's,
        // from the body): through the debug callback, without printing anything
        curl_easy_setopt(m_curl, CURLOPT_DEBUGFUNCTION, debug);
        curl_easy_setopt(m_curl, CURLOPT_DEBUGDATA, &m_serverLines);
        curl_easy_setopt(m_curl, CURLOPT_VERBOSE, 1L);
        m_error[0] = 0;
    }
    static int debug(CURL *, curl_infotype type, char *data, size_t size, void *out)
    {
        if (type == CURLINFO_HEADER_IN)
            static_cast<std::string *>(out)->append(data, size);
        return 0;
    }
    const std::string &serverLines() const { return m_serverLines; }
    ~Session() { curl_easy_cleanup(m_curl); }
    CURL *operator*() const { return m_curl; }
    Result perform(const std::string &url, const char *custom = nullptr)
    {
        m_body.clear();
        m_serverLines.clear();
        curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_CUSTOMREQUEST, custom);
        CURLcode c = curl_easy_perform(m_curl);
        Result r;
        r.ok = c == CURLE_OK;
        if (!r.ok)
            r.error = m_error[0] ? m_error : curl_easy_strerror(c);
        return r;
    }
    const std::string &body() const { return m_body; }

private:
    CURL *m_curl;
    std::string m_body, m_serverLines;
    char m_error[CURL_ERROR_SIZE];
};

struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
} curlInit;

// IMAP's parenthesized lists and quoted strings, enough for LIST and FETCH (FLAGS)
std::vector<std::string> lines(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        out.push_back(l);
    }
    return out;
}

} // namespace

Result listMailboxes(const Account &a, const std::string &pw, std::vector<ListedMailbox> *out)
{
    Session s(a, pw);
    Result r = s.perform(imapURL(a), "LIST \"\" \"*\"");
    if (!r.ok)
        return r;
    for (const std::string &l : lines(s.body())) {
        // * LIST (\HasNoChildren \Sent) "/" "Sent Messages"
        if (l.rfind("* LIST", 0) != 0)
            continue;
        size_t fo = l.find('('), fc = l.find(')');
        if (fo == std::string::npos || fc == std::string::npos)
            continue;
        ListedMailbox m;
        m.flags = l.substr(fo + 1, fc - fo - 1);
        if (m.flags.find("\\Noselect") != std::string::npos || m.flags.find("\\NonExistent") != std::string::npos)
            continue;
        std::string rest = l.substr(fc + 1);
        // the delimiter, then the name
        size_t p = rest.find_first_not_of(' ');
        if (p == std::string::npos)
            continue;
        if (rest[p] == '"')
            p = rest.find('"', p + 1) + 1;
        else
            p = rest.find(' ', p);
        std::string name = rest.substr(rest.find_first_not_of(' ', p));
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
            std::string unq;
            for (size_t i = 1; i + 1 < name.size(); ++i) {
                if (name[i] == '\\' && i + 2 < name.size())
                    ++i;
                unq += name[i];
            }
            name = unq;
        }
        m.name = name;
        out->push_back(m);
    }
    return r;
}

Result uidsAndFlags(const Account &a, const std::string &pw, const std::string &mailbox,
                    std::vector<std::pair<unsigned, unsigned>> *out)
{
    Session s(a, pw);
    // curl passes on the untagged answers named like the command: "FETCH", not "UID FETCH"
    Result r = s.perform(imapURL(a, mailbox), "FETCH 1:* (UID FLAGS)");
    if (!r.ok) {
        // an empty mailbox answers "1:*" with an error on some servers
        if (r.error.find("FETCH") != std::string::npos || r.error.find("response") != std::string::npos) {
            r.ok = true;
            r.error.clear();
        }
        return r;
    }
    for (const std::string &l : lines(s.serverLines())) {
        // * 3 FETCH (UID 7 FLAGS (\Seen \Answered))
        if (l.rfind("* ", 0) != 0 || l.find("FETCH") == std::string::npos)
            continue;
        size_t u = l.find("UID ");
        if (u == std::string::npos)
            continue;
        unsigned uid = unsigned(std::strtoul(l.c_str() + u + 4, nullptr, 10));
        std::vector<std::string> names;
        size_t f = l.find("FLAGS (");
        if (f != std::string::npos) {
            size_t e = l.find(')', f);
            std::istringstream in(l.substr(f + 7, e - f - 7));
            std::string n;
            while (in >> n)
                names.push_back(n);
        }
        out->emplace_back(uid, flagsFromImap(names));
    }
    return r;
}

Result fetchMessage(const Account &a, const std::string &pw, const std::string &mailbox, unsigned uid, std::string *raw)
{
    Session s(a, pw);
    Result r = s.perform(imapURL(a, mailbox) + "/;UID=" + std::to_string(uid));
    if (r.ok)
        *raw = s.body();
    return r;
}

Result storeFlags(const Account &a, const std::string &pw, const std::string &mailbox, unsigned uid, bool add,
                  const std::string &imapFlags)
{
    Session s(a, pw);
    const std::string cmd = "UID STORE " + std::to_string(uid) + (add ? " +FLAGS (" : " -FLAGS (") + imapFlags + ")";
    return s.perform(imapURL(a, mailbox), cmd.c_str());
}

Result moveMessage(const Account &a, const std::string &pw, const std::string &mailbox, unsigned uid, const std::string &to)
{
    Session s(a, pw);
    std::string quoted = "\"";
    for (char c : to) {
        if (c == '"' || c == '\\')
            quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    const std::string id = std::to_string(uid);
    Result r = s.perform(imapURL(a, mailbox), ("UID MOVE " + id + " " + quoted).c_str());
    if (r.ok)
        return r;
    // no MOVE (RFC 6851): copy, mark deleted, expunge
    r = s.perform(imapURL(a, mailbox), ("UID COPY " + id + " " + quoted).c_str());
    if (!r.ok)
        return r;
    r = s.perform(imapURL(a, mailbox), ("UID STORE " + id + " +FLAGS (\\Deleted)").c_str());
    if (!r.ok)
        return r;
    return s.perform(imapURL(a, mailbox), "EXPUNGE");
}

Result appendMessage(const Account &a, const std::string &pw, const std::string &mailbox, const std::string &raw,
                     const std::string &imapFlags)
{
    (void)imapFlags;   // curl's APPEND sends no flags; the server's Sent copy is unread, as a client's would be
    Session s(a, pw);
    Upload u{&raw};
    curl_easy_setopt(*s, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(*s, CURLOPT_READFUNCTION, feed);
    curl_easy_setopt(*s, CURLOPT_READDATA, &u);
    curl_easy_setopt(*s, CURLOPT_INFILESIZE, long(raw.size()));
    return s.perform(imapURL(a, mailbox));
}

Result sendMessage(const Account &a, const std::string &pw, const std::string &from, const std::vector<std::string> &rcpts,
                   const std::string &raw)
{
    Session s(a, pw, true);
    curl_slist *list = nullptr;
    for (const std::string &r : rcpts)
        list = curl_slist_append(list, ("<" + r + ">").c_str());
    const std::string mailFrom = "<" + from + ">";
    Upload u{&raw};
    curl_easy_setopt(*s, CURLOPT_MAIL_FROM, mailFrom.c_str());
    curl_easy_setopt(*s, CURLOPT_MAIL_RCPT, list);
    curl_easy_setopt(*s, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(*s, CURLOPT_READFUNCTION, feed);
    curl_easy_setopt(*s, CURLOPT_READDATA, &u);
    const std::string url = std::string(a.smtpSSL ? "smtps://" : "smtp://") + a.smtpHost + ":" + std::to_string(a.smtpPort);
    Result r = s.perform(url);
    curl_slist_free_all(list);
    return r;
}

Result checkIncoming(const Account &a, const std::string &pw)
{
    std::vector<ListedMailbox> boxes;
    return listMailboxes(a, pw, &boxes);
}

Account guessAccount(const std::string &fullName, const std::string &email)
{
    Account a;
    a.fullName = fullName;
    a.email = email;
    const size_t at = email.find('@');
    const std::string domain = at == std::string::npos ? std::string() : email.substr(at + 1);
    a.imapUser = email;
    a.smtpUser = email;
    // the three domains -[AccountSetupAssistant _canUseAutomaticSetupForDomain:] knows (0x1001103c2)
    if (domain == "gmail.com" || domain == "googlemail.com") {
        a.imapHost = "imap.gmail.com";
        a.smtpHost = "smtp.gmail.com";
    } else if (domain == "yahoo.com") {
        a.imapHost = "imap.mail.yahoo.com";
        a.smtpHost = "smtp.mail.yahoo.com";
    } else if (domain == "aol.com") {
        a.imapHost = "imap.aol.com";
        a.smtpHost = "smtp.aol.com";
    } else {
        a.imapHost = "imap." + domain;
        a.smtpHost = "smtp." + domain;
    }
    return a;
}

// ---- the worker

Worker::Worker() : m_thread([this] { loop(); })
{
    m_timer = AK::App::shared().addTimer(0.1, true, [this] { drain(); });
}

Worker::~Worker()
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
    AK::App::shared().removeTimer(m_timer);
}

void Worker::run(std::function<void()> job, std::function<void()> done)
{
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_jobs.emplace_back(std::move(job), std::move(done));
        ++m_running;
    }
    m_cv.notify_one();
}

bool Worker::busy() const
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_running > 0;
}

void Worker::loop()
{
    for (;;) {
        std::pair<std::function<void()>, std::function<void()>> job;
        {
            std::unique_lock<std::mutex> l(m_mutex);
            m_cv.wait(l, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop)
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        job.first();
        std::lock_guard<std::mutex> l(m_mutex);
        m_done.push_back(job.second ? std::move(job.second) : std::function<void()>([] {}));
    }
}

void Worker::drain()
{
    std::vector<std::function<void()>> done;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        done.swap(m_done);
        m_running -= int(done.size());
    }
    for (auto &f : done)
        f();
}

} // namespace Mail
