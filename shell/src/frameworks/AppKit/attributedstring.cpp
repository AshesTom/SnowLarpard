// SPDX-License-Identifier: LGPL-2.1-or-later
#include "attributedstring.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <tuple>

namespace AK {

namespace {

// Mac OS Roman, 0x80 to 0xFF (\mac, \ansicpg10000)
const uint16_t MacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7};

// Windows 1252, 0x80 to 0x9F (\ansi, \ansicpg1252; the rest is Latin 1)
const uint16_t Cp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                             0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                             0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

void appendUtf8(std::string &s, uint32_t c)
{
    if (c < 0x80) {
        s += char(c);
    } else if (c < 0x800) {
        s += char(0xC0 | (c >> 6));
        s += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += char(0xE0 | (c >> 12));
        s += char(0x80 | ((c >> 6) & 0x3F));
        s += char(0x80 | (c & 0x3F));
    } else {
        s += char(0xF0 | (c >> 18));
        s += char(0x80 | ((c >> 12) & 0x3F));
        s += char(0x80 | ((c >> 6) & 0x3F));
        s += char(0x80 | (c & 0x3F));
    }
}

// the code point at i, i moved past it; invalid bytes as U+FFFD
uint32_t nextCodePoint(const std::string &s, size_t &i)
{
    const unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n < 0 || i + size_t(n) >= s.size() + (n == 0 ? 1 : 0)) {
        ++i;
        return n == 0 ? c : 0xFFFD;
    }
    uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k <= n; ++k) {
        const unsigned char d = (unsigned char)s[i + size_t(k)];
        if ((d & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    i += size_t(n) + 1;
    return cp;
}

bool validUtf8(const std::string &s)
{
    for (size_t i = 0; i < s.size();) {
        const size_t at = i;
        if (nextCodePoint(s, i) == 0xFFFD && !(s.compare(at, 3, "\xEF\xBF\xBD") == 0))
            return false;
    }
    return true;
}

uint32_t decodeByte(unsigned char b, int codepage)
{
    if (b < 0x80)
        return b;
    if (codepage == 10000)
        return MacRoman[b - 0x80];
    if (b < 0xA0)
        return Cp1252[b - 0x80];
    return b;
}

// "LucidaGrande" -> "Lucida Grande"
std::string spaced(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && std::isupper((unsigned char)s[i]) && std::islower((unsigned char)s[i - 1]))
            out += ' ';
        out += s[i];
    }
    return out;
}

std::string squeezed(const std::string &s)
{
    std::string out;
    for (char c : s)
        if (c != ' ')
            out += c;
    return out;
}

bool removeWord(std::string &s, const char *word)
{
    const size_t at = s.find(word);
    if (at == std::string::npos)
        return false;
    s.erase(at, std::strlen(word));
    return true;
}

} // namespace

// ---- styles

bool Style::operator==(const Style &o) const
{
    return family == o.family && face == o.face && bold == o.bold && italic == o.italic && size == o.size && underline == o.underline &&
           strike == o.strike && coloured == o.coloured && (!coloured || (colour.r == o.colour.r && colour.g == o.colour.g && colour.b == o.colour.b)) &&
           script == o.script && baseline == o.baseline && align == o.align && para == o.para && link == o.link;
}

// ---- NSTextList's markers (-_markerForMarkerFormat:itemNumber:isNumbered:… 0x55f7f5)

namespace {

const char *const Bullets[][2] = {{"box", "\u25AB"},     {"check", "\u2713"}, {"circle", "\u25E6"}, {"diamond", "\u25C6"},
                                  {"disc", "\u2022"},    {"hyphen", "\u2043"}, {"square", "\u25AA"}, {"none", ""}};

std::string roman(long n, bool upper)
{
    // m cm d cd c xc l xl x ix v iv i
    static const std::pair<long, const char *> table[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"},
                                                          {40, "xl"},  {10, "x"},   {9, "ix"},  {5, "v"},   {4, "iv"},  {1, "i"}};
    std::string out;
    if (n <= 0)
        return std::to_string(n);
    for (const auto &[v, r] : table)
        while (n >= v) {
            out += r;
            n -= v;
        }
    if (upper)
        for (char &c : out)
            c = char(std::toupper((unsigned char)c));
    return out;
}

// a, b … z, aa, ab …: the letters of an alphabet as digits with no zero
std::string alphabetic(long n, const std::vector<uint32_t> &letters)
{
    if (n <= 0 || letters.empty())
        return std::to_string(n);
    std::vector<uint32_t> digits;
    while (n > 0) {
        --n;
        digits.push_back(letters[size_t(n % long(letters.size()))]);
        n /= long(letters.size());
    }
    std::string out;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        appendUtf8(out, *it);
    return out;
}

std::vector<uint32_t> range(uint32_t from, uint32_t to, uint32_t skip = 0)
{
    std::vector<uint32_t> v;
    for (uint32_t c = from; c <= to; ++c)
        if (c != skip)
            v.push_back(c);
    return v;
}

} // namespace

std::string TextList::specifier() const
{
    const size_t a = markerFormat.find('{'), b = markerFormat.find('}', a == std::string::npos ? 0 : a);
    return a == std::string::npos || b == std::string::npos ? std::string() : markerFormat.substr(a + 1, b - a - 1);
}

std::string TextList::prefix() const
{
    const size_t a = markerFormat.find('{');
    return a == std::string::npos ? markerFormat : markerFormat.substr(0, a);
}

std::string TextList::suffix() const
{
    const size_t a = markerFormat.find('{'), b = markerFormat.find('}', a == std::string::npos ? 0 : a);
    return a == std::string::npos || b == std::string::npos ? std::string() : markerFormat.substr(b + 1);
}

bool TextList::isOrdered() const
{
    const std::string spec = specifier();
    if (spec.empty())
        return false;
    for (const auto &b : Bullets)
        if (spec == b[0])
            return false;
    return true;
}

