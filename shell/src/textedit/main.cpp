// SPDX-License-Identifier: LGPL-2.1-or-later
// TextEdit.app (1.6, 264) on AquaKit. Reversed from TextEdit's x86_64 slice (not protected; the same
// binary on 10.6 and on 10.6.8): its defaults, its documents (rich and plain text, reading and saving
// by type, the encodings), the window's size from the text's font, Make Plain Text / Make Rich Text,
// the transient Untitled document, Prevent Editing, the menus' validation (docs/reverse-status.md,
// section 18). Its nibs and strings, and NSDocument's (AppKit's), are the DVD's, read at run time.
//   TextEdit [file...]
#include "appkit.h"
#include "attributedstring.h"
#include "controls.h"
#include "foundation.h"
#include "mainmenu.h"
#include "nib.h"
#include "panels.h"
#include "rulerview.h"
#include "text.h"
#include "textview.h"

#include <cairo-pdf.h>
#include <glib.h>
#include <iconv.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <langinfo.h>
#include <locale.h>
#include <map>
#include <memory>
#include <optional>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace AK;

namespace TextEdit {

namespace {

const char *const Bundle = "/Applications/TextEdit.app";
const char *const AppKitBundle = "/System/Library/Frameworks/AppKit.framework/Versions/C";

// The Command keys TextEdit's menus answer that WindowServer would otherwise take: W (Close asks
// to save first) and Q (Quit reviews the unsaved documents)
const std::vector<uint32_t> OwnKeys = {XKB_KEY_w, XKB_KEY_q};

std::string res(const std::string &name) { return Assets::path(std::string(Bundle) + "/Contents/Resources/" + name); }
std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }
// NSDocument's strings (AppKit's Document.strings), keyed as the file has them ("Don\\U2019t Save")
std::string docString(const std::string &key, const std::string &fallback) { return Assets::tr(AppKitBundle, "Document", key, fallback); }
std::string findString(const std::string &key) { return Assets::tr(AppKitBundle, "FindPanel", key, key); }

// "%@" (or "%1$@", "%2$@") replaced by the arguments in turn
std::string format(std::string s, const std::vector<std::string> &args)
{
    size_t next = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string numbered = "%" + std::to_string(i + 1) + "$@";
        size_t p = s.find(numbered);
        size_t len = numbered.size();
        if (p == std::string::npos) {
            p = s.find("%@", next);
            len = 2;
        }
        if (p == std::string::npos)
            break;
        s.replace(p, len, args[i]);
        next = p + args[i].size();
    }
    return s;
}

// The menu titles' "&" (Windows' mnemonics in TextEdit's strings) taken out
std::string menuTitle(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '&'), s.end());
    return s;
}

std::string readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}

bool writeFile(const std::string &path, const std::string &bytes)
{
    const std::string tmp = path + ".TextEdit-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(bytes.data(), std::streamsize(bytes.size()));
        if (!f)
            return false;
    }
    // the file's permissions kept; written whole first, then put in its place (writeSafelyToURL:)
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        chmod(tmp.c_str(), st.st_mode & 07777);
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension(const std::string &path)
{
    std::string name = fileName(path);
    while (!name.empty() && name.back() == '/')
        name.pop_back();
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
}

// ---- the defaults: +[Controller initialize] registers them (0x10000113f); the user's in textedit.conf

const std::map<std::string, std::string> &registeredDefaults()
{
    static const std::map<std::string, std::string> d = {
        {"AutosaveDelay", "30"}, {"NumberPagesWhenPrinting", "false"}, {"DeleteBackup", "true"}, {"RichText", "true"},
        {"ShowPageBreaks", "false"}, {"OpenPanelFollowsMainWindow", "false"}, {"AddExtensionToNewPlainTextFiles", "true"},
        {"WidthInChars", "75"}, {"HeightInChars", "30"}, {"PlainTextEncoding", "4294967295"},
        {"PlainTextEncodingForWrite", "4294967295"}, {"TabWidth", "8"}, {"ForegroundLayoutToIndex", "50000"},
        {"IgnoreRichText", "false"}, {"IgnoreHTML", "false"}, {"CheckSpellingWhileTyping", "true"},
        {"CheckGrammarWithSpelling", "false"}, {"CorrectSpellingAutomatically", "true"}, {"ShowRuler", "true"},
        {"SmartCopyPaste", "true"}, {"SmartQuotes", "false"}, {"SmartDashes", "false"}, {"SmartLinks", "false"},
        {"DataDetectors", "false"}, {"TextReplacement", "true"}, {"SubstitutionsEnabledInRichTextOnly", "false"},
        {"author", ""}, {"company", ""}, {"copyright", ""}, {"UseXHTMLDocType", "false"}, {"UseTransitionalDocType", "false"},
        {"UseEmbeddedCSS", "true"}, {"UseInlineCSS", "false"}, {"HTMLEncoding", "4"}, {"PreserveWhitespace", "true"},
        // NSUserFont and NSUserFixedPitchFont (AppKit's, by name and size)
        {"NSFont", "Helvetica"}, {"NSFontSize", "12"}, {"NSFixedPitchFont", "Monaco"}, {"NSFixedPitchFontSize", "10"},
    };
    return d;
}

Settings &settings()
{
    static Settings s("textedit");
    return s;
}

std::string pref(const std::string &key)
{
    auto it = registeredDefaults().find(key);
    return settings().string(key, it == registeredDefaults().end() ? std::string() : it->second);
}
bool prefBool(const std::string &key) { return pref(key) == "true" || pref(key) == "1"; }
long long prefInt(const std::string &key) { return std::atoll(pref(key).c_str()); }

// -[NSFont userFontOfSize:0], -[NSFont userFixedPitchFontOfSize:0]
Style userFont(bool fixedPitch)
{
    Style s;
    s.family = pref(fixedPitch ? "NSFixedPitchFont" : "NSFont");
    s.size = std::atof(pref(fixedPitch ? "NSFixedPitchFontSize" : "NSFontSize").c_str());
    if (s.size <= 0)
        s.size = fixedPitch ? 10 : 12;
    return s;
}

Font fontOf(const Style &s) { return Font{s.size, s.bold, s.family}; }

// ---- the encodings TextEdit offers (NSStringEncoding, iconv's name, -[NSString localizedNameOfStringEncoding:])

constexpr unsigned long long Automatic = 0xffffffffULL;   // NoStringEncoding: TextEdit's "Automatic"

struct Encoding {
    unsigned long long ns;
    const char *iconv;
    const char *name;
};
const std::vector<Encoding> &encodings()
{
    static const std::vector<Encoding> e = {
        {4, "UTF-8", "Unicode (UTF-8)"},
        {10, "UTF-16", "Unicode (UTF-16)"},
        {0x90000100ULL, "UTF-16BE", "Unicode (UTF-16BE)"},
        {0x94000100ULL, "UTF-16LE", "Unicode (UTF-16LE)"},
        {30, "MACINTOSH", "Western (Mac OS Roman)"},
        {5, "ISO-8859-1", "Western (ISO Latin 1)"},
        {12, "CP1252", "Western (Windows Latin 1)"},
        {1, "ASCII", "Western (ASCII)"},
    };
    return e;
}
const Encoding *encoding(unsigned long long ns)
{
    for (const Encoding &e : encodings())
        if (e.ns == ns)
            return &e;
    return nullptr;
}

// bytes from one encoding to another; nullopt when a character can't be converted
std::optional<std::string> convert(const std::string &in, const char *from, const char *to)
{
    iconv_t cd = iconv_open(to, from);
    if (cd == iconv_t(-1))
        return std::nullopt;
    std::string out;
    out.resize(in.size() * 4 + 16);
    char *src = const_cast<char *>(in.data());
    size_t left = in.size();
    char *dst = out.data();
    size_t room = out.size();
    bool ok = true;
    while (left > 0) {
        if (iconv(cd, &src, &left, &dst, &room) == size_t(-1)) {
            if (errno == E2BIG) {
                const size_t used = size_t(dst - out.data());
                out.resize(out.size() * 2);
                dst = out.data() + used;
                room = out.size() - used;
                continue;
            }
            ok = false;
            break;
        }
    }
    iconv(cd, nullptr, nullptr, &dst, &room);
    iconv_close(cd);
    if (!ok)
        return std::nullopt;
    out.resize(size_t(dst - out.data()));
    return out;
}

// Plain text's encoding, automatic: a byte order mark, else UTF-8 when it is, else the default C
// string encoding (Mac OS Roman)
unsigned long long sniffEncoding(const std::string &bytes)
{
    if (bytes.size() >= 2 && ((uint8_t(bytes[0]) == 0xFF && uint8_t(bytes[1]) == 0xFE) || (uint8_t(bytes[0]) == 0xFE && uint8_t(bytes[1]) == 0xFF)))
        return 10;
    if (g_utf8_validate(bytes.data(), gssize(bytes.size()), nullptr))
        return 4;
    return 30;
}

// ---- the document's types (Info.plist's CFBundleDocumentTypes, by the UTIs TextEdit maps:
// -[Document textDocumentTypeToTextEditDocumentTypeMappingTable])

enum class Type { RTF, RTFD, Plain };

// A type TextEdit reads that this one can't: the format's name for the error (Word, OpenDocument,
// web archives; HTML is shown as its source, as with Ignore rich text commands)
std::string unreadableFormat(const std::string &ext)
{
    if (ext == "doc")
        return "Word 97 Format (doc)";
    if (ext == "docx")
        return "Word 2007 Format (docx)";
    if (ext == "odt")
        return "OpenDocument Text (odt)";
    if (ext == "webarchive")
        return "Web Archive";
    if (ext == "wordml")
        return "Word 2003 XML Format";
    return {};
}

class Controller;

// A panel's keys (Find, Preferences, About): ⌘W closes it (performClose: goes to the key window),
// the others are the menus'
std::function<void(Event &)> panelKeys(Window *w, MainMenu &menu)
{
    return [w, &menu](Event &e) {
        if ((e.modifiers & Command) && (e.keysym == XKB_KEY_w || e.keysym == XKB_KEY_W)) {
            w->close();
            return;
        }
        menu.performKeyEquivalent(e);
    };
}

// ---- the find panel: AppKit's NSFindPanel.nib (NSTextFinder's), on the main document's text

class FindPanel {
public:
    explicit FindPanel(Controller &c) : m_c(c) {}
    void show();
    // performFindPanelAction:'s tags: 1 show, 2 next, 3 previous, 4 replace all, 5 replace,
    // 6 replace and find, 7 set the find string from the selection
    void perform(int tag);
    bool hasFindString() const { return !findText().empty(); }
    Window *window() const { return m_window.get(); }

private:
    void build();
    std::string findText() const { return m_find ? m_find->text : m_findString; }
    bool find(bool forward);
    void status(const std::string &s)
    {
        if (m_status)
            m_status->setText(s);
    }
    Controller &m_c;
    std::unique_ptr<Window> m_window;
    Nib m_nib;
    TextField *m_find = nullptr, *m_replace = nullptr;
    CheckBox *m_ignoreCase = nullptr, *m_wrap = nullptr;
    Label *m_status = nullptr;
    std::string m_findString;
};

