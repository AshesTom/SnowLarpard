// SPDX-License-Identifier: LGPL-2.1-or-later
#include "store.h"

#include "foundation.h"
#include "mime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <libsecret/secret.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Mail {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string readFile(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return {};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

// written whole then renamed: a crash never leaves half a message
bool writeFile(const std::string &path, const std::string &data)
{
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string xmlEscape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '&')
            o += "&amp;";
        else if (c == '<')
            o += "&lt;";
        else if (c == '>')
            o += "&gt;";
        else if ((unsigned char)c >= 0x20 || c == '\t' || c == '\n')
            o += c;
    }
    return o;
}

std::string xmlUnescape(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 5, "&amp;") == 0) {
            o += '&';
            i += 4;
        } else if (s.compare(i, 4, "&lt;") == 0) {
            o += '<';
            i += 3;
        } else if (s.compare(i, 4, "&gt;") == 0) {
            o += '>';
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

const SecretSchema *schema()
{
    static const SecretSchema s = {"org.snowleopard.Mail", SECRET_SCHEMA_NONE,
                                   {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"NULL", SECRET_SCHEMA_ATTRIBUTE_STRING}}};
    return &s;
}

std::string sanitize(const std::string &name)
{
    std::string o;
    for (char c : name)
        o += (c == '/' || c == '\0') ? ':' : c;   // a server's hierarchy kept as one folder name
    return o;
}

} // namespace

unsigned flagsFromImap(const std::vector<std::string> &names)
{
    // _IMAPMessageFlagsTable (Message.framework 0x341900), compared without case
    static const std::map<std::string, unsigned> table = {
        {"\\seen", Read}, {"\\deleted", Deleted}, {"\\answered", Answered}, {"\\flagged", Flagged},
        {"\\draft", Draft}, {"$forwarded", Forwarded}, {"forwarded", Forwarded}, {"redirected", Redirected},
        {"$junk", Junk}, {"junk", Junk}, {"$notjunk", NotJunk}, {"notjunk", NotJunk}, {"junkrecorded", 0x20000000}};
    unsigned v = 0;
    for (const std::string &n : names) {
        auto it = table.find(lower(n));
        if (it != table.end())
            v |= it->second;
    }
    return v;
}

std::string Account::dirName() const { return "IMAP-" + imapUser + "@" + imapHost; }

std::vector<Account> loadAccounts()
{
    AK::Settings s("mail");
    std::vector<Account> out;
    for (int i = 0; i < s.integer("accounts"); ++i) {
        const std::string p = "account" + std::to_string(i) + "-";
        Account a;
        a.fullName = s.string(p + "fullName");
        a.email = s.string(p + "email");
        a.imapHost = s.string(p + "imapHost");
        a.imapUser = s.string(p + "imapUser");
        a.imapPort = s.integer(p + "imapPort", 993);
        a.imapSSL = s.boolean(p + "imapSSL", true);
        a.smtpHost = s.string(p + "smtpHost");
        a.smtpUser = s.string(p + "smtpUser");
        a.smtpPort = s.integer(p + "smtpPort", 587);
        a.smtpSSL = s.boolean(p + "smtpSSL", false);
        if (!a.imapHost.empty())
            out.push_back(a);
    }
    return out;
}

void saveAccounts(const std::vector<Account> &accounts)
{
    AK::Settings s("mail");
    s.set("accounts", int(accounts.size()));
    for (size_t i = 0; i < accounts.size(); ++i) {
        const Account &a = accounts[i];
        const std::string p = "account" + std::to_string(i) + "-";
        s.set(p + "fullName", a.fullName);
        s.set(p + "email", a.email);
        s.set(p + "imapHost", a.imapHost);
        s.set(p + "imapUser", a.imapUser);
        s.set(p + "imapPort", a.imapPort);
        s.set(p + "imapSSL", a.imapSSL);
        s.set(p + "smtpHost", a.smtpHost);
        s.set(p + "smtpUser", a.smtpUser);
        s.set(p + "smtpPort", a.smtpPort);
        s.set(p + "smtpSSL", a.smtpSSL);
    }
}

std::string password(const Account &a)
{
    GError *error = nullptr;
    gchar *pw = secret_password_lookup_sync(schema(), nullptr, &error, "account", a.dirName().c_str(), nullptr);
    if (error) {
        std::fprintf(stderr, "Mail: keyring: %s\n", error->message);
        g_error_free(error);
    }
    std::string out = pw ? pw : "";
    if (pw)
        secret_password_free(pw);
    return out;
}

bool storePassword(const Account &a, const std::string &pw)
{
    GError *error = nullptr;
    const std::string label = "Mail: " + a.description();
    gboolean ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), pw.c_str(), nullptr,
                                             &error, "account", a.dirName().c_str(), nullptr);
    if (error) {
        std::fprintf(stderr, "Mail: keyring: %s\n", error->message);
        g_error_free(error);
    }
    return ok;
}

std::string root()
{
    const char *xdg = std::getenv("XDG_DATA_HOME");
    const std::string base = xdg && *xdg ? std::string(xdg) : AK::home() + "/.local/share";
    return base + "/snowleopard/Mail";
}

std::string mailboxDir(const Account &a, const std::string &mailbox)
{
    return root() + "/" + a.dirName() + "/" + sanitize(mailbox) + ".imapmbox";
}

std::string messagePath(const Account &a, const std::string &mailbox, unsigned uid)
{
    return mailboxDir(a, mailbox) + "/Messages/" + std::to_string(uid) + ".emlx";   // _filenameForMessageID
}

