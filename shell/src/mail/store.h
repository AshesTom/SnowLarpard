// SPDX-License-Identifier: LGPL-2.1-or-later
// Mail's accounts and messages on disk, laid out as 10.6's ~/Library/Mail (docs/mail-reverse.md §7–8),
// under $XDG_DATA_HOME/snowleopard/Mail:
//   <type>-<user>@<host>/<mailbox>.imapmbox/Messages/<uid>.emlx
// an .emlx being the message's length, a newline, the RFC 822 message, then an XML plist (flags,
// date-sent, subject, sender, remote-id). The accounts are in ~/.config/snowleopard/mail.conf, their
// passwords in the desktop's keyring (libsecret), never in a file.
#pragma once

#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace Mail {

// The message flags integer (-[Message messageFlags], from _IMAPMessageFlagsTable)
enum Flag : unsigned {
    Read = 0x1, Deleted = 0x2, Answered = 0x4, Flagged = 0x10, Draft = 0x40, Forwarded = 0x100,
    Redirected = 0x200, Junk = 0x1000000, NotJunk = 0x2000000,
};
unsigned flagsFromImap(const std::vector<std::string> &names);

struct Account {
    std::string fullName, email;
    std::string imapHost, imapUser;
    int imapPort = 993;
    bool imapSSL = true;        // imaps:// (else imap:// with STARTTLS when offered)
    std::string smtpHost, smtpUser;
    int smtpPort = 587;
    bool smtpSSL = false;       // smtps:// (465); else smtp:// with STARTTLS
    std::string dirName() const;   // "IMAP-user@host" (defaultPathNameForAccountWithHostname:username:)
    std::string description() const { return email.empty() ? imapHost : email; }
};

std::vector<Account> loadAccounts();
void saveAccounts(const std::vector<Account> &accounts);
std::string password(const Account &a);                       // "" when the keyring has none
bool storePassword(const Account &a, const std::string &pw);

// The mailboxes' special kinds (the sidebar's icons, -[MailboxesController iconForMailboxUid:smallSize:])
enum class Kind { Inbox, Drafts, Sent, Trash, Junk, Other };

struct Mailbox {
    std::string name;      // the server's name ("INBOX", "Sent", "Work/Clients")
    Kind kind = Kind::Other;
    std::string display;   // what the sidebar shows
};

struct Summary {
    unsigned uid = 0;
    std::string path;      // the .emlx
    std::string from, to, subject, messageId;
    time_t date = 0;
    unsigned flags = 0;
    size_t size = 0;
};

std::string root();                                          // $XDG_DATA_HOME/snowleopard/Mail
std::string mailboxDir(const Account &a, const std::string &mailbox);
std::string messagePath(const Account &a, const std::string &mailbox, unsigned uid);

// The .emlx format (+[Library _writeEmlxFile:...], _messageDataAtPath:)
std::string emlxData(const std::string &message, unsigned flags, unsigned uid, const std::string &subject,
                     const std::string &sender, time_t dateSent);
bool readEmlx(const std::string &data, std::string *message, std::map<std::string, std::string> *plist);

bool saveMessage(const Account &a, const std::string &mailbox, unsigned uid, const std::string &raw, unsigned flags);
bool setFlags(const std::string &emlxPath, unsigned flags);
std::string readMessage(const std::string &emlxPath);
std::vector<Summary> summaries(const Account &a, const std::string &mailbox);   // newest first
std::vector<unsigned> storedUids(const Account &a, const std::string &mailbox);
void removeMessage(const std::string &emlxPath);

Kind kindOf(const std::string &name, const std::string &listFlags);

} // namespace Mail