// ---- a document: Document (the text and its properties) with its DocumentWindowController

class Document {
public:
    Document(Controller &c, int untitled);
    ~Document();

    // -[Document readFromURL:ofType:error:]: false and a message when it can't
    bool read(const std::string &path, std::string *error);
    void show(std::optional<Point> at);
    Window *window() const { return m_window.get(); }
    TextView *textView() const { return m_scroll->textView(); }
    const std::string &path() const { return m_path; }

    bool isRichText() const { return m_type != Type::Plain; }
    bool isEdited() const { return m_edited; }
    bool isReadOnly() const { return m_readOnly; }
    bool isTransient() const { return m_transient; }
    void setTransient(bool t) { m_transient = t; }
    // -[Document isTransientAndCanBeReplaced]: transient, without a sheet
    bool isTransientAndCanBeReplaced() const { return m_transient && !Sheet::isOpen(m_window.get()); }
    std::string displayName() const;
    bool isMain() const { return m_window && m_window->isKey(); }

    bool validate(const MainMenuItem &it, MenuValidation &v);
    void perform(const MainMenuItem &it);
    // Close: the changes asked about first (the sheet); done(true) once closed
    void performClose(std::function<void(bool)> done = {});
    // -[Document setValue:forDocumentProperty:]: a property of the text (its attributes, written in
    // the RTF's \info), the document edited
    std::string property(const std::string &key) const
    {
        std::string *p = const_cast<TextView *>(textView())->text().document.property(key == "comment" ? "doccomm" : key);
        return p ? *p : std::string();
    }
    void setProperty(const std::string &key, const std::string &value)
    {
        std::string *p = textView()->text().document.property(key == "comment" ? "doccomm" : key);
        if (!p || *p == value)
            return;
        *p = value;
        setEdited(true);
    }
    void save(bool as, std::function<void(bool)> done = {});
    void print();
    void saveAsPDF();

private:
    void build();
    void updateTitle();
    void setEdited(bool e);
    void textChanged();
    // -[DocumentWindowController setupWindowForDocument] (0x10000356e): the text's size in characters
    Size contentSizeForDefaults() const;
    // -[Document defaultTextAttributes:] (0x1000039f2) given to the view: typing, tab stops
    void applyTextDefaults();
    // -[Document applyDefaultTextAttributes:] (0x1000062a2): the whole text in the default attributes
    void applyDefaultAttributesToText();
    void setRichText(bool rich);
    void toggleRich();
    void doToggleRich();
    bool toggleRichWillLoseInformation() const;
    void toggleReadOnly();
    // -[DocumentWindowController updateForRichTextAndRulerState] (0x100003dee): the ruler for rich
    // text, shown when ShowRuler says so and the document is editable (showRulerDelayed:); hidden
    // for plain text
    void updateRulerState();
    bool m_rulerShown = false, m_rulerDecided = false;
    // -[Document setHasMultiplePages:] (Wrap to Page): the window controller's MultiplePageView
    // (-[DocumentWindowController setHasMultiplePages:force:] 0x1000028b7), the selection in sight
    void setHasMultiplePages(bool pages);
    bool m_hasMultiplePages = false;
    // -[Document printInfo] (0x10000207a): the shared print info's paper, the margins 72 points
    TextView::PageGeometry m_paper;
    // NSViewSizeDocumentAttribute: the view's size the file was saved with (none: 0 x 0)
    Size m_viewSize{0, 0};
    bool writeTo(const std::string &path, Type type, std::string *error);
    void close();

    Controller &m_c;
    std::unique_ptr<Window> m_window;
    TextScrollView *m_scroll = nullptr;
    std::string m_path;
    int m_untitled = 0;
    Type m_type = Type::RTF;
    unsigned long long m_encoding = Automatic;          // the file's, when plain text
    unsigned long long m_encodingForSaving = Automatic;
    bool m_edited = false, m_readOnly = false, m_transient = false, m_openedIgnoringRichText = false;
    bool m_closing = false;
    // Undo Make Plain Text / Make Rich Text: the text and the type before (the next edit forgets it)
    struct ToggleUndo {
        RichText text;
        Type type;
        bool redo = false;
    };
    std::optional<ToggleUndo> m_toggleUndo;
};

// ---- the controller: NSApplication's delegate (Controller) and the DocumentController

class Controller {
public:
    Controller();
    void start(const std::vector<std::string> &files);
    void openFiles(const std::vector<std::string> &paths);
    Document *newDocument(bool transient = false);
    Document *mainDocument() const;
    std::vector<Document *> documents() const { return m_order; }
    void documentBecameMain(Document *d);
    void documentClosed(Document *d);
    void documentsChanged() { updateWindowsMenu(); }
    void noteRecent(const std::string &path);
    MainMenu &menu() { return m_menu; }
    FindPanel &findPanel() { return m_find; }
    Image appIcon() const { return m_icon; }
    std::optional<Point> nextCascadePoint();
    void quit();
    void showPreferences();
    void showLinePanel();
    void toggleProperties();
    void refreshProperties();   // -activeDocumentChanged

private:
    bool validate(const MainMenuItem &it, MenuValidation &v);
    void perform(const MainMenuItem &it);
    void openPanel();
    void updateWindowsMenu();
    void updateRecentMenu();
    void showAbout();
    void reviewAndQuit(size_t from);

    MainMenu m_menu;
    FindPanel m_find{*this};
    std::vector<std::unique_ptr<Document>> m_documents;
    std::vector<Document *> m_order;   // front to back
    std::vector<int> m_untitledUsed;
    size_t m_staticWindowItems = 0;
    Image m_icon;
    std::unique_ptr<Window> m_about, m_prefs, m_linePanel, m_properties;
    TextField *m_lineField = nullptr;
    std::map<std::string, TextField *> m_propertyFields;
    Nib m_prefsNib;
    std::optional<Point> m_lastCascade;
    std::optional<Style> m_copiedStyle;
};

// ---- Document

namespace {

// +[NSPrintInfo sharedPrintInfo]'s paper: the region's (here the locale's LC_PAPER: A4, US Letter...)
TextView::PageGeometry sharedPaper()
{
    TextView::PageGeometry g;   // US Letter, 612 x 792
    if (locale_t l = newlocale(LC_PAPER_MASK, "", locale_t(0))) {
        const unsigned w = unsigned(uintptr_t(nl_langinfo_l(_NL_PAPER_WIDTH, l)));
        const unsigned h = unsigned(uintptr_t(nl_langinfo_l(_NL_PAPER_HEIGHT, l)));
        freelocale(l);
        if (w == 210 && h == 297) {
            g.paperWidth = 595;   // the printers' A4: 595 x 842
            g.paperHeight = 842;
        } else if (w > 0 && h > 0 && w < 2000 && h < 2000 && !(w == 216 && h == 279)) {
            g.paperWidth = std::round(w * 72 / 25.4);
            g.paperHeight = std::round(h * 72 / 25.4);
        }
    }
    return g;
}

} // namespace

Document::Document(Controller &c, int untitled) : m_c(c), m_untitled(untitled)
{
    // -[Document init] (0x100001a43): white, encodings automatic, the default type by RichText
    // (-[DocumentController defaultType]), the window wrapped to the window unless ShowPageBreaks
    m_type = prefBool("RichText") ? Type::RTF : Type::Plain;
    m_hasMultiplePages = prefBool("ShowPageBreaks");
    m_paper = sharedPaper();
}

Document::~Document() = default;

std::string Document::displayName() const
{
    // NSDocument's: the file's name, else "Untitled", "Untitled 2"...
    if (!m_path.empty()) {
        std::string name = fileName(m_path);
        while (!name.empty() && name.back() == '/')
            name.pop_back();
        return name;
    }
    if (m_untitled <= 1)
        return docString("Untitled", "Untitled");
    return format(docString("Untitled %@", "Untitled %@"), {std::to_string(m_untitled)});
}

bool Document::read(const std::string &path, std::string *error)
{
    // -[Document readFromURL:ofType:encoding:ignoreRTF:ignoreHTML:error:] (0x100005348): the type by
    // the file, RTF and HTML as plain text when the preferences ignore their commands; plain text in
    // its encoding (PlainTextEncoding, else found), in the plain text font
    const std::string ext = extension(path);
    const bool dir = isDir(path);
    if (const std::string unreadable = unreadableFormat(ext); !unreadable.empty()) {
        *error = format(docString("%@ cannot open files in the \\U201C%@\\U201D format.", "%1$@ cannot open files in the “%2$@” format."),
                        {"TextEdit", unreadable});
        return false;
    }
    const bool rtf = ext == "rtf" || ext == "rtfd" || dir;
    const bool html = ext == "html" || ext == "htm";
    RichText text;
    if (rtf && !prefBool("IgnoreRichText")) {
        if (dir) {
            // RTFD: a folder, TXT.rtf and its pictures
            std::map<std::string, std::string> files;
            if (DIR *d = opendir(path.c_str())) {
                while (dirent *e = readdir(d)) {
                    const std::string n = e->d_name;
                    if (n != "." && n != ".." && n != "TXT.rtf" && isFile(path + "/" + n))
                        files[n] = readFile(path + "/" + n);
                }
                closedir(d);
            }
            const std::string rtfText = readFile(path + "/TXT.rtf");
            if (rtfText.empty()) {
                *error = "The file couldn’t be opened because it isn’t in the correct format.";
                return false;
            }
            text = fromRTF(rtfText, files);
            m_type = Type::RTFD;
        } else {
            const std::string bytes = readFile(path);
            if (bytes.compare(0, 4, "rtfd") == 0) {
                text = fromRTFD(bytes);
                m_type = Type::RTFD;
            } else {
                text = fromRTF(bytes);
                m_type = Type::RTF;
            }
        }
        if (text.styles.empty())
            text.styles.push_back(userFont(false));
        // -[Document readFromURL:ofType:error:] (0x1000059e5): the view mode (1: the pages, else
        // wrapped to the window), the margins and the paper given, the view's size
        const DocumentAttributes &d = text.document;
        m_hasMultiplePages = d.viewKind == 1;
        if (d.left >= 0)
            m_paper.left = d.left;
        if (d.right >= 0)
            m_paper.right = d.right;
        if (d.top >= 0)
            m_paper.top = d.top;
        if (d.bottom >= 0)
            m_paper.bottom = d.bottom;
        if (d.paperWidth > 0 && d.paperHeight > 0) {
            m_paper.paperWidth = d.paperWidth;
            m_paper.paperHeight = d.paperHeight;
        }
        if (d.viewWidth > 0 && d.viewHeight > 0)
            m_viewSize = {d.viewWidth, d.viewHeight};
    } else {
        if (dir) {
            *error = "The file couldn’t be opened because it isn’t in the correct format.";
            return false;
        }
        const std::string bytes = readFile(path);
        if (bytes.empty() && access(path.c_str(), R_OK) != 0) {
            *error = "The file couldn’t be opened because you don’t have permission to view it.";
            return false;
        }
        unsigned long long enc = static_cast<unsigned long long>(prefInt("PlainTextEncoding"));
        if (enc == Automatic || !encoding(enc))
            enc = sniffEncoding(bytes);
        std::optional<std::string> utf8 = convert(bytes, encoding(enc)->iconv, "UTF-8");
        if (!utf8) {
            enc = 30;   // Mac OS Roman reads any byte
            utf8 = convert(bytes, "MACINTOSH", "UTF-8");
        }
        std::string s = utf8 ? *utf8 : std::string();
        if (s.compare(0, 3, "\xEF\xBB\xBF") == 0)
            s.erase(0, 3);
        // the line ends as NSString reads them: CR LF and CR are paragraph ends
        std::string norm;
        norm.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r') {
                norm += '\n';
                if (i + 1 < s.size() && s[i + 1] == '\n')
                    ++i;
            } else {
                norm += s[i];
            }
        }
        text = fromPlainText(norm, userFont(true));
        m_type = Type::Plain;
        m_hasMultiplePages = false;   // no view mode: wrapped to the window
        m_encoding = enc;
        m_openedIgnoringRichText = rtf || html;
    }
    m_path = path;
    build();
    textView()->setText(text);
    if (m_type == Type::Plain)
        applyDefaultAttributesToText();
    applyTextDefaults();
    textView()->select(0, 0);
    // a file the user can't write: shown as it is, editing prevented only by Prevent Editing
    m_edited = false;
    updateTitle();
    return true;
}

