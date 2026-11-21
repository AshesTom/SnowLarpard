// SPDX-License-Identifier: LGPL-2.1-or-later
// AppKit's NSAttributedString additions (NSAttributedStringKitAdditions): rich text in RTF and RTFD,
// as Stickies' notes (-[Document RTFDData]) and TextEdit's documents hold it, read and written as
// Cocoa's RTF reader and writer do: fonts by their PostScript name
// (Helvetica-Bold, LucidaGrande, MarkerFelt-Wide), sizes, bold and italic, underline and strike
// through, colours, super- and subscripts, the paragraphs' styles (alignment, indents, tab stops,
// line and paragraph spacing: -[NSRTFWriter writeParagraphStyle:]), and pictures (NSTextAttachment,
// RTFD's files, \NeXTGraphic in the RTF).
//
// The flattened RTFD (-[NSAttributedString RTFDFromRange:documentAttributes:], NSFileWrapper's
// serialized form, as the welcome notes hold it):
//   "rtfd", u32 0, then a directory: u32 3, u32 n, n names (u32 length, bytes), n sizes (u32), the n
//   entries; a file entry u32 1, u32 length, its bytes. The names: TXT.rtf, the pictures, and "." (the
//   files' attributes: u32 count, their names, a u32 16 each, then 16 bytes each: u32 modification
//   time, u32 POSIX permissions, 8 zero bytes)
#pragma once

#include "graphics.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace AK {

enum class Alignment { Left, Center, Right, Justified };

// NSTextTab: where (points from the line fragment's start, padding in) and how the text lines up
struct TabStop {
    enum Kind { Left, Center, Right, Decimal };   // \tx, \tqc, \tqr, \tqdec
    double location = 0;
    Kind kind = Left;
    bool operator==(const TabStop &o) const { return location == o.location && kind == o.kind; }
};

// NSParagraphStyle besides the alignment, in points (the RTF's twips over 20):
//   firstLineHeadIndent, headIndent   from the container's start (\li the head, \fi first less head)
//   tailIndent                        > 0 from the start, <= 0 from the end (\ri its opposite)
//   tabStops                          \tx..., given (hasTabs) or the text's defaults; then every
//                                     defaultTabInterval (\pardeftab)
//   lineHeightMultiple                \sl (240 a line) \slmult1; lineSpacing \slleading
//   minimumLineHeight, maximumLineHeight   \sl alone (at least; negative: exactly), \slminimum, \slmaximum
//   paragraphSpacingBefore, paragraphSpacing   \sb, \sa
struct ParagraphStyle {
    double firstLineHeadIndent = 0, headIndent = 0, tailIndent = 0;
    bool hasTabs = false;
    std::vector<TabStop> tabStops;
    double defaultTabInterval = 0;
    double lineHeightMultiple = 0, lineSpacing = 0;
    double minimumLineHeight = 0, maximumLineHeight = 0;   // points, 0: none
    double paragraphSpacingBefore = 0, paragraphSpacing = 0;
    bool operator==(const ParagraphStyle &o) const;
    bool operator!=(const ParagraphStyle &o) const { return !(*this == o); }
};

struct Style {
    std::string family = "Helvetica";   // the font's family ("Lucida Grande", "Marker Felt")
    std::string face;                    // a face besides bold and italic ("Wide", "Thin")
    bool bold = false, italic = false;
    double size = 12;
    bool underline = false, strike = false;
    bool coloured = false;               // \cf other than 0: the colour below, else the text's default (black)
    AK::Color colour = AK::Color::gray(0);
    int script = 0;                      // 1 superscript, -1 subscript (\super, \sub)
    double baseline = 0;                 // points up (\up, \dn: Raise, Lower)
    Alignment align = Alignment::Left;   // its paragraph's
    ParagraphStyle para;                 // its paragraph's (the first character's decides)
    std::string link;                    // NSLinkAttributeName: the URL ({\field{\*\fldinst{HYPERLINK "…"}}{\fldrslt …}})

    bool operator==(const Style &o) const;
    bool operator!=(const Style &o) const { return !(*this == o); }
    // the PostScript name Cocoa writes in the font table, and back
    std::string postScriptName() const;
    static Style fromPostScriptName(const std::string &name, double size);
};

struct Attachment {
    std::string name;    // its file in the RTFD
    std::string data;
    double width = 0, height = 0;   // points (\width, \height in twips)
    AK::Image image;     // decoded when drawn
};

// U+FFFC stands for each attachment in the text, the n-th for attachments[n]
constexpr const char *AttachmentCharacter = "\xEF\xBF\xBC";

// The document's attributes, as NSRTFReader gives them and NSRTFWriter writes them after the tables:
// the paper (\paperw \paperh, NSPaperSizeDocumentAttribute) and its margins (\margl \margr \margt
// \margb), the view's size (\vieww \viewh) and mode (\viewkind: 1 the pages, NSViewModeDocumentAttribute),
// in points (twips in the file); 0 or -1: not given
struct DocumentAttributes {
    double paperWidth = 0, paperHeight = 0;
    double left = -1, right = -1, top = -1, bottom = -1;
    double viewWidth = 0, viewHeight = 0;
    int viewKind = -1;
    // the properties (NSTitleDocumentAttribute, NSSubjectDocumentAttribute, NSAuthorDocumentAttribute,
    // NSCompanyDocumentAttribute, NSCopyrightDocumentAttribute, NSKeywordsDocumentAttribute,
    // NSCommentDocumentAttribute): the \info group's \title, \subject, \author, \*\company,
    // \*\copyright, \keywords, \doccomm
    std::string title, subject, author, company, copyright, keywords, comment;
    std::string *property(const std::string &rtfWord);
};

struct RichText {
    DocumentAttributes document;
    std::string text;                 // UTF-8, paragraphs ended by '\n'
    std::vector<uint16_t> runs;       // the style of each byte of text, an index into styles
    std::vector<Style> styles;
    std::vector<Attachment> attachments;

    size_t size() const { return text.size(); }
    const Style &styleAt(size_t byte) const;
    uint16_t index(const Style &s);   // the style's index (added if new)
    void insert(size_t at, const std::string &s, const Style &style);
    void erase(size_t from, size_t to);
    RichText slice(size_t from, size_t to) const;
    void insert(size_t at, const RichText &other);
    void setStyle(size_t from, size_t to, const std::function<void(Style &)> &change);
    void compact();   // drops the styles no byte uses
    size_t attachmentIndex(size_t byte) const;   // which attachment the U+FFFC at `byte` is
};

RichText fromRTF(const std::string &rtf, const std::map<std::string, std::string> &files = {});
// The paragraphs' tab stops in points (\txN, in twips): Stickies' twelve 28 points apart by default
std::string toRTF(const RichText &t, const std::vector<double> &tabStops = {28, 56, 84, 112, 140, 168, 196, 224, 252, 280, 308, 336});
std::map<std::string, std::string> unflattenRTFD(const std::string &bytes);
std::string flattenRTFD(const std::map<std::string, std::string> &files);
RichText fromRTFD(const std::string &bytes);
std::string toRTFD(const RichText &t);
// Plain text, a file's (as Import Text… reads one): UTF-8, else Mac Roman
RichText fromPlainText(const std::string &bytes, const Style &style);

} // namespace AK