std::string TextList::unaffixedMarker(long n) const
{
    const std::string spec = specifier();
    for (const auto &b : Bullets)
        if (spec == b[0])
            return b[1];
    char buf[32];
    if (spec == "lower-hexadecimal" || spec == "upper-hexadecimal" || spec == "octal") {
        std::snprintf(buf, sizeof buf, spec == "octal" ? "%lo" : spec[0] == 'l' ? "%lx" : "%lX", n);
        return buf;
    }
    if (spec == "lower-alpha" || spec == "lower-latin")
        return alphabetic(n, range('a', 'z'));
    if (spec == "upper-alpha" || spec == "upper-latin")
        return alphabetic(n, range('A', 'Z'));
    if (spec == "lower-greek")
        return alphabetic(n, range(0x3B1, 0x3C9, 0x3C2));   // α … ω, final sigma left out
    if (spec == "upper-greek")
        return alphabetic(n, range(0x391, 0x3A9, 0x3A2));
    if (spec == "lower-russian")
        return alphabetic(n, range(0x430, 0x44F));
    if (spec == "upper-russian")
        return alphabetic(n, range(0x410, 0x42F));
    if (spec == "lower-roman" || spec == "upper-roman")
        return roman(n, spec[0] == 'u');
    if (spec == "decimal-leading-zero") {
        std::snprintf(buf, sizeof buf, n >= 0 && n < 10 ? "0%ld" : "%ld", n);
        return buf;
    }
    if (spec == "arabic-indic") {
        std::string out;
        for (char d : std::to_string(n))
            if (d >= '0' && d <= '9')
                appendUtf8(out, 0x660 + uint32_t(d - '0'));
            else
                out += d;
        return out;
    }
    // decimal, and the scripts not drawn here (hebrew, the CJK and kana counters): numbers
    return std::to_string(n);
}

std::string TextList::marker(long n) const
{
    if (specifier().empty())
        return markerFormat;
    return prefix() + unaffixedMarker(n) + suffix();
}

bool ParagraphStyle::operator==(const ParagraphStyle &o) const
{
    return firstLineHeadIndent == o.firstLineHeadIndent && headIndent == o.headIndent && tailIndent == o.tailIndent &&
           hasTabs == o.hasTabs && tabStops == o.tabStops && defaultTabInterval == o.defaultTabInterval &&
           lineHeightMultiple == o.lineHeightMultiple && lineSpacing == o.lineSpacing &&
           minimumLineHeight == o.minimumLineHeight && maximumLineHeight == o.maximumLineHeight &&
           textLists == o.textLists &&
           paragraphSpacingBefore == o.paragraphSpacingBefore && paragraphSpacing == o.paragraphSpacing;
}

Style Style::fromPostScriptName(const std::string &name, double size)
{
    Style s;
    s.size = size;
    std::string base = name, suffix;
    if (const size_t dash = name.find('-'); dash != std::string::npos) {
        base = name.substr(0, dash);
        suffix = name.substr(dash + 1);
    }
    for (const char *mt : {"PSMT", "PS", "MT"})
        if (base.size() > std::strlen(mt) && base.compare(base.size() - std::strlen(mt), std::string::npos, mt) == 0) {
            base.erase(base.size() - std::strlen(mt));
            break;
        }
    removeWord(suffix, "MT");
    s.bold = removeWord(suffix, "Bold");
    s.italic = removeWord(suffix, "Italic") || removeWord(suffix, "Oblique");
    removeWord(suffix, "Roman");
    removeWord(suffix, "Regular");
    s.face = suffix;
    s.family = spaced(base);
    if (s.family.empty())
        s.family = "Helvetica";
    return s;
}

std::string Style::postScriptName() const
{
    const std::string base = squeezed(family);
    if (base == "Times" || base == "Courier" || base == "Helvetica") {
        const char *slant = base == "Times" ? "Italic" : "Oblique";
        if (!bold && !italic && face.empty())
            return base == "Times" ? "Times-Roman" : base;
        return base + "-" + face + (bold ? "Bold" : "") + (italic ? slant : "");
    }
    if (base == "Arial")
        return std::string(bold || italic ? "Arial-" : "Arial") + (bold ? "Bold" : "") + (italic ? "Italic" : "") + "MT";
    if (base == "TimesNewRoman")
        return std::string(bold || italic ? "TimesNewRomanPS-" : "TimesNewRomanPS") + (bold ? "Bold" : "") + (italic ? "Italic" : "") + "MT";
    if (!bold && !italic && face.empty())
        return base;
    return base + "-" + face + (bold ? "Bold" : "") + (italic ? "Italic" : "");
}

// ---- the text and its runs

const Style &RichText::styleAt(size_t byte) const
{
    static const Style plain;
    if (runs.empty())
        return styles.empty() ? plain : styles[0];
    return styles[runs[std::min(byte, runs.size() - 1)]];
}

uint16_t RichText::index(const Style &s)
{
    for (size_t i = 0; i < styles.size(); ++i)
        if (styles[i] == s)
            return uint16_t(i);
    styles.push_back(s);
    return uint16_t(styles.size() - 1);
}

void RichText::insert(size_t at, const std::string &s, const Style &style)
{
    at = std::min(at, text.size());
    const uint16_t i = index(style);
    text.insert(at, s);
    runs.insert(runs.begin() + long(at), s.size(), i);
}

size_t RichText::attachmentIndex(size_t byte) const
{
    size_t n = 0;
    for (size_t p = text.find(AttachmentCharacter); p != std::string::npos && p < byte; p = text.find(AttachmentCharacter, p + 3))
        ++n;
    return n;
}

void RichText::erase(size_t from, size_t to)
{
    to = std::min(to, text.size());
    if (from >= to)
        return;
    // the attachments inside go with their characters
    size_t first = attachmentIndex(from), count = 0;
    for (size_t p = text.find(AttachmentCharacter, from); p != std::string::npos && p + 3 <= to; p = text.find(AttachmentCharacter, p + 3))
        ++count;
    if (count && first < attachments.size())
        attachments.erase(attachments.begin() + long(first), attachments.begin() + long(std::min(attachments.size(), first + count)));
    text.erase(from, to - from);
    runs.erase(runs.begin() + long(from), runs.begin() + long(to));
}

RichText RichText::slice(size_t from, size_t to) const
{
    RichText r;
    to = std::min(to, text.size());
    if (from >= to)
        return r;
    r.text = text.substr(from, to - from);
    r.styles = styles;
    r.runs.assign(runs.begin() + long(from), runs.begin() + long(to));
    size_t first = attachmentIndex(from);
    for (size_t p = r.text.find(AttachmentCharacter); p != std::string::npos; p = r.text.find(AttachmentCharacter, p + 3))
        if (first < attachments.size())
            r.attachments.push_back(attachments[first++]);
    r.compact();
    return r;
}

void RichText::insert(size_t at, const RichText &other)
{
    at = std::min(at, text.size());
    std::vector<uint16_t> mapped;
    for (const Style &s : other.styles)
        mapped.push_back(index(s));
    const size_t first = attachmentIndex(at);
    text.insert(at, other.text);
    std::vector<uint16_t> r;
    for (uint16_t i : other.runs)
        r.push_back(i < mapped.size() ? mapped[i] : 0);
    r.resize(other.text.size(), 0);
    runs.insert(runs.begin() + long(at), r.begin(), r.end());
    attachments.insert(attachments.begin() + long(std::min(first, attachments.size())), other.attachments.begin(), other.attachments.end());
}