void Document::build()
{
    if (m_window)
        return;
    // DocumentWindow.nib: a titled, closable, miniaturizable, resizable window (style 15), its min
    // size 100 x 36, the ScalingScrollView filling it (a vertical scroller, no border), the text in it
    m_window = std::make_unique<Window>(Size{494, 357}, displayName());
    m_window->setKeyEquivalents(OwnKeys);
    m_window->setResizable({100, 36});
    m_scroll = m_window->contentView()->add<TextScrollView>();
    m_scroll->setFrame(m_window->contentView()->bounds());
    TextView *tv = textView();
    tv->background = Color::gray(255);
    tv->onChange = [this] { textChanged(); };
    m_window->onResize = [this](Size s) { m_scroll->setFrame({0, 0, s.w, s.h}); };
    m_window->onClose = [this] { performClose(); };
    m_window->onKeyChanged = [this](bool key) {
        if (key)
            m_c.documentBecameMain(this);
    };
    m_window->onUnhandledKey = [this](Event &e) { m_c.menu().performKeyEquivalent(e); };
    m_window->makeFirstResponder(tv);
    if (m_hasMultiplePages)
        m_scroll->setPages(m_paper);
    applyTextDefaults();
}

void Document::setHasMultiplePages(bool pages)
{
    m_hasMultiplePages = pages;
    if (!m_scroll)
        return;
    m_scroll->setPages(pages ? std::optional<TextView::PageGeometry>(m_paper) : std::nullopt);
    textView()->scrollToSelection();   // scrollRangeToVisible: the selected range
}

Size Document::contentSizeForDefaults() const
{
    // WidthInChars × the width of "x" (else " ", else the font's widest advance) in the rich or plain
    // font, and the line fragment padding on each side (resizeWindowForViewSize:, 0x1000042cc);
    // HeightInChars × the layout manager's default line height for it; the vertical scroller beside
    const Font f = fontOf(userFont(!isRichText()));
    double charWidth = textWidth("x", f);
    if (charWidth <= 0)
        charWidth = textWidth(" ", f);
    const double w = std::ceil(double(prefInt("WidthInChars")) * charWidth) + 2 * TextView::Padding + CoreUI::ScrollerWidth;
    const double h = std::ceil(double(prefInt("HeightInChars")) * std::ceil(lineHeight(f)));
    return {std::max(100.0, w), std::max(36.0, h)};
}

void Document::applyTextDefaults()
{
    // -[Document defaultTextAttributes:]: rich text in the user font, tab stops every half inch
    // (every centimetre with AppleMeasurementUnits Centimeters), twelve of them; plain text in the
    // fixed pitch font, no stops, a tab every TabWidth spaces
    TextView *tv = textView();
    if (isRichText()) {
        const double step = pref("AppleMeasurementUnits") == "Centimeters" ? 28.346456692913385 : 36.0;
        std::vector<double> stops;
        for (int i = 1; i <= 12; ++i)
            stops.push_back(step * i);
        tv->setTabs(stops, 0);
        if (tv->text().size() == 0)
            tv->setTypingStyle(userFont(false));
    } else {
        const Style plain = userFont(true);
        double space = textWidth(" ", fontOf(plain));
        if (space <= 0)
            space = plain.size * 0.6;
        tv->setTabs({}, double(prefInt("TabWidth")) * space);
        tv->setTypingStyle(plain);
    }
    tv->editable = !m_readOnly;
    updateRulerState();
}

void Document::updateRulerState()
{
    if (!m_scroll)
        return;
    if (!isRichText()) {
        m_scroll->setRulerVisible(false);
        return;
    }
    // the preference decides once, when the document becomes rich text and editable
    if (!m_rulerDecided && !m_readOnly) {
        m_rulerShown = prefBool("ShowRuler");
        m_rulerDecided = true;
    }
    m_scroll->setRulerVisible(m_rulerShown);
    if (TextRulerView *r = m_scroll->ruler()) {
        r->centimeters = pref("AppleMeasurementUnits") == "Centimeters";
        r->setRichText(true);
    }
}

void Document::applyDefaultAttributesToText()
{
    // setAttributes:range: over the whole text: every run the default font, its paragraph's style
    TextView *tv = textView();
    RichText t = tv->text();
    const Style def = userFont(!isRichText());
    for (Style &s : t.styles) {
        s = def;
    }
    if (t.styles.empty())
        t.styles.push_back(def);
    t.compact();
    const size_t a = tv->selectionStart(), b = tv->selectionEnd();
    tv->setText(t);
    tv->select(std::min(a, t.size()), std::min(b, t.size()));
    tv->setTypingStyle(def);
}

void Document::show(std::optional<Point> at)
{
    build();
    // -[DocumentWindowController setupWindowForDocument]: the saved view size, else the first page's
    // with pages, else the size in characters; resizeWindowForViewSize: (0x100004249) the line
    // fragment padding beside without pages, and the scrollers (frameSizeForContentSize:...)
    const double sw = CoreUI::ScrollerWidth;
    if (m_viewSize.w > 0 && m_viewSize.h > 0)
        m_window->setContentSize({m_viewSize.w + (m_hasMultiplePages ? 0 : 2 * TextView::Padding) + sw,
                                  m_viewSize.h + (m_hasMultiplePages ? sw : 0)});
    else if (m_hasMultiplePages)
        m_window->setContentSize({m_paper.paperWidth + sw, m_paper.paperHeight + sw});
    else
        m_window->setContentSize(contentSizeForDefaults());
    m_scroll->setFrame(m_window->contentView()->bounds());
    updateTitle();
    m_window->show();
    if (at)
        m_window->moveTo(*at);
    m_window->makeFirstResponder(textView());
}

void Document::updateTitle()
{
    if (!m_window)
        return;
    m_window->setTitle(displayName());
    // the represented file's icon: TextEdit's own for its types (Info.plist's CFBundleTypeIconFile)
    if (!m_path.empty()) {
        const char *icon = m_type == Type::Plain ? "txt.icns" : m_type == Type::RTFD ? "rtfd.icns" : "rtf.icns";
        m_window->setRepresentedIcon(Image::load(res(icon), {16, 16}));
    } else {
        m_window->setRepresentedIcon(Image());
    }
    m_window->setDocumentEdited(m_edited);
    m_c.documentsChanged();
}

void Document::setEdited(bool e)
{
    if (e)
        m_transient = false;   // -[Document updateChangeCount:]: no longer replaceable
    if (m_edited == e)
        return;
    m_edited = e;
    if (m_window)
        m_window->setDocumentEdited(e);
}

void Document::textChanged()
{
    m_toggleUndo.reset();
    setEdited(true);
}

// ---- Make Plain Text / Make Rich Text (-[DocumentWindowController toggleRich:], 0x10000f1ad)

bool Document::toggleRichWillLoseInformation() const
{
    // 0x1000088dc: rich text whose first run isn't the default attributes all through, or a document
    // with properties
    if (!isRichText())
        return false;
    const RichText &t = textView()->text();
    if (t.size() == 0)
        return false;
    const Style def = userFont(false);
    for (size_t i = 0; i < t.runs.size(); ++i)
        if (t.styles[t.runs[i]] != def)
            return true;
    return !t.attachments.empty();
}

void Document::toggleRich()
{
    if (!toggleRichWillLoseInformation())
        return doToggleRich();
    // NSBeginAlertSheet("Convert this document to plain text?", OK, Cancel)
    Alert::beginSheet(m_window.get(), m_c.appIcon(), tr("Convert this document to plain text?"),
                      tr("Making a rich text document plain will lose all text styles (such as fonts and colors), images, attachments, and document properties."),
                      {tr("OK"), tr("Cancel")}, [this](int b) {
                          if (b == 0)
                              doToggleRich();
                      });
}

void Document::setRichText(bool rich)
{
    // -[Document setRichText:] (0x100008d85): the type plain text or RTF
    m_type = rich ? Type::RTF : Type::Plain;
}

void Document::doToggleRich()
{
    // 0x10000e12c: the type changed, the file forgotten (saving asks for a new one), the text in the
    // new type's default attributes (attachments removed when it becomes plain); undoable
    const bool wasRich = isRichText();
    const size_t selStart = textView()->selectionStart(), selEnd = textView()->selectionEnd();
    m_toggleUndo = ToggleUndo{textView()->text(), m_type};
    setRichText(!wasRich);
    if (!m_path.empty())
        m_path.clear();
    if (wasRich) {
        // removeAttachments: the U+FFFC characters out
        RichText t = textView()->text();
        for (size_t i = t.text.size(); i >= 3; --i) {
            if (t.text.compare(i - 3, 3, AttachmentCharacter) == 0) {
                t.erase(i - 3, i);
                i -= 2;
            }
        }
        t.attachments.clear();
        textView()->setText(t);
    }
    applyDefaultAttributesToText();
    applyTextDefaults();
    const size_t n = textView()->text().size();
    textView()->select(std::min(selStart, n), std::min(selEnd, n));
    setEdited(true);
    updateTitle();
}