std::string emlxData(const std::string &message, unsigned flags, unsigned uid, const std::string &subject,
                     const std::string &sender, time_t dateSent)
{
    std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>date-sent</key>\n\t<real>" + std::to_string((long long)dateSent) + "</real>\n"
        "\t<key>flags</key>\n\t<integer>" + std::to_string(flags) + "</integer>\n"
        "\t<key>remote-id</key>\n\t<string>" + std::to_string(uid) + "</string>\n"
        "\t<key>sender</key>\n\t<string>" + xmlEscape(sender) + "</string>\n"
        "\t<key>subject</key>\n\t<string>" + xmlEscape(subject) + "</string>\n"
        "</dict>\n</plist>\n";
    return std::to_string(message.size()) + "\n" + message + plist;
}

bool readEmlx(const std::string &data, std::string *message, std::map<std::string, std::string> *plist)
{
    size_t nl = data.find('\n');
    if (nl == std::string::npos || nl == 0)
        return false;
    for (size_t i = 0; i < nl; ++i)
        if (!std::isdigit((unsigned char)data[i]) && data[i] != ' ')
            return false;
    const size_t length = std::strtoull(data.c_str(), nullptr, 10);
    if (nl + 1 + length > data.size())
        return false;
    if (message)
        *message = data.substr(nl + 1, length);
    if (plist) {
        const std::string tail = data.substr(nl + 1 + length);
        size_t p = 0;
        while ((p = tail.find("<key>", p)) != std::string::npos) {
            size_t e = tail.find("</key>", p);
            if (e == std::string::npos)
                break;
            const std::string key = xmlUnescape(tail.substr(p + 5, e - p - 5));
            size_t vs = tail.find('<', e + 6);
            size_t ve = vs == std::string::npos ? vs : tail.find('>', vs);
            if (ve == std::string::npos)
                break;
            const std::string tag = tail.substr(vs + 1, ve - vs - 1);
            size_t close = tail.find("</" + tag + ">", ve);
            if (close != std::string::npos)
                (*plist)[key] = xmlUnescape(tail.substr(ve + 1, close - ve - 1));
            p = e + 6;
        }
    }
    return true;
}

bool saveMessage(const Account &a, const std::string &mailbox, unsigned uid, const std::string &raw, unsigned flags)
{
    const std::string dir = mailboxDir(a, mailbox) + "/Messages";
    AK::makePath(dir);
    Headers h = parseHeaders(raw);
    time_t date = parseDate(h.get("date"));
    return writeFile(messagePath(a, mailbox, uid), emlxData(raw, flags, uid, h.get("subject"), h.get("from"), date));
}

bool setFlags(const std::string &emlxPath, unsigned flags)
{
    std::string message;
    std::map<std::string, std::string> plist;
    if (!readEmlx(readFile(emlxPath), &message, &plist))
        return false;
    const unsigned uid = unsigned(std::strtoul(plist["remote-id"].c_str(), nullptr, 10));
    return writeFile(emlxPath, emlxData(message, flags, uid, plist["subject"], plist["sender"],
                                        time_t(std::strtoll(plist["date-sent"].c_str(), nullptr, 10))));
}

std::string readMessage(const std::string &emlxPath)
{
    std::string message;
    readEmlx(readFile(emlxPath), &message, nullptr);
    return message;
}

std::vector<unsigned> storedUids(const Account &a, const std::string &mailbox)
{
    std::vector<unsigned> out;
    const std::string dir = mailboxDir(a, mailbox) + "/Messages";
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".emlx") == 0 && std::isdigit((unsigned char)n[0]))
                out.push_back(unsigned(std::strtoul(n.c_str(), nullptr, 10)));
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Summary> summaries(const Account &a, const std::string &mailbox)
{
    std::vector<Summary> out;
    for (unsigned uid : storedUids(a, mailbox)) {
        Summary s;
        s.uid = uid;
        s.path = messagePath(a, mailbox, uid);
        const std::string data = readFile(s.path);
        std::string message;
        std::map<std::string, std::string> plist;
        if (!readEmlx(data, &message, &plist))
            continue;
        Headers h = parseHeaders(message);
        s.from = h.get("from");
        s.to = h.get("to");
        s.subject = h.get("subject");
        s.messageId = h.get("message-id");
        s.date = parseDate(h.get("date"));
        s.flags = unsigned(std::strtoul(plist["flags"].c_str(), nullptr, 10));
        s.size = message.size();
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Summary &x, const Summary &y) {
        return x.date != y.date ? x.date > y.date : x.uid > y.uid;
    });
    return out;
}

void removeMessage(const std::string &emlxPath) { unlink(emlxPath.c_str()); }

Kind kindOf(const std::string &name, const std::string &listFlags)
{
    const std::string f = lower(listFlags), n = lower(name);
    if (n == "inbox")
        return Kind::Inbox;
    // RFC 6154's special-use flags first, then the names servers give them
    if (f.find("\\sent") != std::string::npos)
        return Kind::Sent;
    if (f.find("\\drafts") != std::string::npos)
        return Kind::Drafts;
    if (f.find("\\trash") != std::string::npos)
        return Kind::Trash;
    if (f.find("\\junk") != std::string::npos)
        return Kind::Junk;
    std::string leaf = n.substr(n.find_last_of("/.") == std::string::npos ? 0 : n.find_last_of("/.") + 1);
    if (leaf == "sent" || leaf == "sent messages" || leaf == "sent items" || leaf == "sent mail")
        return Kind::Sent;
    if (leaf == "drafts")
        return Kind::Drafts;
    if (leaf == "trash" || leaf == "deleted messages" || leaf == "deleted items" || leaf == "bin")
        return Kind::Trash;
    if (leaf == "junk" || leaf == "spam" || leaf == "junk e-mail")
        return Kind::Junk;
    return Kind::Other;
}

} // namespace Mail
