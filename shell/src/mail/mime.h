// SPDX-License-Identifier: LGPL-2.1-or-later
// What Mail needs of RFC 822 / MIME to show a message: the headers (unfolded, RFC 2047 words
// decoded to UTF-8), the text to show (text/plain, else text/html made plain), the attachments' names.
#pragma once

#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace Mail {

struct Headers {
    std::vector<std::pair<std::string, std::string>> fields;   // as written, in order (values raw)
    std::string get(const std::string &name) const;            // the first, decoded; "" when missing
    std::vector<std::string> keys() const;                     // lowercased, each once
};

struct Message {
    Headers headers;
    std::string text;                       // UTF-8, what the message pane shows
    std::vector<std::string> attachments;   // file names
};

// Splits at the blank line; headers only when `headersOnly`
Headers parseHeaders(const std::string &raw, size_t *bodyStart = nullptr);
Message parse(const std::string &raw);

std::string decodeWords(const std::string &value);          // =?charset?Q|B?...?=
std::string toUtf8(const std::string &bytes, const std::string &charset);
std::string decodeQuotedPrintable(const std::string &s);
std::string decodeBase64(const std::string &s);
std::string encodeBase64(const std::string &s);
std::string encodeWord(const std::string &utf8);            // a header value, =?UTF-8?B?..?= if needed

// "Name <addr>" -> display name (or the address); the address alone
std::string displayName(const std::string &from);
std::string address(const std::string &from);

time_t parseDate(const std::string &date);   // RFC 822 date, 0 when unreadable
std::string formatDate(time_t t);            // for a Date: header
std::string listDate(time_t t);              // the list's: "Today 12:04", "Yesterday", "15/11/26"

} // namespace Mail