void Document::toggleReadOnly()
{
    // -[Document toggleReadOnly:]: the text view's editable bound to !document.readOnly
    m_readOnly = !m_readOnly;
    textView()->editable = !m_readOnly;
    textView()->setNeedsDisplay();
}

// ---- validating and performing the window's and the document's items

bool Document::validate(const MainMenuItem &it, MenuValidation &v)
{
    const std::string &a = it.action;
    TextView *tv = textView();
    // DocumentWindowController (NSMenuValidation) 0x10000f88a and Document 0x100008517
    if (a == "toggleRich:") {
        v.title = menuTitle(tr(isRichText() ? "&Make Plain Text" : "&Make Rich Text"));
        return !m_readOnly;
    }
    if (a == "toggleReadOnly:") {
        v.title = tr(m_readOnly ? "Allow Editing" : "Prevent Editing");
        return true;
    }
    if (a == "togglePageBreaks:") {
        // Document's validateMenuItem: (0x1000085c4)
        v.title = menuTitle(tr(m_hasMultiplePages ? "&Wrap to Window" : "&Wrap to Page"));
        return true;
    }
    if (a == "toggleRuler:") {
        // NSTextView's: Show Ruler / Hide Ruler (AppKit's Ruler.strings), rich text only
        v.title = Assets::tr(AppKitBundle, "Ruler", m_scroll->isRulerVisible() ? "Hide Ruler" : "Show Ruler",
                             m_scroll->isRulerVisible() ? "Hide Ruler" : "Show Ruler");
        return isRichText();
    }
    if (a == "performClose:" || a == "saveDocumentAs:" || a == "performMiniaturize:" || a == "performZoom:" ||
        a == "saveDocumentAsPDFTo:")
        return true;
    if (a == "saveDocument:")
        return true;
    if (a == "revertDocumentToSaved:")
        return !m_path.empty() && m_edited;
    if (a == "printDocument:")
        return Workspace::inPath("lp");
    if (a == "undo:") {
        if (m_toggleUndo && !m_toggleUndo->redo) {
            v.title = "Undo " + tr(m_toggleUndo->type == Type::Plain ? "Make Rich Text" : "Make Plain Text");
            return true;
        }
        return tv->canUndo() && !m_readOnly;
    }
    if (a == "redo:") {
        if (m_toggleUndo && m_toggleUndo->redo) {
            v.title = "Redo " + tr(m_toggleUndo->type == Type::Plain ? "Make Rich Text" : "Make Plain Text");
            return true;
        }
        return tv->canRedo() && !m_readOnly;
    }
    if (a == "cut:" || a == "delete:")
        return tv->hasSelection() && !m_readOnly;
    if (a == "copy:")
        return tv->hasSelection();
    if (a == "paste:" || a == "pasteAsPlainText:")
        return !m_readOnly;
    if (a == "selectAll:" || a == "centerSelectionInVisibleArea:")
        return true;
    if (a == "performFindPanelAction:") {
        if (it.tag == 2 || it.tag == 3)
            return m_c.findPanel().hasFindString();
        if (it.tag == 7)
            return tv->hasSelection();
        return it.tag == 1;
    }
    if (a == "insertNewline:" || a == "insertLineBreak:")
        return !m_readOnly;
    if (a == "uppercaseWord:" || a == "lowercaseWord:" || a == "capitalizeWord:")
        return !m_readOnly;
    // the rich text's own: fonts, styles, alignment
    const bool rich = isRichText() && !m_readOnly;
    if (a == "addFontTrait:" || a == "underline:" || a == "modifyFont:" || a == "superscript:" || a == "subscript:" ||
        a == "unscript:" || a == "raiseBaseline:" || a == "lowerBaseline:" || a == "copyFont:" || a == "alignLeft:" ||
        a == "alignCenter:" || a == "alignRight:" || a == "alignJustified:")
        return rich || (a == "copyFont:" && isRichText());
    // spelling, substitutions, speech, the font, colour, link, list, table and spacing panels, the
    // ruler, Page Setup, Attach Files, the properties: not here
    return false;
}

void Document::perform(const MainMenuItem &it)
{
    const std::string &a = it.action;
    TextView *tv = textView();
    if (a == "performClose:")
        performClose();
    else if (a == "saveDocument:")
        save(false);
    else if (a == "saveDocumentAs:")
        save(true);
    else if (a == "saveDocumentAsPDFTo:")
        saveAsPDF();
    else if (a == "printDocument:")
        print();
    else if (a == "revertDocumentToSaved:") {
        // NSDocument's: asked first
        Alert::beginSheet(m_window.get(), m_c.appIcon(),
                          format(docString("Do you want to revert to the most recently saved version of the document \\U201C%@\\U201D?",
                                           "Do you want to revert to the most recently saved version of the document “%@”?"),
                                 {displayName()}),
                          docString("Your current changes will be lost.", "Your current changes will be lost."),
                          {docString("Revert", "Revert"), docString("Cancel", "Cancel")}, [this](int b) {
                              if (b != 0)
                                  return;
                              std::string error;
                              const std::string p = m_path;
                              if (read(p, &error))
                                  setEdited(false);
                          });
    } else if (a == "performMiniaturize:")
        m_window->miniaturize();
    else if (a == "toggleRich:")
        toggleRich();
    else if (a == "toggleReadOnly:")
        toggleReadOnly();
    else if (a == "togglePageBreaks:")
        setHasMultiplePages(!m_hasMultiplePages);   // -[Document togglePageBreaks:] (0x1000088a8)
    else if (a == "toggleRuler:") {
        m_rulerShown = !m_scroll->isRulerVisible();
        m_scroll->setRulerVisible(m_rulerShown);
        updateRulerState();
    }
    else if (a == "undo:") {
        if (m_toggleUndo && !m_toggleUndo->redo) {
            ToggleUndo u = *m_toggleUndo;
            ToggleUndo redo{tv->text(), m_type, true};
            m_type = u.type;
            tv->setText(u.text);
            applyTextDefaults();
            m_toggleUndo = redo;
            updateTitle();
        } else {
            tv->undo();
        }
    } else if (a == "redo:") {
        if (m_toggleUndo && m_toggleUndo->redo) {
            m_toggleUndo.reset();
            doToggleRich();
        } else {
            tv->redo();
        }
    } else if (a == "cut:")
        tv->cut();
    else if (a == "copy:")
        tv->copy();
    else if (a == "paste:")
        tv->paste(!isRichText());
    else if (a == "pasteAsPlainText:")
        tv->paste(true);
    else if (a == "delete:")
        tv->deleteSelection();
    else if (a == "selectAll:")
        tv->selectAll();
    else if (a == "insertNewline:" || a == "insertLineBreak:")
        tv->insertText("\n");
    else if (a == "centerSelectionInVisibleArea:")
        tv->scrollToSelection();
    else if (a == "performFindPanelAction:")
        m_c.findPanel().perform(it.tag);
    else if (a == "uppercaseWord:")
        tv->transform(0);
    else if (a == "lowercaseWord:")
        tv->transform(1);
    else if (a == "capitalizeWord:")
        tv->transform(2);
    else if (a == "addFontTrait:") {
        // NSFontManager's tags: 2 bold, 1 italic, toggled from the selection's first character
        const Style at = tv->styleAtSelection();
        if (it.tag == 2)
            tv->changeStyle([b = !at.bold](Style &s) { s.bold = b; });
        else
            tv->changeStyle([i = !at.italic](Style &s) { s.italic = i; });
    } else if (a == "modifyFont:") {
        // NSFontManager's modifyFont: tags 3 bigger, 4 smaller: a point at a time
        const double d = it.tag == 3 ? 1 : -1;
        tv->changeStyle([d](Style &s) { s.size = std::max(1.0, s.size + d); });
    } else if (a == "underline:") {
        const bool u = !tv->styleAtSelection().underline;
        tv->changeStyle([u](Style &s) { s.underline = u; });
    } else if (a == "superscript:")
        tv->changeStyle([](Style &s) { s.script = std::min(1, s.script + 1); });
    else if (a == "subscript:")
        tv->changeStyle([](Style &s) { s.script = std::max(-1, s.script - 1); });
    else if (a == "unscript:")
        tv->changeStyle([](Style &s) {
            s.script = 0;
            s.baseline = 0;
        });
    else if (a == "raiseBaseline:")
        tv->changeStyle([](Style &s) { s.baseline += 1; });
    else if (a == "lowerBaseline:")
        tv->changeStyle([](Style &s) { s.baseline -= 1; });
    else if (a == "alignLeft:")
        tv->setAlignment(Alignment::Left);
    else if (a == "alignCenter:")
        tv->setAlignment(Alignment::Center);
    else if (a == "alignRight:")
        tv->setAlignment(Alignment::Right);
    else if (a == "alignJustified:")
        tv->setAlignment(Alignment::Justified);
}

// ---- saving (NSDocument's save operations; -[Document fileWrapperOfType:error:] 0x1000092a1)

bool Document::writeTo(const std::string &path, Type type, std::string *error)
{
    // -[Document fileWrapperOfType:error:] (0x100009307): the document's attributes with the text,
    // the view mode, the margins, the paper, the view's size
    RichText t = textView()->text();
    DocumentAttributes &d = t.document;
    d.viewKind = m_hasMultiplePages ? 1 : 0;
    d.left = m_paper.left;
    d.right = m_paper.right;
    d.top = m_paper.top;
    d.bottom = m_paper.bottom;
    d.paperWidth = m_paper.paperWidth;
    d.paperHeight = m_paper.paperHeight;
    if (m_scroll) {
        // the view's size as resizeWindowForViewSize: takes it back: the scroll view less its
        // scrollers (without pages, less the line fragment padding too)
        const Rect f = m_scroll->frame();
        d.viewWidth = f.w - CoreUI::ScrollerWidth - (m_hasMultiplePages ? 0 : 2 * TextView::Padding);
        d.viewHeight = f.h - (m_hasMultiplePages ? CoreUI::ScrollerWidth : 0);
    }
    bool ok = false;
    if (type == Type::Plain) {
        // the text in encodingForSaving, else the one it was read in, else the suggested one
        // (PlainTextEncodingForWrite when the text fits in it, else UTF-8: 0x100008e56)
        std::string plain;
        for (size_t i = 0; i < t.text.size(); ++i) {
            if (t.text.compare(i, 3, AttachmentCharacter) == 0) {
                i += 2;
                continue;
            }
            plain += t.text[i];
        }
        unsigned long long enc = m_encodingForSaving != Automatic ? m_encodingForSaving : m_encoding;
        if (enc == Automatic || !encoding(enc)) {
            enc = static_cast<unsigned long long>(prefInt("PlainTextEncodingForWrite"));
            if (enc == Automatic || !encoding(enc) || enc == 10 || enc == 4 || !convert(plain, "UTF-8", encoding(enc)->iconv))
                enc = 4;
        }
        std::optional<std::string> bytes = convert(plain, "UTF-8", encoding(enc)->iconv);
        if (!bytes) {
            *error = format(tr("This document can no longer be saved using its original %@ encoding."), {encoding(enc)->name}) + " " +
                     tr("Please choose another encoding (such as UTF-8).");
            return false;
        }
        ok = writeFile(path, *bytes);
        if (ok)
            m_encoding = enc;
    } else if (type == Type::RTF) {
        ok = writeFile(path, toRTF(t, textView()->tabStops));
    } else {
        // RTFD: a folder, TXT.rtf and the pictures
        mkdir(path.c_str(), 0755);
        ok = writeFile(path + "/TXT.rtf", toRTF(t, textView()->tabStops));
        for (const Attachment &at : t.attachments)
            if (!at.name.empty())
                ok = writeFile(path + "/" + at.name, at.data) && ok;
    }
    if (!ok && error->empty())
        *error = std::strerror(errno);
    return ok;
}

