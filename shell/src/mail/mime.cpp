// SPDX-License-Identifier: LGPL-2.1-or-later
#include "mime.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iconv.h>

namespace Mail {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A parameter of a structured header: Content-Type: text/plain; charset="utf-8"
std::string param(const std::string &value, const std::string &name)
{
    const std::string v = lower(value);
    size_t p = 0;
    while ((p = v.find(name, p)) != std::string::npos) {
        const bool start = p == 0 || v[p - 1] == ';' || std::isspace((unsigned char)v[p - 1]);
        size_t q = p + name.size();
        while (q < v.size() && std::isspace((unsigned char)v[q]))
            ++q;
        if (start && q < v.size() && v[q] == '=') {
            ++q;
            while (q < v.size() && std::isspace((unsigned char)v[q]))
                ++q;
            if (q < value.size() && value[q] == '"') {
                size_t e = value.find('"', q + 1);
                return value.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
            }
            size_t e = value.find_first_of("; \t\r\n", q);
            return value.substr(q, e == std::string::npos ? std::string::npos : e - q);
        }
        p += name.size();
    }
    return {};
}

std::string mainType(const std::string &contentType)
{
    std::string t = lower(trim(contentType.substr(0, contentType.find(';'))));
    return t.empty() ? "text/plain" : t;
}

std::string stripHtml(const std::string &html)
{
    std::string out;
    bool tag = false;
    std::string name;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') {
            tag = true;
            name.clear();
            continue;
        }
        if (tag) {
            if (c == '>') {
                tag = false;
                const std::string n = lower(name);
                if (n == "br" || n == "br/" || n == "/p" || n == "/div" || n == "/tr" || n == "/h1" || n == "/h2" || n == "/li")
                    out += '\n';
            } else if (name.size() < 8 && !std::isspace((unsigned char)c)) {
                name += c;
            } else {
                name += ' ';
            }
            continue;
        }
        if (c == '&') {
            static const std::pair<const char *, const char *> ents[] = {
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}};
            bool done = false;
            for (auto &e : ents)
                if (html.compare(i, std::strlen(e.first), e.first) == 0) {
                    out += e.second;
                    i += std::strlen(e.first) - 1;
                    done = true;
                    break;
                }
            if (done)
                continue;
        }
        out += c == '\r' ? ' ' : c;
    }
    // collapse the blank lines HTML leaves
    std::string collapsed;
    int newlines = 0;
    for (char c : out) {
        if (c == '\n') {
            if (++newlines > 2)
                continue;
        } else if (!std::isspace((unsigned char)c)) {
            newlines = 0;
        }
        collapsed += c;
    }
    return trim(collapsed);
}

struct Part {
    Headers headers;
    std::string body;
};

void walk(const std::string &raw, Message &m, std::string &plain, std::string &html, int depth)
{
    size_t start = 0;
    Headers h = parseHeaders(raw, &start);
    const std::string body = raw.substr(std::min(start, raw.size()));
    const std::string ct = h.get("content-type");
    const std::string type = mainType(ct);
    const std::string disposition = lower(h.get("content-disposition"));
    if (type.rfind("multipart/", 0) == 0 && depth < 10) {
        const std::string boundary = param(ct, "boundary");
        if (boundary.empty())
            return;
        const std::string delim = "--" + boundary;
        size_t p = body.find(delim);
        while (p != std::string::npos) {
            p += delim.size();
            if (body.compare(p, 2, "--") == 0)
                break;
            size_t next = body.find(delim, p);
            std::string part = body.substr(p, next == std::string::npos ? std::string::npos : next - p);
            if (!part.empty() && part[0] == '\r')
                part.erase(0, 1);
            if (!part.empty() && part[0] == '\n')
                part.erase(0, 1);
            walk(part, m, plain, html, depth + 1);
            p = next;
        }
        return;
    }
    if (type == "message/rfc822" && depth < 10) {
        walk(body, m, plain, html, depth + 1);
        return;
    }
    std::string filename = param(h.get("content-disposition"), "filename");
    if (filename.empty())
        filename = param(ct, "name");
    const std::string encoding = lower(trim(h.get("content-transfer-encoding")));
    auto decoded = [&] {
        if (encoding == "quoted-printable")
            return decodeQuotedPrintable(body);
        if (encoding == "base64")
            return decodeBase64(body);
        return body;
    };
    const bool attachment = disposition.rfind("attachment", 0) == 0 || (!filename.empty() && type.rfind("text/", 0) != 0);
    if (attachment) {
        m.attachments.push_back(filename.empty() ? std::string("Attachment") : decodeWords(filename));
        return;
    }
    const std::string charset = param(ct, "charset");
    if (type == "text/plain" && plain.empty())
        plain = toUtf8(decoded(), charset);
    else if (type == "text/html" && html.empty())
        html = toUtf8(decoded(), charset);
    else if (type.rfind("text/", 0) != 0 && !filename.empty())
        m.attachments.push_back(decodeWords(filename));
}

} // namespace