void RichText::setStyle(size_t from, size_t to, const std::function<void(Style &)> &change)
{
    to = std::min(to, text.size());
    std::map<uint16_t, uint16_t> done;
    for (size_t i = from; i < to; ++i) {
        auto it = done.find(runs[i]);
        if (it == done.end()) {
            Style s = styles[runs[i]];
            change(s);
            it = done.emplace(runs[i], index(s)).first;
        }
        runs[i] = it->second;
    }
}

void RichText::compact()
{
    std::vector<int> used(styles.size(), -1);
    std::vector<Style> kept;
    for (uint16_t &r : runs) {
        if (r >= styles.size())
            r = 0;
        if (used[r] < 0) {
            used[r] = int(kept.size());
            kept.push_back(styles[r]);
        }
        r = uint16_t(used[r]);
    }
    if (kept.empty() && !styles.empty())
        kept.push_back(styles[0]);
    styles = kept;
}

// ---- RTF, as Cocoa reads it

RichText fromRTF(const std::string &rtf, const std::map<std::string, std::string> &files)
{
    RichText out;
    enum Dest { Text, FontTable, ColorTable, Ignore, Graphic, Info, FieldInstruction, ListTable, LevelMarker };
    struct State {
        Style style;
        Dest dest = Text;
        std::string *info = nullptr;   // the \info property its text goes to
        int uc = 1;
        int skip = 0;   // characters to skip (a \u's fallback, an attachment's placeholder)
    };
    std::vector<State> stack(1);
    std::map<int, std::string> fonts;
    std::vector<AK::Color> colours;   // index 0 the default
    bool colourAuto = true;
    double red = 0, green = 0, blue = 0;
    int fontNumber = 0, codepage = 1252;
    std::string fontName, graphicName;
    double graphicWidth = 0, graphicHeight = 0;
    uint32_t highSurrogate = 0;
    TabStop::Kind tabKind = TabStop::Left;   // \tqr and the like, for the next \tx
    long slValue = 0;                        // \sl, for \slmult1 after it
    std::string fieldInstruction;            // the last \fldinst's text (HYPERLINK "url")
    // the list table (NSRTFReader): each \list's first level (its \levelmarker, \levelstartat,
    // \levelprepend) by its \listid; \listoverride's \ls numbers to those
    struct RtfList {
        long id = 0;
        TextList list;
        int levels = 0;
    };
    std::vector<RtfList> rtfLists;
    std::map<long, long> listOverrides;
    bool inListOverride = false;
    long overrideListId = 0;
    stack[0].style.size = 12;

    auto put = [&](uint32_t c) {
        State &s = stack.back();
        if (s.skip > 0) {
            --s.skip;
            return;
        }
        if (s.dest == FieldInstruction) {
            appendUtf8(fieldInstruction, c);
            return;
        }
        if (s.dest == LevelMarker) {
            if (!rtfLists.empty() && rtfLists.back().levels == 1)
                appendUtf8(rtfLists.back().list.markerFormat, c);
            return;
        }
        if (s.dest == ListTable)
            return;
        if (s.dest == FontTable) {
            if (c == ';') {
                fonts[fontNumber] = fontName;
                fontName.clear();
            } else if (c != ' ' || !fontName.empty()) {
                appendUtf8(fontName, c);
            }
            return;
        }
        if (s.dest == ColorTable) {
            if (c == ';') {
                colours.push_back(colourAuto ? AK::Color::gray(0) : AK::Color::rgb(int(red), int(green), int(blue)));
                colourAuto = true;
                red = green = blue = 0;
            }
            return;
        }
        if (s.dest == Graphic) {
            if (c != ' ' || !graphicName.empty())
                appendUtf8(graphicName, c);
            return;
        }
        if (s.dest == Info) {
            if (s.info)
                appendUtf8(*s.info, c);
            return;
        }
        if (s.dest != Text)
            return;
        if (c >= 0xD800 && c < 0xDC00) {
            highSurrogate = c;
            return;
        }
        if (c >= 0xDC00 && c < 0xE000 && highSurrogate) {
            c = 0x10000 + ((highSurrogate - 0xD800) << 10) + (c - 0xDC00);
            highSurrogate = 0;
        }
        std::string u;
        appendUtf8(u, c);
        out.insert(out.text.size(), u, s.style);
    };

    size_t i = 0;
    const size_t n = rtf.size();
    while (i < n) {
        const unsigned char ch = (unsigned char)rtf[i];
        if (ch == '{') {
            State s = stack.back();
            s.skip = 0;
            stack.push_back(s);
            ++i;
        } else if (ch == '}') {
            if (stack.back().dest == Graphic) {
                // the picture's character, then the placeholder Cocoa writes after its group is skipped
                Attachment a;
                a.name = graphicName;
                while (!a.name.empty() && a.name.back() == ' ')
                    a.name.pop_back();
                a.width = graphicWidth / 20;
                a.height = graphicHeight / 20;
                if (auto f = files.find(a.name); f != files.end())
                    a.data = f->second;
                stack.pop_back();
                if (!stack.empty()) {
                    State &outer = stack.back();
                    out.insert(out.text.size(), AttachmentCharacter, outer.style);
                    out.attachments.push_back(std::move(a));
                    outer.skip = 1;
                }
                graphicName.clear();
                graphicWidth = graphicHeight = 0;
            } else if (stack.size() > 1) {
                if (stack.back().dest == FontTable && !fontName.empty()) {
                    fonts[fontNumber] = fontName;
                    fontName.clear();
                }
                stack.pop_back();
            }
            ++i;
        } else if (ch == '\\') {
            ++i;
            if (i >= n)
                break;
            const char c = rtf[i];
            if (std::isalpha((unsigned char)c)) {
                const size_t start = i;
                while (i < n && std::isalpha((unsigned char)rtf[i]))
                    ++i;
                const std::string word = rtf.substr(start, i - start);
                bool hasParam = false;
                long param = 0;
                bool negative = false;
                if (i < n && rtf[i] == '-') {
                    negative = true;
                    ++i;
                }
                while (i < n && std::isdigit((unsigned char)rtf[i])) {
                    hasParam = true;
                    param = param * 10 + (rtf[i] - '0');
                    ++i;
                }
                if (negative)
                    param = -param;
                if (i < n && rtf[i] == ' ')
                    ++i;
                State &s = stack.back();
                Style &st = s.style;
                const bool on = !hasParam || param != 0;
                DocumentAttributes &doc = out.document;
                if (hasParam && (word == "paperw" || word == "paperh" || word == "margl" || word == "margr" || word == "margt" ||
                                 word == "margb" || word == "vieww" || word == "viewh")) {
                    const double pt = param / 20.0;
                    (word == "paperw" ? doc.paperWidth : word == "paperh" ? doc.paperHeight : word == "margl" ? doc.left
                     : word == "margr" ? doc.right : word == "margt" ? doc.top : word == "margb" ? doc.bottom
                     : word == "vieww" ? doc.viewWidth : doc.viewHeight) = pt;
                } else if (word == "viewkind" && hasParam)
                    doc.viewKind = int(param);
                else if (word == "fonttbl")
                    s.dest = FontTable;
                else if (word == "colortbl")
                    s.dest = ColorTable, colourAuto = true;
                else if (word == "info")
                    s.dest = Info, s.info = nullptr;
                else if (s.dest == Info && doc.property(word))
                    s.info = doc.property(word);
                else if (word == "listtable" || word == "listoverridetable")
                    s.dest = ListTable;
                else if (s.dest == ListTable || s.dest == LevelMarker) {
                    if (word == "list") {
                        rtfLists.push_back({});
                        inListOverride = false;
                    } else if (word == "listlevel" && !rtfLists.empty())
                        ++rtfLists.back().levels;
                    else if (word == "levelstartat" && hasParam && !rtfLists.empty() && rtfLists.back().levels == 1)
                        rtfLists.back().list.start = param;
                    else if (word == "levelprepend" && !rtfLists.empty() && rtfLists.back().levels == 1)
                        rtfLists.back().list.options |= 1;
                    else if (word == "levelmarker")
                        s.dest = LevelMarker;
                    else if (word == "leveltext" || word == "levelnumbers" || word == "listname")
                        s.dest = Ignore;
                    else if (word == "listoverride")
                        inListOverride = true, overrideListId = 0;
                    else if (word == "listid" && hasParam) {
                        if (inListOverride)
                            overrideListId = param;
                        else if (!rtfLists.empty())
                            rtfLists.back().id = param;
                    } else if (word == "ls" && hasParam && inListOverride)
                        listOverrides[param] = overrideListId;
                } else if (word == "ls" && hasParam && s.dest == Text) {
                    // the paragraph's list (its first level: nested levels not kept)
                    const auto o = listOverrides.find(param);
                    const long id = o == listOverrides.end() ? param : o->second;
                    for (const RtfList &l : rtfLists)
                        if (l.id == id)
                            s.style.para.textLists = {l.list};
                } else if (word == "stylesheet" ||
                         word == "expandedcolortbl" || word == "header" || word == "footer" || word == "pict" || word == "object" ||
                         word == "themedata" || word == "colorschememapping" || word == "latentstyles")
                    s.dest = Ignore;
                else if (word == "field")
                    fieldInstruction.clear();
                else if (word == "fldinst")
                    s.dest = FieldInstruction;
                else if (word == "fldrslt") {
                    // a HYPERLINK field's result: its text linked (NSRTFReader's NSLink)
                    const size_t h = fieldInstruction.find("HYPERLINK");
                    if (h != std::string::npos) {
                        std::string url = fieldInstruction.substr(h + 9);
                        const size_t q = url.find('"');
                        if (q != std::string::npos) {
                            const size_t e = url.find('"', q + 1);
                            url = url.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
                        }
                        while (!url.empty() && (url.front() == ' ' || url.front() == '\t'))
                            url.erase(0, 1);
                        while (!url.empty() && (url.back() == ' ' || url.back() == '\t'))
                            url.pop_back();
                        s.style.link = url;
                    }
                }
                else if (word == "NeXTGraphic")
                    s.dest = Graphic, graphicName.clear();
                else if (word == "width" && s.dest == Graphic)
                    graphicWidth = double(param);
                else if (word == "height" && s.dest == Graphic)
                    graphicHeight = double(param);
                else if (word == "red")
                    red = double(param), colourAuto = false;
                else if (word == "green")
                    green = double(param), colourAuto = false;
                else if (word == "blue")
                    blue = double(param), colourAuto = false;
                else if (word == "f") {
                    if (s.dest == FontTable)
                        fontNumber = int(param);
                    else if (auto f = fonts.find(int(param)); f != fonts.end()) {
                        Style named = Style::fromPostScriptName(f->second, st.size);
                        st.family = named.family;
                        st.face = named.face;
                        st.bold = named.bold;
                        st.italic = named.italic;
                    }
                } else if (word == "ansicpg")
                    codepage = int(param);
                else if (word == "mac")
                    codepage = 10000;
                else if (word == "fs")
                    st.size = std::max(1.0, param / 2.0);
                else if (word == "b")
                    st.bold = on;
                else if (word == "i")
                    st.italic = on;
                else if (word == "ul" || word == "uld" || word == "uldb" || word == "ulw" || word == "ulth")
                    st.underline = on;
                else if (word == "ulnone")
                    st.underline = false;
                else if (word == "strike")
                    st.strike = on;
                else if (word == "cf") {
                    if (param <= 0 || size_t(param) >= colours.size())
                        st.coloured = false;
                    else
                        st.coloured = true, st.colour = colours[size_t(param)];
                } else if (word == "super")
                    st.script = 1;
                else if (word == "sub")
                    st.script = -1;
                else if (word == "nosupersub")
                    st.script = 0;
                else if (word == "up")
                    st.baseline = hasParam ? param / 2.0 : 3;
                else if (word == "dn")
                    st.baseline = -(hasParam ? param / 2.0 : 3);
                else if (word == "plain") {
                    // the character formatting reset, the paragraph's kept
                    const Alignment a = st.align;
                    const ParagraphStyle ps = st.para;
                    st = Style();
                    if (auto f = fonts.find(0); f != fonts.end())
                        st = Style::fromPostScriptName(f->second, 12);
                    st.align = a;
                    st.para = ps;
                } else if (word == "pard") {
                    st.align = Alignment::Left;
                    st.para = ParagraphStyle();
                    tabKind = TabStop::Left;
                } else if (word == "tqc" || word == "tqr" || word == "tqdec")
                    tabKind = word == "tqc" ? TabStop::Center : word == "tqr" ? TabStop::Right : TabStop::Decimal;
                else if (word == "tx" && hasParam) {
                    st.para.hasTabs = true;
                    st.para.tabStops.push_back({param / 20.0, tabKind});
                    tabKind = TabStop::Left;
                } else if (word == "pardeftab" && hasParam)
                    st.para.defaultTabInterval = param / 20.0;
                else if (word == "li" && hasParam) {
                    // \fi is relative to \li, whichever comes first
                    const double fi = st.para.firstLineHeadIndent - st.para.headIndent;
                    st.para.headIndent = param / 20.0;
                    st.para.firstLineHeadIndent = st.para.headIndent + fi;
                } else if (word == "fi" && hasParam)
                    st.para.firstLineHeadIndent = st.para.headIndent + param / 20.0;
                else if (word == "ri" && hasParam)
                    st.para.tailIndent = -param / 20.0;
                else if (word == "sl" && hasParam) {
                    // alone, a line height: at least that much, exactly when negative
                    slValue = param;
                    st.para.minimumLineHeight = std::abs(param) / 20.0;
                    st.para.maximumLineHeight = param < 0 ? -param / 20.0 : 0;
                } else if (word == "slmult" && hasParam && param == 1 && slValue > 0) {
                    st.para.lineHeightMultiple = slValue / 240.0;
                    st.para.minimumLineHeight = st.para.maximumLineHeight = 0;
                } else if (word == "slminimum" && hasParam)
                    st.para.minimumLineHeight = param / 20.0;
                else if (word == "slmaximum" && hasParam)
                    st.para.maximumLineHeight = param / 20.0;
                else if (word == "slleading" && hasParam)
                    st.para.lineSpacing = param / 20.0;
                else if (word == "sb" && hasParam)
                    st.para.paragraphSpacingBefore = param / 20.0;
                else if (word == "sa" && hasParam)
                    st.para.paragraphSpacing = param / 20.0;
                else if (word == "ql")
                    st.align = Alignment::Left;
                else if (word == "qc")
                    st.align = Alignment::Center;
                else if (word == "qr")
                    st.align = Alignment::Right;
                else if (word == "qj")
                    st.align = Alignment::Justified;
                else if (word == "par" || word == "line")
                    put('\n');
                else if (word == "tab")
                    put('\t');
                else if (word == "uc")
                    s.uc = int(param);
                else if (word == "u") {
                    put(uint32_t(param < 0 ? param + 65536 : param));
                    stack.back().skip = s.uc;
                } else if (word == "emdash")
                    put(0x2014);
                else if (word == "endash")
                    put(0x2013);
                else if (word == "bullet")
                    put(0x2022);
                else if (word == "lquote")
                    put(0x2018);
                else if (word == "rquote")
                    put(0x2019);
                else if (word == "ldblquote")
                    put(0x201C);
                else if (word == "rdblquote")
                    put(0x201D);
            } else if (c == '\'') {
                if (i + 2 < n + 1) {
                    const unsigned b = unsigned(std::strtoul(rtf.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 3;
                    put(decodeByte((unsigned char)b, codepage));
                } else {
                    i = n;
                }
            } else if (c == '*') {
                stack.back().dest = stack.back().dest == Text ? Ignore : stack.back().dest;
                ++i;
            } else if (c == '\n' || c == '\r') {
                put('\n');
                ++i;
            } else if (c == '~') {
                put(0xA0);
                ++i;
            } else if (c == '_') {
                put(0x2011);
                ++i;
            } else if (c == '-') {
                ++i;   // an optional hyphen
            } else if (c == 'u' && false) {
                ++i;
            } else {
                put((unsigned char)c);   // \\ \{ \}
                ++i;
            }
        } else if (ch == '\n' || ch == '\r') {
            ++i;
        } else {
            put(decodeByte(ch, codepage));
            ++i;
        }
    }
    if (out.styles.empty())
        out.styles.push_back(stack[0].style);
    return out;
}

// ---- RTF, as Cocoa writes it

std::string toRTF(const RichText &t, const std::vector<double> &tabStops)
{
    std::vector<std::string> fonts;
    std::vector<AK::Color> colours = {AK::Color::rgb(255, 255, 255)};
    auto fontIndex = [&](const Style &s) {
        const std::string name = s.postScriptName();
        for (size_t i = 0; i < fonts.size(); ++i)
            if (fonts[i] == name)
                return int(i);
        fonts.push_back(name);
        return int(fonts.size() - 1);
    };
    auto rgb = [](const AK::Color &c) { return std::make_tuple(int(std::lround(c.r * 255)), int(std::lround(c.g * 255)), int(std::lround(c.b * 255))); };
    auto colourIndex = [&](const Style &s) {
        if (!s.coloured)
            return 0;
        for (size_t i = 0; i < colours.size(); ++i)
            if (rgb(colours[i]) == rgb(s.colour))
                return int(i) + 1;
        colours.push_back(s.colour);
        return int(colours.size());
    };
    // the tables first: every style the text uses
    for (uint16_t r : t.runs)
        if (r < t.styles.size()) {
            fontIndex(t.styles[r]);
            colourIndex(t.styles[r]);
        }
    if (fonts.empty())
        fontIndex(t.styles.empty() ? Style() : t.styles[0]);
    // the lists, in the order their paragraphs come (-[NSRTFWriter writeListTable] 0x2449f0)
    std::vector<TextList> lists;
    auto listIndex = [&](const TextList &l) {
        for (size_t i = 0; i < lists.size(); ++i)
            if (lists[i] == l)
                return i + 1;
        lists.push_back(l);
        return lists.size();
    };
    for (size_t i = 0; i <= t.text.size() && !t.text.empty();) {
        const Style &st = t.styleAt(std::min(i, t.text.size() - 1));
        if (!st.para.textLists.empty())
            listIndex(st.para.textLists.front());
        const size_t nl = t.text.find('\n', i);
        if (nl == std::string::npos)
            break;
        i = nl + 1;
    }
    // text for the RTF: ASCII as it is (the specials escaped), the rest \uc0\u
    auto rtfText = [](const std::string &str) {
        std::string out;
        for (size_t i = 0; i < str.size();) {
            const unsigned char c = (unsigned char)str[i];
            if (c == '\\' || c == '{' || c == '}') {
                out += '\\';
                out += char(c);
                ++i;
            } else if (c < 0x80) {
                out += char(c);
                ++i;
            } else {
                uint32_t cp = nextCodePoint(str, i);
                auto unit = [&](uint32_t u) { out += "\\uc0\\u" + std::to_string(u > 0x7FFF ? int(u) - 65536 : int(u)) + " "; };
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    unit(0xD800 + (cp >> 10));
                    unit(0xDC00 + (cp & 0x3FF));
                } else {
                    unit(cp);
                }
            }
        }
        return out;
    };

    std::string body;
    // -[NSRTFWriter writeParagraphStyle:]: \pard, the tab stops (their kind first), \pardeftab, \li,
    // \fi, \ri, the line height, \slleading, \sb, \sa, the alignment; points to twips as
    // floor(x × 20 + 0.01)
    auto twips = [](double pt) { return std::to_string(long(std::floor(pt * 20 + 0.01))); };
    auto paragraphWords = [&](const Style &s) {
        const ParagraphStyle &p = s.para;
        std::string w = "\\pard";
        std::vector<TabStop> stops = p.tabStops;
        if (!p.hasTabs) {
            stops.clear();
            for (double stop : tabStops)
                stops.push_back({stop, TabStop::Left});
        }
        for (const TabStop &t : stops)
            w += std::string(t.kind == TabStop::Center ? "\\tqc" : t.kind == TabStop::Right ? "\\tqr" : t.kind == TabStop::Decimal ? "\\tqdec" : "") +
                 "\\tx" + twips(t.location);
        if (p.defaultTabInterval > 0)
            w += "\\pardeftab" + twips(p.defaultTabInterval);
        if (p.headIndent != 0)
            w += "\\li" + twips(p.headIndent);
        if (p.firstLineHeadIndent != p.headIndent)
            w += "\\fi" + std::to_string(long(std::floor((p.firstLineHeadIndent - p.headIndent) * 20 + 0.01)));
        if (p.tailIndent < 0)
            w += "\\ri" + std::to_string(long(std::floor(0.01 - p.tailIndent * 20)));
        // -[NSRTFWriter writeParagraphStyle:] (0x2479f2): the multiple (12 points a line) with the
        // minimum and maximum after it; else the minimum alone, negative when it is the maximum too
        // (exactly); a maximum of 0 or 10000000 is none
        {
            const double mult = p.lineHeightMultiple, mn = p.minimumLineHeight, mx = p.maximumLineHeight;
            const bool noMax = mx == 0 || mx >= 10000000;
            if (mult > 0 && mult != 1) {
                const std::string sl = "\\sl" + std::to_string(long(std::floor(mult * 12 * 20 + 0.01))) + "\\slmult1";
                if (noMax)
                    w += mn > 0 ? sl + "\\slminimum" + twips(mn) : sl;
                else if (mn == mx)
                    w += "\\sl-" + twips(mn);
                else
                    w += sl + "\\slminimum" + twips(mn) + "\\slmaximum" + twips(mx);
            } else if (noMax) {
                if (mn > 0)
                    w += "\\sl" + twips(mn);
            } else if (mn == mx) {
                w += "\\sl-" + twips(mn);
            } else {
                w += "\\sl" + twips(mn) + "\\slmaximum" + twips(mx);
            }
        }
        if (p.lineSpacing > 0)
            w += "\\slleading" + twips(p.lineSpacing);
        if (p.paragraphSpacingBefore > 0)
            w += "\\sb" + twips(p.paragraphSpacingBefore);
        if (p.paragraphSpacing > 0)
            w += "\\sa" + twips(p.paragraphSpacing);
        w += s.align == Alignment::Center ? "\\qc" : s.align == Alignment::Right ? "\\qr" : s.align == Alignment::Justified ? "\\qj" : "\\ql\\qnatural";
        w += "\\pardirnatural";
        if (!p.textLists.empty())
            w += "\n\\ls" + std::to_string(listIndex(p.textLists.front())) + "\\ilvl0";
        return w;
    };
    bool first = true;
    Style cur;
    std::string link;          // the field open, its URL
    Style beforeLink;          // the attributes in force where it opened (its group's end restores them)
    bool paragraphStart = true;
    Alignment paraAlign = Alignment::Left;
    ParagraphStyle paraStyle;
    size_t attachment = 0;
    for (size_t i = 0; i < t.text.size();) {
        const Style &s = t.styleAt(i);
        if (!link.empty() && s.link != link) {
            // the field closed before a paragraph's words (its group's end would undo them)
            body += "}}";
            cur = beforeLink;
            paraAlign = beforeLink.align;   // the paragraph as it was outside the group
            paraStyle = beforeLink.para;
            link.clear();
        }
        if (paragraphStart && (first || s.align != paraAlign || s.para != paraStyle)) {
            body += std::string(first ? "" : "\n") + paragraphWords(s) + "\n";
            paraAlign = s.align;
            paraStyle = s.para;
        }
        const bool startsParagraph = paragraphStart;
        paragraphStart = false;
        // NSRTFWriter: a link's run in {\field{\*\fldinst{HYPERLINK "url"}}{\fldrslt …}}
        if (s.link != link) {
            link = s.link;
            if (!link.empty()) {
                std::string url;
                for (char ch : link) {
                    if (ch == '\\' || ch == '{' || ch == '}' || ch == '"')
                        url += '\\';
                    url += ch;
                }
                body += "{\\field{\\*\\fldinst{HYPERLINK \"" + url + "\"}}{\\fldrslt ";
                beforeLink = cur;
            }
        }
        if (first || s != cur) {
            std::string w;
            if (first || s.postScriptName() != cur.postScriptName())
                w += "\\f" + std::to_string(fontIndex(s));
            if (first || s.bold != cur.bold)
                w += s.bold ? "\\b" : (first ? "" : "\\b0");
            if (first || s.italic != cur.italic)
                w += s.italic ? "\\i" : (first ? "" : "\\i0");
            if (first || s.size != cur.size) {
                char fs[32];
                std::snprintf(fs, sizeof fs, "\\fs%d", int(std::lround(s.size * 2)));
                w += fs;
            }
            if (first || s.coloured != cur.coloured || rgb(s.colour) != rgb(cur.colour))
                w += std::string(w.empty() ? "" : " ") + "\\cf" + std::to_string(colourIndex(s));
            if (first ? s.underline : s.underline != cur.underline)
                w += s.underline ? "\\ul\\ulc0" : "\\ulnone";
            if (first ? s.strike : s.strike != cur.strike)
                w += s.strike ? "\\strike\\strikec0" : "\\strike0\\striked0";
            if (first ? s.script != 0 : s.script != cur.script)
                w += s.script > 0 ? "\\super" : s.script < 0 ? "\\sub" : "\\nosupersub";
            if (first ? s.baseline != 0 : s.baseline != cur.baseline) {
                const int half = int(std::lround(std::fabs(s.baseline) * 2));
                w += s.baseline >= 0 ? "\\up" + std::to_string(half) : "\\dn" + std::to_string(half);
            }
            if (!w.empty())
                body += (first ? "" : "\n") + w + " ";
            cur = s;
            first = false;
        }
        // a list item's marker ("\t•\t") in {\listtext …} (-[NSRTFWriter writeBody] 0x2466c2)
        if (startsParagraph && !s.para.textLists.empty() && t.text[i] == '\t') {
            const size_t end = t.text.find('\t', i + 1), nl = t.text.find('\n', i);
            if (end != std::string::npos && (nl == std::string::npos || end < nl)) {
                body += "{\\listtext" + rtfText(t.text.substr(i, end + 1 - i)) + "}";
                i = end + 1;
                continue;
            }
        }
        if (t.text.compare(i, 3, AttachmentCharacter) == 0) {
            const Attachment *a = attachment < t.attachments.size() ? &t.attachments[attachment] : nullptr;
            ++attachment;
            i += 3;
            if (!a)
                continue;
            body += "{{\\NeXTGraphic " + a->name + " \\width" + std::to_string(int(std::lround(a->width * 20))) + " \\height" +
                    std::to_string(int(std::lround(a->height * 20))) + "\n}\xAC}";
            continue;
        }
        const unsigned char c = (unsigned char)t.text[i];
        if (c == '\n') {
            body += "\\\n";
            paragraphStart = true;
            ++i;
        } else if (c == '\t') {
            body += "\\tab ";
            ++i;
        } else if (c == '\\' || c == '{' || c == '}') {
            body += '\\';
            body += char(c);
            ++i;
        } else if (c < 0x80) {
            body += char(c);
            ++i;
        } else {
            uint32_t cp = nextCodePoint(t.text, i);
            auto unit = [&](uint32_t u) { body += "\\uc0\\u" + std::to_string(u > 0x7FFF ? int(u) - 65536 : int(u)) + " "; };
            if (cp >= 0x10000) {
                cp -= 0x10000;
                unit(0xD800 + (cp >> 10));
                unit(0xDC00 + (cp & 0x3FF));
            } else {
                unit(cp);
            }
        }
    }
    if (!link.empty())
        body += "}}";

    std::string out = "{\\rtf1\\ansi\\ansicpg1252\\cocoartf1038\\cocoasubrtf360\n{\\fonttbl";
    for (size_t f = 0; f < fonts.size(); ++f)
        out += "\\f" + std::to_string(f) + "\\fnil\\fcharset0 " + fonts[f] + ";";
    out += "}\n{\\colortbl;";
    for (const AK::Color &c : colours) {
        auto [r, g, b] = rgb(c);
        out += "\\red" + std::to_string(r) + "\\green" + std::to_string(g) + "\\blue" + std::to_string(b) + ";";
    }
    out += "}\n";
    if (!lists.empty()) {
        // \levelnfc: 0 decimal, 1 upper-roman, 2 lower-roman, 3 upper-alpha, 4 lower-alpha, 23 the
        // rest; \leveltext's length, the prefix, the number's place (\'00), the suffix; a bullet's
        // own character
        out += "{\\*\\listtable";
        for (size_t k = 0; k < lists.size(); ++k) {
            const TextList &l = lists[k];
            const std::string f = l.markerFormat;
            const unsigned nfc = f.find("{decimal}") != std::string::npos ? 0
                                 : f.find("{upper-roman}") != std::string::npos ? 1
                                 : f.find("{lower-roman}") != std::string::npos ? 2
                                 : f.find("{upper-alpha}") != std::string::npos || f.find("{upper-latin}") != std::string::npos ? 3
                                 : f.find("{lower-alpha}") != std::string::npos || f.find("{lower-latin}") != std::string::npos ? 4
                                                                                                                    : 23;
            const std::string id = std::to_string(k + 1);
            out += "{\\list\\listtemplateid" + id + "\\listhybrid";
            out += "{\\listlevel\\levelnfc" + std::to_string(nfc) + "\\levelnfcn" + std::to_string(nfc) +
                   "\\leveljc0\\leveljcn0\\levelfollow0\\levelstartat" + std::to_string(l.start) + "\\levelspace360\\levelindent0";
            out += "{\\*\\levelmarker " + rtfText(f) + "}";
            if (l.options & 1)
                out += "\\levelprepend";
            out += "{\\leveltext\\leveltemplateid" + id;
            char hex[8];
            if (l.isOrdered()) {
                const std::string pre = l.prefix(), suf = l.suffix();
                std::snprintf(hex, sizeof hex, "\\'%02x", unsigned(pre.size() + suf.size() + 1) & 0xff);
                out += hex + rtfText(pre) + "\\'00" + rtfText(suf);
                std::snprintf(hex, sizeof hex, "\\'%02x", unsigned(pre.size() + 1) & 0xff);
                out += ";}{\\levelnumbers" + std::string(hex) + ";}";
            } else {
                out += "\\'01" + rtfText(l.marker(1)) + ";}{\\levelnumbers;}";
            }
            out += "\\fi-360\\li720\\lin720 }{\\listname ;}\\listid" + id + "}";
        }
        out += "}\n{\\*\\listoverridetable";
        for (size_t k = 0; k < lists.size(); ++k)
            out += "{\\listoverride\\listid" + std::to_string(k + 1) + "\\listoverridecount0\\ls" + std::to_string(k + 1) + "}";
        out += "}\n";
    }
    // the document's attributes (NSRTFWriter: \paperw11900\paperh16840\margl1440\margr1440\vieww9000\viewh8400\viewkind0)
    const DocumentAttributes &d = t.document;
    std::string attrs;
    if (d.paperWidth > 0 && d.paperHeight > 0)
        attrs += "\\paperw" + twips(d.paperWidth) + "\\paperh" + twips(d.paperHeight);
    if (d.left >= 0)
        attrs += "\\margl" + twips(d.left);
    if (d.right >= 0)
        attrs += "\\margr" + twips(d.right);
    if (d.top >= 0 && d.top != 72)
        attrs += "\\margt" + twips(d.top);
    if (d.bottom >= 0 && d.bottom != 72)
        attrs += "\\margb" + twips(d.bottom);
    if (d.viewWidth > 0 && d.viewHeight > 0)
        attrs += "\\vieww" + twips(d.viewWidth) + "\\viewh" + twips(d.viewHeight);
    if (d.viewKind >= 0)
        attrs += "\\viewkind" + std::to_string(d.viewKind);
    if (!attrs.empty())
        out += attrs + "\n";
    // the properties, as NSRTFWriter puts them: {\info {\title …}{\subject …}{\author …}{\*\company …}
    // {\*\copyright …}{\keywords …}{\doccomm …}}
    auto escaped = [](const std::string &v) {
        std::string e;
        for (size_t k = 0; k < v.size();) {
            const unsigned char ch = (unsigned char)v[k];
            if (ch == '\\' || ch == '{' || ch == '}') {
                e += '\\';
                e += char(ch);
                ++k;
            } else if (ch < 0x80) {
                e += char(ch);
                ++k;
            } else {
                // a UTF-8 character as \uN (no fallback: \uc0)
                const int len = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : 2;
                uint32_t cp = ch & (len == 4 ? 0x07 : len == 3 ? 0x0F : 0x1F);
                for (int m = 1; m < len && k + size_t(m) < v.size(); ++m)
                    cp = cp << 6 | ((unsigned char)v[k + size_t(m)] & 0x3F);
                k += size_t(len);
                e += "\\uc0\\u" + std::to_string(cp > 0x7FFF ? long(cp) - 0x10000 : long(cp)) + " ";
            }
        }
        return e;
    };
    std::string info;
    const std::pair<const char *, const std::string *> props[] = {
        {"title", &d.title}, {"subject", &d.subject}, {"author", &d.author}, {"*\\company", &d.company},
        {"*\\copyright", &d.copyright}, {"keywords", &d.keywords}, {"doccomm", &d.comment}};
    for (const auto &[word, value] : props)
        if (!value->empty())
            info += std::string("\n{\\") + word + " " + escaped(*value) + "}";
    if (!info.empty())
        out += "{\\info" + info + "}\n";
    if (t.text.empty())
        out += paragraphWords(t.styles.empty() ? Style() : t.styles[0]) + "\n\\f0\\fs" +
               std::to_string(int(std::lround((t.styles.empty() ? 12.0 : t.styles[0].size) * 2))) + " \\cf0 ";
    out += body + "}";
    return out;
}

// ---- RTFD: NSFileWrapper's serialized form

namespace {

uint32_t u32(const std::string &b, size_t at) { return at + 4 <= b.size() ? uint32_t((unsigned char)b[at]) | uint32_t((unsigned char)b[at + 1]) << 8 | uint32_t((unsigned char)b[at + 2]) << 16 | uint32_t((unsigned char)b[at + 3]) << 24 : 0; }

void put32(std::string &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b += char((v >> (8 * i)) & 0xff);
}

} // namespace

std::map<std::string, std::string> unflattenRTFD(const std::string &b)
{
    std::map<std::string, std::string> files;
    if (b.size() < 16 || b.compare(0, 4, "rtfd") != 0)
        return files;
    size_t p = 8;
    if (u32(b, p) != 3)
        return files;
    const uint32_t count = u32(b, p + 4);
    p += 8;
    std::vector<std::string> names;
    for (uint32_t i = 0; i < count && p + 4 <= b.size(); ++i) {
        const uint32_t len = u32(b, p);
        p += 4;
        if (p + len > b.size())
            return files;
        names.push_back(b.substr(p, len));
        p += len;
    }
    std::vector<uint32_t> sizes;
    for (uint32_t i = 0; i < count; ++i, p += 4)
        sizes.push_back(u32(b, p));
    for (uint32_t i = 0; i < names.size() && i < sizes.size(); ++i) {
        if (p + sizes[i] > b.size())
            break;
        const std::string entry = b.substr(p, sizes[i]);
        p += sizes[i];
        if (u32(entry, 0) == 1 && entry.size() >= 8 && names[i] != ".")
            files[names[i]] = entry.substr(8, std::min<size_t>(u32(entry, 4), entry.size() - 8));
    }
    return files;
}

std::string flattenRTFD(const std::map<std::string, std::string> &files)
{
    // TXT.rtf first, the pictures, then "." (their attributes)
    std::vector<std::string> names;
    if (files.count("TXT.rtf"))
        names.push_back("TXT.rtf");
    for (const auto &f : files)
        if (f.first != "TXT.rtf" && f.first != ".")
            names.push_back(f.first);
    std::string attrs;
    put32(attrs, uint32_t(names.size()));
    for (const std::string &n : names) {
        put32(attrs, uint32_t(n.size()));
        attrs += n;
    }
    for (size_t i = 0; i < names.size(); ++i)
        put32(attrs, 16);
    for (size_t i = 0; i < names.size(); ++i) {
        put32(attrs, 0);       // modification time
        put32(attrs, 0644);    // permissions
        put32(attrs, 0);
        put32(attrs, 0);
    }
    std::vector<std::pair<std::string, std::string>> entries;
    for (const std::string &n : names)
        entries.push_back({n, files.at(n)});
    entries.push_back({".", attrs});

    std::string out = "rtfd";
    put32(out, 0);
    put32(out, 3);
    put32(out, uint32_t(entries.size()));
    for (const auto &e : entries) {
        put32(out, uint32_t(e.first.size()));
        out += e.first;
    }
    for (const auto &e : entries)
        put32(out, uint32_t(e.second.size() + 8));
    for (const auto &e : entries) {
        put32(out, 1);
        put32(out, uint32_t(e.second.size()));
        out += e.second;
    }
    return out;
}

RichText fromRTFD(const std::string &bytes)
{
    if (bytes.compare(0, 5, "{\\rtf") == 0)
        return fromRTF(bytes);
    const std::map<std::string, std::string> files = unflattenRTFD(bytes);
    auto rtf = files.find("TXT.rtf");
    if (rtf == files.end())
        return RichText();
    return fromRTF(rtf->second, files);
}

std::string toRTFD(const RichText &t)
{
    std::map<std::string, std::string> files;
    files["TXT.rtf"] = toRTF(t);
    for (const Attachment &a : t.attachments)
        if (!a.name.empty() && !a.data.empty())
            files[a.name] = a.data;
    return flattenRTFD(files);
}

RichText fromPlainText(const std::string &bytes, const Style &style)
{
    std::string text;
    if (validUtf8(bytes)) {
        text = bytes;
    } else {
        for (unsigned char c : bytes)
            appendUtf8(text, decodeByte(c, 10000));
    }
    std::string clean;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            clean += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        } else {
            clean += text[i];
        }
    }
    RichText r;
    r.insert(0, clean, style);
    if (r.styles.empty())
        r.styles.push_back(style);
    return r;
}

std::string *DocumentAttributes::property(const std::string &w)
{
    return w == "title" ? &title : w == "subject" ? &subject : w == "author" ? &author : w == "company" ? &company
         : w == "copyright" ? &copyright : w == "keywords" ? &keywords : w == "doccomm" ? &comment : nullptr;
}

} // namespace AK