void Document::save(bool as, std::function<void(bool)> done)
{
    // rich text with pictures is RTFD; plain text .txt when AddExtensionToNewPlainTextFiles
    const RichText &t = textView()->text();
    Type type = m_type;
    if (type == Type::RTF && !t.attachments.empty())
        type = Type::RTFD;
    auto write = [this, type, done](const std::string &path) {
        if (path.empty()) {
            if (done)
                done(false);
            return;
        }
        std::string error;
        if (!writeTo(path, type, &error)) {
            Alert::beginSheet(m_window.get(), m_c.appIcon(),
                              format(docString("The document \\U201C%@\\U201D could not be saved. %@", "The document “%1$@” could not be saved. %2$@"),
                                     {displayName(), ""}),
                              error, {tr("OK")}, [done](int) {
                                  if (done)
                                      done(false);
                              });
            return;
        }
        m_type = type;
        m_path = path;
        m_transient = false;
        setEdited(false);
        updateTitle();
        m_c.noteRecent(path);
        if (done)
            done(true);
    };
    if (!as && !m_path.empty() && (type != Type::RTFD || extension(m_path) == "rtfd"))
        return write(m_path);
    const char *ext = type == Type::Plain ? "txt" : type == Type::RTFD ? "rtfd" : "rtf";
    std::string base = m_path.empty() ? displayName() : baseName(m_path);
    const std::string folder = m_path.empty() ? home() + "/Documents" : dirName(m_path);
    std::string start = (isDir(folder) ? folder : home()) + "/" + base;
    if (type != Type::Plain || prefBool("AddExtensionToNewPlainTextFiles"))
        start += std::string(".") + ext;
    std::vector<FileFilter> filters;
    if (type == Type::Plain)
        filters = {{"Plain Text", {"*.txt", "*"}}};
    else if (type == Type::RTFD)
        filters = {{"Rich Text Format with Attachments (RTFD)", {"*.rtfd"}}};
    else
        filters = {{"Rich Text Format (RTF)", {"*.rtf"}}};
    SavePanel::run(docString("Save", "Save"), start, filters, [write, type](std::string path) {
        // the type's extension added when none is there (plain text: by the preference)
        if (!path.empty()) {
            const std::string e = extension(path);
            if (type == Type::RTF && e != "rtf")
                path += ".rtf";
            else if (type == Type::RTFD && e != "rtfd")
                path += ".rtfd";
            else if (type == Type::Plain && e.empty() && prefBool("AddExtensionToNewPlainTextFiles"))
                path += ".txt";
        }
        write(path);
    });
}

void Document::performClose(std::function<void(bool)> done)
{
    // NSDocument's canCloseDocumentWithDelegate:: an edited document asks first, as a sheet
    if (m_closing)
        return;
    if (!m_edited || (m_transient && textView()->text().size() == 0)) {
        close();
        if (done)
            done(true);
        return;
    }
    m_window->show();
    Alert::beginSheet(m_window.get(), m_c.appIcon(),
                      format(docString("Do you want to save the changes you made in the document \\U201C%@\\U201D?",
                                       "Do you want to save the changes you made in the document “%@”?"),
                             {displayName()}),
                      docString("Your changes will be lost if you don\\U2019t save them.", "Your changes will be lost if you don’t save them."),
                      // Save… when it has no file yet (the save panel follows)
                      {m_path.empty() ? docString("Save\\U2026", "Save…") : docString("Save", "Save"), docString("Cancel", "Cancel"),
                       docString("Don\\U2019t Save", "Don’t Save")},
                      [this, done](int b) {
                          if (b == 1) {
                              if (done)
                                  done(false);
                              return;
                          }
                          if (b == 2) {
                              close();
                              if (done)
                                  done(true);
                              return;
                          }
                          save(false, [this, done](bool saved) {
                              if (saved)
                                  close();
                              if (done)
                                  done(saved);
                          });
                      });
}

void Document::close()
{
    m_closing = true;
    if (m_window) {
        m_window->onClose = nullptr;
        m_window->onKeyChanged = nullptr;
        m_window->close();
    }
    m_c.documentClosed(this);
}

// ---- printing (-[Document printOperationWithSettings:error:]: the text laid out in the document's
// pages, MultiplePageView's geometry: the print info's paper and margins, 72 points); a PDF

namespace {

bool writePDF(const RichText &text, const std::vector<double> &stops, double interval, const TextView::PageGeometry &paper,
              const std::string &path)
{
    TextView view;
    view.setText(text);
    view.setTabs(stops, interval);
    view.setPages(paper);
    view.setFrame({0, 0, paper.paperWidth, paper.paperHeight});
    cairo_surface_t *pdf = cairo_pdf_surface_create(path.c_str(), paper.paperWidth, paper.paperHeight);
    if (cairo_surface_status(pdf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(pdf);
        return false;
    }
    cairo_t *cr = cairo_create(pdf);
    for (size_t k = 0; k < view.numberOfPages(); ++k) {
        // the page's text container, its glyphs alone
        const Rect page = view.pageRect(k);
        cairo_save(cr);
        cairo_rectangle(cr, paper.left - TextView::Padding, paper.top, view.textWidth() + 2 * TextView::Padding,
                        paper.paperHeight - paper.top - paper.bottom);
        cairo_clip(cr);
        cairo_translate(cr, 0, -page.y);
        view.drawGlyphs(cr, page.y, page.y + page.h);
        cairo_restore(cr);
        cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(pdf);
    return true;
}

} // namespace

void Document::saveAsPDF()
{
    // -[Document saveDocumentAsPDFTo:] (0x1000082c8): the print operation's PDF to a file
    const std::string start = (isDir(home() + "/Documents") ? home() + "/Documents" : home()) + "/" +
                              (m_path.empty() ? displayName() : baseName(m_path)) + ".pdf";
    SavePanel::run(docString("Save", "Save"), start, {{"PDF", {"*.pdf"}}}, [this](std::string path) {
        if (path.empty())
            return;
        if (extension(path) != "pdf")
            path += ".pdf";
        TextView *tv = textView();
        if (!writePDF(tv->text(), tv->tabStops, tv->defaultTabInterval, m_paper, path))
            Alert::beginSheet(m_window.get(), m_c.appIcon(),
                              format(docString("The document \\U201C%@\\U201D could not be exported as \\U201C%@\\U201D. %@",
                                               "The document “%1$@” could not be exported as “%2$@”. %3$@"),
                                     {displayName(), fileName(path), ""}),
                              std::strerror(errno), {tr("OK")}, nullptr);
    });
}

void Document::print()
{
    char tmpl[] = "/tmp/textedit-print-XXXXXX.pdf";
    const int fd = mkstemps(tmpl, 4);
    if (fd < 0)
        return;
    ::close(fd);
    TextView *tv = textView();
    if (!writePDF(tv->text(), tv->tabStops, tv->defaultTabInterval, m_paper, tmpl)) {
        std::remove(tmpl);
        return;
    }
    std::vector<std::string> argv = {"lp", "-t", displayName(), tmpl};
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, "lp", nullptr, nullptr, args.data(), environ) == 0) {
        const std::string file = tmpl;
        App::shared().addTimer(5, false, [pid, file] {
            waitpid(pid, nullptr, WNOHANG);
            std::remove(file.c_str());
        });
    } else {
        std::remove(tmpl);
    }
}

// ---- the find panel


// ---- Select Line… (⌘L, SelectLinePanel.nib, LinePanelController): "Line number" — n, n-m, or +n / -n
// lines from the selection's; the lines selected and shown, the panel gone; else an alert

namespace {

// -parseLineDescription:fromLineSpec:toLineSpec:relative:
bool parseLines(const std::string &spec, long &from, long &to, int &relative)
{
    size_t i = 0;
    auto skip = [&] {
        while (i < spec.size() && spec[i] == ' ')
            ++i;
    };
    skip();
    relative = 0;
    if (i < spec.size() && (spec[i] == '+' || spec[i] == '-')) {
        relative = spec[i] == '+' ? 1 : -1;
        ++i;
    }
    auto number = [&](long &out) {
        skip();
        const size_t start = i;
        while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i])))
            ++i;
        if (i == start)
            return false;
        out = std::atol(spec.substr(start, i - start).c_str());
        return true;
    };
    if (!number(from))
        return false;
    skip();
    if (i < spec.size() && spec[i] == '-') {
        ++i;
        if (!number(to) || to < from)
            return false;
    } else {
        to = from;
    }
    skip();
    return i == spec.size();
}

// -getRange:inTextView:fromLineSpec:toLineSpec:relative:, on the text's bytes (lines end at '\n')
bool lineRange(const std::string &text, size_t selection, long from, long to, int relative, size_t &start, size_t &end)
{
    auto lineStart = [&](size_t at) {
        const size_t nl = at == 0 ? std::string::npos : text.rfind('\n', at - 1);
        return nl == std::string::npos ? size_t(0) : nl + 1;
    };
    auto nextLine = [&](size_t at) {
        const size_t nl = text.find('\n', at);
        return nl == std::string::npos ? text.size() : nl + 1;
    };
    if (relative == 0 && from == 0)
        return false;
    size_t s = relative ? lineStart(selection) : 0;
    if (relative < 0) {
        // backwards: from lines before the selection's, through to lines before
        size_t first = s;
        for (long k = 0; k < to; ++k) {
            if (first == 0)
                return false;
            first = lineStart(first - 1);
        }
        size_t last = s;
        for (long k = 1; k < from; ++k) {
            if (last == 0)
                return false;
            last = lineStart(last - 1);
        }
        start = first;
        end = last;
        return true;
    }
    for (long k = relative ? 0 : 1; k < from; ++k) {
        if (s == text.size())
            return false;
        s = nextLine(s);
    }
    size_t e = s;
    for (long k = from; k <= to; ++k) {
        if (e == text.size())
            return false;
        e = nextLine(e);
    }
    start = s;
    end = e;
    return true;
}

} // namespace