std::string Headers::get(const std::string &name) const
{
    const std::string n = lower(name);
    for (const auto &f : fields)
        if (lower(f.first) == n)
            return decodeWords(f.second);
    return {};
}

std::vector<std::string> Headers::keys() const
{
    std::vector<std::string> out;
    for (const auto &f : fields) {
        std::string k = lower(f.first);
        if (std::find(out.begin(), out.end(), k) == out.end())
            out.push_back(k);
    }
    return out;
}

Headers parseHeaders(const std::string &raw, size_t *bodyStart)
{
    Headers h;
    size_t p = 0;
    while (p < raw.size()) {
        size_t e = raw.find('\n', p);
        if (e == std::string::npos)
            e = raw.size();
        std::string line = raw.substr(p, e - p);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        p = e + 1;
        if (line.empty())
            break;
        if ((line[0] == ' ' || line[0] == '\t') && !h.fields.empty()) {
            h.fields.back().second += " " + trim(line);   // folded
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        h.fields.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    if (bodyStart)
        *bodyStart = std::min(p, raw.size());
    return h;
}

Message parse(const std::string &raw)
{
    Message m;
    m.headers = parseHeaders(raw);
    std::string plain, html;
    walk(raw, m, plain, html, 0);
    std::string text = !plain.empty() ? plain : stripHtml(html);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    m.text = text;
    return m;
}

std::string toUtf8(const std::string &bytes, const std::string &charset)
{
    std::string cs = lower(trim(charset));
    if (cs.empty() || cs == "utf-8" || cs == "utf8" || cs == "us-ascii" || cs == "ascii")
        return bytes;
    iconv_t cd = iconv_open("UTF-8//TRANSLIT", cs.c_str());
    if (cd == (iconv_t)-1)
        return bytes;
    std::string out;
    out.resize(bytes.size() * 4 + 16);
    char *in = const_cast<char *>(bytes.data());
    size_t inLeft = bytes.size(), outLeft = out.size();
    char *o = out.data();
    while (inLeft > 0) {
        if (iconv(cd, &in, &inLeft, &o, &outLeft) == (size_t)-1) {
            if (errno == EILSEQ || errno == EINVAL) {   // a bad byte: skip it
                ++in;
                --inLeft;
                continue;
            }
            break;
        }
    }
    iconv_close(cd);
    out.resize(out.size() - outLeft);
    return out;
}

std::string decodeQuotedPrintable(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        c = char(std::toupper((unsigned char)c));
        return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '=' && i + 1 < s.size()) {
            if (s[i + 1] == '\n') {
                ++i;
                continue;
            }
            if (s[i + 1] == '\r' && i + 2 < s.size() && s[i + 2] == '\n') {
                i += 2;
                continue;
            }
            if (i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
                out += char(hex(s[i + 1]) * 16 + hex(s[i + 2]));
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string decodeBase64(const std::string &s)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+' || c == '-')
            return 62;
        if (c == '/' || c == '_')
            return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0)
            continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::string encodeBase64(const std::string &s)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < s.size(); i += 3) {
        unsigned n = (unsigned char)s[i] << 16 | (unsigned char)s[i + 1] << 8 | (unsigned char)s[i + 2];
        out += t[n >> 18];
        out += t[(n >> 12) & 63];
        out += t[(n >> 6) & 63];
        out += t[n & 63];
    }
    if (i < s.size()) {
        unsigned n = (unsigned char)s[i] << 16 | (i + 1 < s.size() ? (unsigned char)s[i + 1] << 8 : 0);
        out += t[n >> 18];
        out += t[(n >> 12) & 63];
        out += i + 1 < s.size() ? t[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string decodeWords(const std::string &value)
{
    std::string out;
    size_t p = 0;
    bool lastWasWord = false;
    while (p < value.size()) {
        size_t s = value.find("=?", p);
        if (s == std::string::npos) {
            out += value.substr(p);
            break;
        }
        size_t q1 = value.find('?', s + 2);
        size_t q2 = q1 == std::string::npos ? q1 : value.find('?', q1 + 1);
        size_t e = q2 == std::string::npos ? q2 : value.find("?=", q2 + 1);
        if (e == std::string::npos) {
            out += value.substr(p);
            break;
        }
        std::string between = value.substr(p, s - p);
        if (!(lastWasWord && trim(between).empty()))   // spaces between two words vanish
            out += between;
        std::string charset = value.substr(s + 2, q1 - s - 2);
        if (size_t star = charset.find('*'); star != std::string::npos)
            charset.erase(star);
        const char enc = char(std::toupper((unsigned char)value[q1 + 1]));
        std::string text = value.substr(q2 + 1, e - q2 - 1);
        if (enc == 'B') {
            text = decodeBase64(text);
        } else {
            std::replace(text.begin(), text.end(), '_', ' ');
            text = decodeQuotedPrintable(text);
        }
        out += toUtf8(text, charset);
        p = e + 2;
        lastWasWord = true;
    }
    return out;
}

std::string encodeWord(const std::string &utf8)
{
    for (unsigned char c : utf8)
        if (c >= 0x80 || c < 0x20)
            return "=?UTF-8?B?" + encodeBase64(utf8) + "?=";
    return utf8;
}

std::string displayName(const std::string &from)
{
    std::string f = trim(decodeWords(from));
    size_t lt = f.find('<');
    if (lt != std::string::npos && lt > 0) {
        std::string name = trim(f.substr(0, lt));
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
            name = name.substr(1, name.size() - 2);
        if (!name.empty())
            return name;
    }
    return address(f);
}

std::string address(const std::string &from)
{
    size_t lt = from.find('<'), gt = from.find('>');
    if (lt != std::string::npos && gt != std::string::npos && gt > lt)
        return trim(from.substr(lt + 1, gt - lt - 1));
    return trim(from);
}

time_t parseDate(const std::string &date)
{
    static const char *months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string d = date;
    if (size_t comma = d.find(','); comma != std::string::npos)
        d = d.substr(comma + 1);
    char mon[8] = {0}, zone[16] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (std::sscanf(d.c_str(), "%d %3s %d %d:%d:%d %15s", &day, mon, &year, &hh, &mm, &ss, zone) < 5)
        return 0;
    int m = -1;
    for (int i = 0; i < 12; ++i)
        if (lower(mon) == months[i])
            m = i;
    if (m < 0)
        return 0;
    if (year < 100)
        year += year < 50 ? 2000 : 1900;
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = m;
    t.tm_mday = day;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_sec = ss;
    time_t utc = timegm(&t);
    int offset = 0;
    if ((zone[0] == '+' || zone[0] == '-') && std::strlen(zone) >= 5) {
        int v = std::atoi(zone + 1);
        offset = (v / 100) * 3600 + (v % 100) * 60;
        if (zone[0] == '-')
            offset = -offset;
    }
    return utc - offset;
}

std::string formatDate(time_t t)
{
    char buf[64];
    struct tm lt;
    localtime_r(&t, &lt);
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %z", &lt);
    return buf;
}

std::string listDate(time_t t)
{
    if (t <= 0)
        return {};
    time_t now = time(nullptr);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    char buf[64];
    std::strftime(buf, sizeof buf, "%H:%M", &a);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        return std::string("Today ") + buf;
    time_t y = now - 86400;
    struct tm c;
    localtime_r(&y, &c);
    if (a.tm_year == c.tm_year && a.tm_yday == c.tm_yday)
        return "Yesterday";
    std::strftime(buf, sizeof buf, "%d/%m/%y", &a);
    return buf;
}

} // namespace Mail