void Controller::showLinePanel()
{
    if (!m_linePanel) {
        constexpr double W = 249, H = 62;
        m_linePanel = std::make_unique<Window>(Size{W, H}, "Select Line");
        m_linePanel->setFloatingPanel(true);
        View *root = m_linePanel->contentView();
        m_lineField = root->add<TextField>();
        m_lineField->placeholder = "Line number";
        m_lineField->setFrame({20, H - 20 - 22, 116, 22});
        auto *select = root->add<Button>("Select", Button::Push);
        select->isDefault = true;
        select->setFrame({145, H - 12 - 32, 90, 32});
        auto go = [this](bool closeOnSuccess) {
            // -selectClicked: / -lineFieldChanged:
            Document *d = mainDocument();
            const std::string spec = m_lineField->text;
            if (!d || spec.empty())
                return;
            long from = 0, to = 0;
            int relative = 0;
            size_t start = 0, end = 0;
            TextView *tv = d->textView();
            if (parseLines(spec, from, to, relative) && lineRange(tv->text().text, tv->selectionStart(), from, to, relative, start, end)) {
                tv->select(start, end);
                tv->scrollToSelection();
                if (closeOnSuccess)
                    m_linePanel->close();
                return;
            }
            const std::string shown = spec.size() > 100 ? spec.substr(0, 100) + "…" : spec;
            Alert::run(m_icon, "Invalid line specification “" + shown + "”.",
                       "Please enter the line number or numbers (separated by dash) of the line(s) to select.", {"OK"}, nullptr);
        };
        select->action = [go] { go(false); };
        m_lineField->onEnter = [go] { go(true); };
        m_linePanel->onUnhandledKey = panelKeys(m_linePanel.get(), m_menu);
    }
    m_linePanel->show();
    m_linePanel->makeFirstResponder(m_lineField);
    m_lineField->selectAll();
}

// ---- Show Properties (⌥⌘P, DocumentProperties.nib, DocumentPropertiesPanelController): a utility
// panel (333 × 264) on the main document's Author, Organization, Copyright, Title, Subject,
// Keywords, Comment; editable for rich text that isn't read only; each change the document's

void Controller::toggleProperties()
{
    if (m_properties && m_properties->isVisible()) {
        m_properties->close();
        return;
    }
    if (!m_properties) {
        constexpr double W = 333, H = 264;
        m_properties = std::make_unique<Window>(Size{W, H}, "Document Properties");
        m_properties->setFloatingPanel(true);
        View *root = m_properties->contentView();
        struct Row {
            const char *key, *label;
            double y, labelY, h;
        };
        const Row rows[] = {{"author", "Author:", 235, 237, 19},      {"company", "Organization:", 208, 207, 19},
                            {"copyright", "Copyright:", 181, 183, 19}, {"title", "Title:", 154, 156, 19},
                            {"subject", "Subject:", 127, 129, 19},     {"keywords", "Keywords:", 100, 102, 19},
                            {"comment", "Comment:", 14, 75, 78}};
        for (const Row &r : rows) {
            auto *l = root->add<Label>(r.label, Font::system(11));
            l->style.align = Align::Right;
            l->setFrame({5, H - r.labelY - 14, 77, 14});
            auto *f = root->add<TextField>();
            f->setFrame({87, H - r.y - r.h, 233, r.h});
            const std::string key = r.key;
            // -objectDidEndEditing:: the value the document's
            f->onChange = [this, key, f](const std::string &) {
                if (Document *d = mainDocument())
                    d->setProperty(key, f->text);
            };
            m_propertyFields[key] = f;
        }
        m_properties->onUnhandledKey = panelKeys(m_properties.get(), m_menu);
    }
    refreshProperties();
    m_properties->show();
}

void Controller::refreshProperties()
{
    if (!m_properties)
        return;
    Document *d = mainDocument();
    const bool editable = d && d->isRichText() && !d->isReadOnly();
    for (auto &[key, field] : m_propertyFields) {
        field->setText(d ? d->property(key) : std::string());
        field->editable = editable;
        field->setEnabled(d != nullptr);
        field->setNeedsDisplay();
    }
}

void FindPanel::build()
{
    if (m_window)
        return;
    const std::string nib = Assets::path(std::string(AppKitBundle) + "/Resources/English.lproj/NSFindPanel.nib/keyedobjects.nib");
    m_nib.load(nib, res(""));
    Size size{525, 152};
    View *content = nullptr;
    for (const Nib::WindowEntry &w : m_nib.windows()) {
        content = m_nib.instantiateWindow(w.object, &size);
        break;
    }
    m_window = std::make_unique<Window>(size, "Find");
    m_window->setKeyEquivalents(OwnKeys);
    if (content) {
        content->setFrame({0, 0, size.w, size.h});
        m_window->contentView()->addSubview(content);
    }
    // its outlets (NSTextFinder's): the find and replace combo boxes, the options, the status line
    m_find = m_nib.outlet<TextField>("findComboBox");
    m_replace = m_nib.outlet<TextField>("replaceComboBox");
    m_ignoreCase = m_nib.outlet<CheckBox>("ignoreCaseButton");
    m_wrap = m_nib.outlet<CheckBox>("wrapAroundButton");
    m_status = m_nib.outlet<Label>("statusField");
    if (m_status)
        m_status->setText("");
    if (m_ignoreCase)
        m_ignoreCase->setChecked(true);
    if (m_wrap)
        m_wrap->setChecked(true);
    if (m_find) {
        m_find->setText(m_findString);
        m_find->onEnter = [this] {
            // findNextAndOrderFindPanelOut:
            if (find(true))
                m_window->close();
        };
    }
    for (const Nib::Connection &c : m_nib.controls()) {
        auto *control = dynamic_cast<class AK::Control *>(c.view);
        if (control && c.action == "performFindPanelAction:")
            control->action = [this, tag = c.tag] { perform(tag); };
    }
    m_window->onUnhandledKey = panelKeys(m_window.get(), m_c.menu());
}

void FindPanel::show()
{
    build();
    m_window->show();
    if (m_find) {
        m_window->makeFirstResponder(m_find);
        m_find->selectAll();
    }
}

namespace {

size_t search(const std::string &hay, const std::string &needle, size_t from, bool forward, bool ignoreCase)
{
    if (needle.empty() || hay.size() < needle.size())
        return std::string::npos;
    auto matchAt = [&](size_t at) {
        if (!ignoreCase)
            return hay.compare(at, needle.size(), needle) == 0;
        size_t i = at, j = 0;
        while (j < needle.size()) {
            if (i >= hay.size())
                return false;
            const gunichar a = g_utf8_get_char(hay.c_str() + i), b = g_utf8_get_char(needle.c_str() + j);
            if (g_unichar_tolower(a) != g_unichar_tolower(b))
                return false;
            i = size_t(g_utf8_next_char(hay.c_str() + i) - hay.c_str());
            j = size_t(g_utf8_next_char(needle.c_str() + j) - needle.c_str());
        }
        return true;
    };
    if (forward) {
        for (size_t at = from; at < hay.size(); ++at)
            if ((static_cast<unsigned char>(hay[at]) & 0xC0) != 0x80 && matchAt(at))
                return at;
    } else {
        for (size_t at = std::min(from, hay.size()); at-- > 0;)
            if ((static_cast<unsigned char>(hay[at]) & 0xC0) != 0x80 && matchAt(at))
                return at;
    }
    return std::string::npos;
}

size_t matchLength(const std::string &hay, size_t at, const std::string &needle)
{
    size_t i = at, chars = size_t(g_utf8_strlen(needle.c_str(), long(needle.size())));
    while (chars-- > 0 && i < hay.size())
        i = size_t(g_utf8_next_char(hay.c_str() + i) - hay.c_str());
    return i - at;
}

} // namespace

bool FindPanel::find(bool forward)
{
    m_findString = findText();
    Document *d = m_c.mainDocument();
    if (!d || m_findString.empty())
        return false;
    TextView *tv = d->textView();
    const std::string &hay = tv->text().text;
    const bool ic = !m_ignoreCase || m_ignoreCase->checked();
    const bool wrap = !m_wrap || m_wrap->checked();
    size_t at = search(hay, m_findString, forward ? tv->selectionEnd() : tv->selectionStart(), forward, ic);
    if (at == std::string::npos && wrap)
        at = search(hay, m_findString, forward ? 0 : hay.size(), forward, ic);
    if (at == std::string::npos) {
        status(findString("Not found"));
        return false;
    }
    status("");
    tv->select(at, at + matchLength(hay, at, m_findString));
    tv->scrollToSelection();
    return true;
}

void FindPanel::perform(int tag)
{
    Document *d = m_c.mainDocument();
    TextView *tv = d ? d->textView() : nullptr;
    switch (tag) {
    case 1:
        show();
        return;
    case 2:
        find(true);
        return;
    case 3:
        find(false);
        return;
    case 7:
        if (tv && tv->hasSelection()) {
            m_findString = tv->selectedString();
            if (m_find)
                m_find->setText(m_findString);
        }
        return;
    default:
        break;
    }
    if (!tv || d->isReadOnly())
        return;
    const std::string with = m_replace ? m_replace->text : std::string();
    if (tag == 4) {
        // Replace All: every match, from the start; "%ld replaced"
        m_findString = findText();
        const bool ic = !m_ignoreCase || m_ignoreCase->checked();
        long count = 0;
        size_t from = 0;
        tv->select(0, 0);
        for (;;) {
            const std::string &hay = tv->text().text;
            const size_t at = search(hay, m_findString, from, true, ic);
            if (at == std::string::npos)
                break;
            tv->select(at, at + matchLength(hay, at, m_findString));
            tv->insertText(with);
            from = at + with.size();
            ++count;
        }
        if (count == 0)
            status(findString("Not found"));
        else if (count == 1) {
            status(findString("1 replaced"));
        } else {
            std::string msg = findString("%ld replaced");
            if (size_t p = msg.find("%ld"); p != std::string::npos)
                msg.replace(p, 3, std::to_string(count));
            status(msg);
        }
        return;
    }
    // 5 Replace, 6 Replace & Find: the selection when it is a match
    if (tv->hasSelection())
        tv->insertText(with);
    if (tag == 6)
        find(true);
}

// ---- Controller

Controller::Controller()
{
    m_menu.load(res("English.lproj/Edit.nib"));
    m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) { return validate(it, v); };
    m_menu.perform = [this](const MainMenuItem &it) { perform(it); };
    for (MainMenuItem &m : m_menu.menus())
        if (m.title == "Window")
            m_staticWindowItems = m.items.size();
    m_icon = Image::load(res("Edit.icns"), {64, 64});
}

std::optional<Point> Controller::nextCascadePoint()
{
    // NSWindowController's cascading: the first window where DocumentWindow.nib has it (241 from the
    // left, its top 76 under the screen's on the nib's 1920 x 1178 screen), each next one down and
    // right from the last, back to the start past the visible frame
    const Rect visible = App::shared().visibleFrame();
    const Rect screen = App::shared().screenFrame();
    Point p;
    if (!m_lastCascade) {
        p = {screen.x + 241, std::max(visible.y, screen.y + 76)};
    } else {
        p = {m_lastCascade->x + 21, m_lastCascade->y + 23};
        if (p.y > visible.bottom() - 200 || p.x > visible.right() - 200)
            p = {visible.x + 21, visible.y};
    }
    m_lastCascade = p;
    return p;
}

Document *Controller::newDocument(bool transient)
{
    // the lowest Untitled number free
    int n = 1;
    while (std::find(m_untitledUsed.begin(), m_untitledUsed.end(), n) != m_untitledUsed.end())
        ++n;
    m_untitledUsed.push_back(n);
    m_documents.push_back(std::make_unique<Document>(*this, n));
    Document *d = m_documents.back().get();
    // -setDocumentPropertiesToDefaults: the preferences' author, organization and copyright
    for (const char *k : {"author", "company", "copyright"}) {
        std::string *p = d->textView()->text().document.property(k);
        if (p)
            *p = pref(k);
    }
    d->setTransient(transient);
    m_order.insert(m_order.begin(), d);
    d->show(nextCascadePoint());
    App::shared().setFrontWithoutWindows(false);
    updateWindowsMenu();
    return d;
}

void Controller::openFiles(const std::vector<std::string> &paths)
{
    // -[DocumentController openDocumentWithContentsOfURL:display:error:] (0x100004bc6): an open
    // document comes forward; the transient Untitled one is replaced (0x100006625: the new document
    // takes its window's place)
    for (const std::string &raw : paths) {
        char *whole = realpath(raw.c_str(), nullptr);
        std::string path = whole ? whole : raw;
        std::free(whole);
        bool open = false;
        for (Document *d : m_order)
            if (d->path() == path) {
                d->window()->show();
                open = true;
            }
        if (open)
            continue;
        Document *transient = nullptr;
        if (m_order.size() == 1 && m_order[0]->isTransientAndCanBeReplaced())
            transient = m_order[0];
        auto doc = std::make_unique<Document>(*this, 0);
        std::string error;
        if (!doc->read(path, &error)) {
            std::string name = fileName(path);
            Alert::run(m_icon,
                       format(docString("The document \\U201C%@\\U201D could not be opened. %@", "The document “%1$@” could not be opened. %2$@"),
                              {name, error}),
                       "", {tr("OK")}, nullptr);
            continue;
        }
        Document *d = doc.get();
        m_documents.push_back(std::move(doc));
        m_order.insert(m_order.begin(), d);
        std::optional<Point> at;
        if (transient) {
            at = transient->window()->screenOrigin();
            transient->performClose();
        } else {
            at = nextCascadePoint();
        }
        d->show(at);
        noteRecent(path);
    }
    App::shared().setFrontWithoutWindows(m_order.empty());
    updateWindowsMenu();
}

Document *Controller::mainDocument() const
{
    for (Document *d : m_order)
        if (d->isMain())
            return d;
    return m_order.empty() ? nullptr : m_order.front();
}

void Controller::documentBecameMain(Document *d)
{
    App::shared().later([this] { refreshProperties(); });
    auto it = std::find(m_order.begin(), m_order.end(), d);
    if (it != m_order.end()) {
        m_order.erase(it);
        m_order.insert(m_order.begin(), d);
    }
    App::shared().setFrontWithoutWindows(false);
    updateWindowsMenu();
}

void Controller::documentClosed(Document *d)
{
    m_order.erase(std::remove(m_order.begin(), m_order.end(), d), m_order.end());
    App::shared().later([this, d] {
        for (auto it = m_documents.begin(); it != m_documents.end(); ++it)
            if (it->get() == d) {
                m_documents.erase(it);
                break;
            }
    });
    // the Untitled numbers start again once every document is closed
    if (m_order.empty())
        m_untitledUsed.clear();
    // the last window closed: TextEdit stays, in front with its menus
    if (m_order.empty())
        App::shared().setFrontWithoutWindows(true);
    updateWindowsMenu();
}

void Controller::noteRecent(const std::string &path)
{
    // NSDocumentController's recent documents (Open Recent), ten at most, the latest first
    std::vector<std::string> recent = settings().list("NSRecentDocuments");
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 10)
        recent.resize(10);
    settings().set("NSRecentDocuments", recent);
    updateRecentMenu();
}

void Controller::updateRecentMenu()
{
    const std::vector<std::string> recent = settings().list("NSRecentDocuments");
    for (MainMenuItem &m : m_menu.menus()) {
        if (m.title != "File")
            continue;
        for (MainMenuItem &it : m.items) {
            if (it.title != "Open Recent")
                continue;
            MainMenuItem clear;
            for (const MainMenuItem &s : it.items)
                if (s.action == "clearRecentDocuments:")
                    clear = s;
            it.items.clear();
            for (size_t i = 0; i < recent.size(); ++i) {
                MainMenuItem r;
                r.title = fileName(recent[i]);
                r.action = "openRecentDocument:";
                r.tag = int(i);
                it.items.push_back(r);
            }
            if (!recent.empty()) {
                MainMenuItem sep;
                sep.separator = true;
                it.items.push_back(sep);
            }
            if (clear.action.empty()) {
                clear.title = "Clear Menu";
                clear.action = "clearRecentDocuments:";
            }
            it.items.push_back(clear);
        }
    }
}

void Controller::updateWindowsMenu()
{
    // the Window menu: its own items, then the documents' windows (the main one checked; a diamond
    // for the edited ones is the menu bar's)
    for (MainMenuItem &m : m_menu.menus()) {
        if (m.title != "Window")
            continue;
        m.items.resize(std::min(m.items.size(), m_staticWindowItems));
        if (m_order.empty())
            break;
        MainMenuItem sep;
        sep.separator = true;
        m.items.push_back(sep);
        for (size_t i = 0; i < m_documents.size(); ++i) {
            if (std::find(m_order.begin(), m_order.end(), m_documents[i].get()) == m_order.end())
                continue;
            MainMenuItem it;
            it.title = m_documents[i]->displayName();
            it.action = "makeKeyAndOrderFront:";
            it.tag = int(i);
            m.items.push_back(it);
        }
    }
}

void Controller::openPanel()
{
    // -[DocumentController runModalOpenPanel:forTypes:] (0x10000d9b1): TextEdit's types; from the
    // main document's folder with OpenPanelFollowsMainWindow
    std::string dir = home() + "/Documents";
    if (prefBool("OpenPanelFollowsMainWindow"))
        if (Document *d = mainDocument(); d && !d->path().empty())
            dir = dirName(d->path());
    if (!isDir(dir))
        dir = home();
    OpenPanel::run("Open", dir,
                   {{"TextEdit documents", {"*.rtf", "*.rtfd", "*.txt", "*.text", "*.html", "*.htm", "*.RTF", "*.TXT"}}, {"All files", {"*"}}},
                   true, [this](std::vector<std::string> files) { openFiles(files); });
}

void Controller::start(const std::vector<std::string> &files)
{
    m_menu.publish();
    updateRecentMenu();
    if (!files.empty()) {
        openFiles(files);
    } else {
        // -[DocumentController openUntitledDocumentAndDisplay:error:] (0x100001966): the one opened at
        // launch (an 'oapp' or 'rapp' event) is transient
        newDocument(true);
    }
}

void Controller::showAbout()
{
    // -[NSApplication orderFrontStandardAboutPanel:]: the icon, the name, the version, the copyright
    if (!m_about) {
        const auto info = Assets::plistStrings(std::string(Bundle) + "/Contents/Info.plist");
        const auto version = Assets::plistStrings(std::string(Bundle) + "/Contents/version.plist");
        auto get = [](const std::map<std::string, std::string> &m, const char *k) {
            auto it = m.find(k);
            return it == m.end() ? std::string() : it->second;
        };
        std::string copyright = Assets::tr(Bundle, "InfoPlist", "NSHumanReadableCopyright", get(info, "NSHumanReadableCopyright"));
        m_about = std::make_unique<Window>(Size{285, 196}, "");
        m_about->setKeyEquivalents(OwnKeys);
        View *c = m_about->contentView();
        auto *icon = c->add<ImageView>();
        icon->image = m_icon;
        icon->setFrame({110, 16, 64, 64});
        auto *name = c->add<Label>("TextEdit", Font::boldSystem(14));
        name->style.align = Align::Center;
        name->setFrame({0, 94, 285, 18});
        const std::string shortVersion = get(info, "CFBundleShortVersionString").empty() ? get(version, "CFBundleShortVersionString")
                                                                                          : get(info, "CFBundleShortVersionString");
        const std::string build = get(info, "CFBundleVersion").empty() ? get(version, "CFBundleVersion") : get(info, "CFBundleVersion");
        auto *ver = c->add<Label>("Version " + shortVersion + " (" + build + ")", Font::system(10));
        ver->style.align = Align::Center;
        ver->setFrame({0, 118, 285, 14});
        auto *cr = c->add<Label>(copyright, Font::system(10));
        cr->style.align = Align::Center;
        cr->wrap = true;
        cr->setFrame({10, 152, 265, 30});
        m_about->onUnhandledKey = panelKeys(m_about.get(), m_menu);
    }
    m_about->show();
}

void Controller::showPreferences()
{
    // Preferences.nib (Preferences, an NSWindowController): New Document and Open and Save, each
    // control bound to the shared user defaults controller's values.<key>
    if (!m_prefs) {
        m_prefsNib.load(res("English.lproj/Preferences.nib/keyedobjects.nib"), res(""));
        View *content = m_prefsNib.instantiate("window");
        const Size size = m_prefsNib.size();
        m_prefs = std::make_unique<Window>(size, "Preferences");
        m_prefs->setKeyEquivalents(OwnKeys);
        if (content) {
            content->setFrame({0, 0, size.w, size.h});
            m_prefs->contentView()->addSubview(content);
        }
        auto boolKey = [this](const char *key) {
            for (View *v : m_prefsNib.bound(std::string("values.") + key)) {
                auto *box = dynamic_cast<CheckBox *>(v);
                if (!box)
                    continue;
                box->setChecked(prefBool(key));
                box->onToggle = [key](CheckBox *b) { settings().set(key, b->checked()); };
            }
        };
        for (const char *k : {"ShowPageBreaks", "ShowRuler", "CheckSpellingWhileTyping", "CheckGrammarWithSpelling",
                              "CorrectSpellingAutomatically", "SmartCopyPaste", "SmartQuotes", "SmartDashes", "SmartLinks",
                              "DataDetectors", "TextReplacement", "IgnoreRichText", "IgnoreHTML", "DeleteBackup",
                              "AddExtensionToNewPlainTextFiles", "PreserveWhitespace"})
            boolKey(k);
        auto textKey = [this](const char *key) {
            for (View *v : m_prefsNib.bound(std::string("values.") + key)) {
                auto *f = dynamic_cast<TextField *>(v);
                if (!f)
                    continue;
                f->setText(pref(key));
                f->onChange = [key](const std::string &s) { settings().set(key, s); };
            }
        };
        for (const char *k : {"WidthInChars", "HeightInChars", "author", "company", "copyright"})
            textKey(k);
        // Format: Rich text (tag 1) or Plain text (0), the matrix bound by selectedTag
        if (auto *r = m_prefsNib.bound1<RadioGroup>("values.RichText", "selectedTag")) {
            r->select(prefBool("RichText") ? 0 : 1);
            r->onSelect = [](int i) { settings().set("RichText", i == 0); };
        }
        // the fonts' names (FontNameTransformer: the display name and the size)
        auto fontName = [](bool fixed) {
            const Style s = userFont(fixed);
            char size[16];
            std::snprintf(size, sizeof size, "%g", s.size);
            return s.family + " " + size;
        };
        for (View *v : m_prefsNib.bound("richTextFont"))
            if (auto *l = dynamic_cast<Label *>(v))
                l->setText(fontName(false));
            else if (auto *f = dynamic_cast<TextField *>(v))
                f->setText(fontName(false));
        for (View *v : m_prefsNib.bound("plainTextFont"))
            if (auto *l = dynamic_cast<Label *>(v))
                l->setText(fontName(true));
            else if (auto *f = dynamic_cast<TextField *>(v))
                f->setText(fontName(true));
        // the autosave interval's tags (15, 30, 60, 300 seconds, 0 never)
        if (auto *p = m_prefsNib.bound1<PopUpButton>("values.AutosaveDelay", "selectedTag")) {
            static const int tags[] = {15, 30, 60, 300, 0};
            const long long now = prefInt("AutosaveDelay");
            for (int i = 0; i < 5; ++i)
                if (tags[i] == now)
                    p->setSelected(i);
            p->onSelect = [](int i) {
                if (i >= 0 && i < 5)
                    settings().set("AutosaveDelay", tags[i]);
            };
        }
        // the encodings' popups (EncodingPopUpButtonCell, filled by EncodingManager): Automatic, then
        // the encodings
        for (const char *key : {"PlainTextEncoding", "PlainTextEncodingForWrite"}) {
            auto *p = m_prefsNib.bound1<PopUpButton>(std::string("values.") + key, "selectedObject");
            if (!p)
                continue;
            p->items.clear();
            p->items.push_back(MenuItem(tr("Automatic")));
            p->items.push_back(MenuItem::line());
            const auto now = static_cast<unsigned long long>(prefInt(key));
            int selected = 0;
            for (const Encoding &e : encodings()) {
                if (e.ns == now)
                    selected = int(p->items.size());
                p->items.push_back(MenuItem(e.name));
            }
            p->setSelected(selected);
            p->onSelect = [key](int i) {
                if (i <= 1) {
                    settings().set(key, std::string("4294967295"));
                } else if (size_t(i - 2) < encodings().size()) {
                    settings().set(key, std::to_string(encodings()[size_t(i - 2)].ns));
                }
            };
        }
        for (const Nib::Connection &c : m_prefsNib.controls()) {
            auto *control = dynamic_cast<class AK::Control *>(c.view);
            if (control && c.action == "revertToDefault:")
                control->action = [this] {
                    // -[Preferences revertToDefault:] (0x10000bef3): every registered default back
                    for (const auto &kv : registeredDefaults())
                        if (kv.first.compare(0, 2, "NS") != 0)
                            settings().set(kv.first, kv.second);
                    m_prefs->close();
                    App::shared().later([this] {
                        m_prefs.reset();
                        showPreferences();
                    });
                };
        }
        m_prefs->onUnhandledKey = panelKeys(m_prefs.get(), m_menu);
    }
    m_prefs->show();
}

bool Controller::validate(const MainMenuItem &it, MenuValidation &v)
{
    const std::string &a = it.action;
    if (a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
        a == "orderFrontStandardAboutPanel:" || a == "newDocument:" || a == "openDocument:" || a == "arrangeInFront:")
        return true;
    if (a == "showWindow:")
        return it.title.find("Preferences") != std::string::npos || (it.title.find("Line") != std::string::npos && mainDocument());
    if (a == "toggleWindow:") {
        // -[DocumentPropertiesPanelController validateMenuItem:]
        v.title = m_properties && m_properties->isVisible() ? "Hide Properties" : "Show Properties";
        return true;
    }
    if (a == "showHelp:")
        return Assets::exists(std::string(Bundle) + "/Contents/Resources/TextEdit.help/Contents/Resources/English.lproj/index.html");
    if (a == "openRecentDocument:" || a == "clearRecentDocuments:")
        return !settings().list("NSRecentDocuments").empty();
    if (a == "makeKeyAndOrderFront:") {
        v.checked = it.tag >= 0 && size_t(it.tag) < m_documents.size() && m_documents[size_t(it.tag)]->isMain();
        return true;
    }
    if (a == "performClose:")
        for (Window *panel : {m_find.window(), m_prefs.get(), m_about.get()})
            if (panel && panel->isKey())
                return true;
    if (a == "saveAllDocuments:")
        return std::any_of(m_order.begin(), m_order.end(), [](Document *d) { return d->isEdited(); });
    Document *d = mainDocument();
    if (a == "pasteFont:")
        return d && d->isRichText() && !d->isReadOnly() && m_copiedStyle.has_value();
    if (a == "copyFont:")
        return d && d->isRichText();
    return d && d->validate(it, v);
}

void Controller::perform(const MainMenuItem &it)
{
    const std::string &a = it.action;
    if (a == "terminate:")
        return quit();
    if (a == "orderFrontStandardAboutPanel:")
        return showAbout();
    if (a == "newDocument:") {
        newDocument();
        return;
    }
    if (a == "openDocument:")
        return openPanel();
    if (a == "showWindow:")
        return it.title.find("Line") != std::string::npos ? showLinePanel() : showPreferences();
    if (a == "toggleWindow:")
        return toggleProperties();
    if (a == "showHelp:")
        return Workspace::openFile(Assets::path(std::string(Bundle) + "/Contents/Resources/TextEdit.help/Contents/Resources/English.lproj/index.html"));
    if (a == "openRecentDocument:") {
        const std::vector<std::string> recent = settings().list("NSRecentDocuments");
        if (it.tag >= 0 && size_t(it.tag) < recent.size())
            openFiles({recent[size_t(it.tag)]});
        return;
    }
    if (a == "clearRecentDocuments:") {
        settings().set("NSRecentDocuments", std::vector<std::string>{});
        return updateRecentMenu();
    }
    if (a == "arrangeInFront:") {
        for (auto r = m_order.rbegin(); r != m_order.rend(); ++r)
            (*r)->window()->show();
        return;
    }
    if (a == "makeKeyAndOrderFront:") {
        if (it.tag >= 0 && size_t(it.tag) < m_documents.size())
            m_documents[size_t(it.tag)]->window()->show();
        return;
    }
    if (a == "saveAllDocuments:") {
        for (Document *d : m_order)
            if (d->isEdited())
                d->save(false);
        return;
    }
    // Close: the key window's (a panel's too)
    if (a == "performClose:")
        for (Window *panel : {m_find.window(), m_prefs.get(), m_about.get()})
            if (panel && panel->isKey())
                return panel->close();
    Document *d = mainDocument();
    if (!d)
        return;
    TextView *tv = d->textView();
    if (a == "copyFont:")
        m_copiedStyle = tv->styleAtSelection();
    else if (a == "pasteFont:") {
        if (m_copiedStyle)
            tv->changeStyle([s = *m_copiedStyle](Style &x) {
                const Alignment keep = x.align;
                x = s;
                x.align = keep;
            });
    } else
        d->perform(it);
}

void Controller::reviewAndQuit(size_t from)
{
    // NSDocumentController's reviewUnsavedDocumentsWithAlertTitle:...: each edited document asks in
    // turn; a Cancel stops the quitting
    std::vector<Document *> edited;
    for (Document *d : m_order)
        if (d->isEdited() && !(d->isTransient() && d->textView()->text().size() == 0))
            edited.push_back(d);
    if (from >= edited.size() || edited.empty()) {
        App::shared().quit();
        return;
    }
    edited[0]->performClose([this](bool closed) {
        if (closed)
            App::shared().later([this] { reviewAndQuit(0); });
    });
}

void Controller::quit()
{
    const size_t count = size_t(std::count_if(m_order.begin(), m_order.end(), [](Document *d) { return d->isEdited(); }));
    if (count <= 1)
        return reviewAndQuit(0);
    // "You have %1$@ %2$@ documents with unsaved changes. Do you want to review these changes before
    // quitting?" Review Changes…, Cancel, Discard Changes
    Alert::run(m_icon,
               format(docString("You have %1$@ %2$@ documents with unsaved changes. Do you want to review these changes before quitting?",
                                "You have %1$@ %2$@ documents with unsaved changes. Do you want to review these changes before quitting?"),
                      {std::to_string(count), "TextEdit"}),
               docString("If you don\\U2019t review your documents, all your changes will be lost.",
                         "If you don’t review your documents, all your changes will be lost."),
               {docString("Review Changes\\U2026", "Review Changes…"), docString("Cancel", "Cancel"), docString("Discard Changes", "Discard Changes")},
               [this](int b) {
                   if (b == 0)
                       reviewAndQuit(0);
                   else if (b == 2)
                       App::shared().quit();
               });
}

} // namespace

} // namespace TextEdit

int main(int argc, char **argv)
{
    using namespace TextEdit;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-')
            files.push_back(argv[i]);
    // a TextEdit already running opens them (NSApplication's openFiles)
    if (Instance::handToRunning("textedit", files))
        return 0;
    App &app = App::shared();
    setProgramName("TextEdit");
    if (!app.open()) {
        std::fprintf(stderr, "TextEdit: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    FontManager::registerAllFonts();
    if (!Assets::exists(std::string(Bundle) + "/Contents/Resources/English.lproj/Edit.nib")) {
        std::fprintf(stderr, "TextEdit: its files are not in the assets (tools/extract.py takes them from the DVD)\n");
        return 1;
    }
    static Controller controller;
    controller.start(files);
    Instance::listen("textedit", [&app](std::vector<std::string> paths) {
        if (paths.empty()) {
            // a launch without files: come forward, a new Untitled document when none is open
            if (!controller.mainDocument())
                controller.newDocument(false);
            else
                controller.mainDocument()->window()->show();
            return;
        }
        controller.openFiles(paths);
        (void)app;
    });
    // a session ending (SIGTERM): quits at once, as the Mac's logging out after asking
    const int code = app.run();
    app.setFrontWithoutWindows(false);
    return code;
}
