// SPDX-License-Identifier: LGPL-2.1-or-later
// Help Viewer.app (5.0.4, 190.2.1) as 10.6.8's (its x86_64 code, docs/reverse-status.md, section 23): the
// help books of the applications (Contents/Resources/*.help) and Mac Help
// (/Library/Documentation/Help/MacHelp.help), their pages drawn by a web engine as WebKit drew them.
//   The window (-[HelpWindowController windowDidLoad]): the floating level (CGWindowLevelForKey 5),
//   key only when needed; a toolbar (HelpViewToolbarController -_setupSegmentedButtons): back and
//   forward (a textured segmented control, 23 points a segment, menus of the history), Home (its menu the
//   library of books), the action menu (HelpActionMenu.nib: Make Text Smaller, Make Text Bigger, Find,
//   Print…), the spinner, the search field "Ask a Question". Mac Help when nothing else is asked.
//   No Dock icon, no menus of its own (LSUIElement).
//   help: URLs (HVAnchorURLHandler, HVSearchURLHandler): openbook=, anchor= (the page with that named
//   anchor, a list when several have it), topic_list= (the pages with the anchor, through the book's
//   template, an XQuery, its stylesheet and the headline Other=), search= and searchall= (the pages'
//   titles, keywords, descriptions and text, through Help Viewer's searchResultTemplate.xquery and
//   resultList.xquery). The books' .helpindex (Apple's search index) is not read: their pages are.
//
//   HelpViewer [help:URL | --book <bookID> [--anchor <anchor>]]
// A second launch hands its URL to the running one.
#include "aqua.h"
#include "assets.h"
#include "x11.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QBuffer>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QPainterPath>
#include <QPrinter>
#include <QPrintDialog>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QPushButton>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>

namespace {

const QString Bundle = QStringLiteral("/System/Library/CoreServices/HelpViewer.app");
QString res(const QString &name) { return Assets::path(Bundle + "/Contents/Resources/" + name); }
QString loc(const char *key) { return Assets::tr(Bundle, "Localizable", key, key); }

QString escapeHtml(const QString &s) { return s.toHtmlEscaped(); }

// ---- the books

struct Page {
    QString file, title, description, keywords, text;
};

struct Book {
    QString id, title, root;   // root: its English.lproj
    QString access, icon;      // HPDBookAccessPath (in root), HPDBookIconPath (in Resources)
    bool indexed = false;
    QVector<Page> pages;
    QHash<QString, QVector<int>> anchors;   // named anchor → pages
};

QJsonObject jsonOf(const QString &file)
{
    QFile f(file + ".json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

class Library {
public:
    Library()
    {
        // Mac Help first, then the applications' books
        QStringList bundles = {"/Library/Documentation/Help/MacHelp.help"};
        for (const QString &folder : {QStringLiteral("/Applications"), QStringLiteral("/Applications/Utilities"),
                                      QStringLiteral("/System/Library/CoreServices")}) {
            const QDir apps(Assets::path(folder));
            for (const QString &app : apps.entryList({"*.app"}, QDir::Dirs, QDir::Name)) {
                const QDir r(apps.filePath(app + "/Contents/Resources"));
                for (const QString &help : r.entryList({"*.help"}, QDir::Dirs))
                    bundles << folder + "/" + app + "/Contents/Resources/" + help;
            }
        }
        for (const QString &b : bundles) {
            const QString dir = Assets::path(b);
            const QJsonObject info = jsonOf(dir + "/Contents/Info.plist");
            if (info.isEmpty())
                continue;
            Book book;
            book.id = info["CFBundleIdentifier"].toString();
            book.title = info["HPDBookTitle"].toString();
            const QJsonObject strings = jsonOf(dir + "/Contents/Resources/English.lproj/InfoPlist.strings");
            if (strings.contains("HPDBookTitle"))
                book.title = strings["HPDBookTitle"].toString();
            book.root = dir + "/Contents/Resources/English.lproj";
            book.access = info["HPDBookAccessPath"].toString();
            book.icon = dir + "/Contents/Resources/" + info["HPDBookIconPath"].toString();
            if (!book.id.isEmpty() && QDir(book.root).exists())
                books.append(book);
        }
    }

    QVector<Book> books;

    Book *find(const QString &id)
    {
        for (Book &b : books)
            if (b.id.compare(id, Qt::CaseInsensitive) == 0)
                return &b;
        return nullptr;
    }

    // a book's pages: their named anchors, titles, meta description and keywords, text
    void index(Book &b)
    {
        if (b.indexed)
            return;
        b.indexed = true;
        static const QRegularExpression anchor(R"(<a\s+[^>]*name\s*=\s*["']([^"']+)["'])", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression title(R"(<title>(.*?)</title>)", QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression meta(R"(<meta\s+name\s*=\s*["'](description|keywords)["']\s+content\s*=\s*["']([^"']*)["'])", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tags(R"(<[^>]*>)"), spaces(R"(\s+)");
        QDirIterator it(b.root, {"*.html", "*.htm"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            QFile f(file);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QString html = QString::fromUtf8(f.readAll());
            Page p;
            p.file = file;
            p.title = title.match(html).captured(1).trimmed();
            for (auto m = meta.globalMatch(html); m.hasNext();) {
                const auto mm = m.next();
                (mm.captured(1).toLower() == "description" ? p.description : p.keywords) = mm.captured(2);
            }
            QString body = html.mid(html.indexOf("<body", 0, Qt::CaseInsensitive));
            p.text = body.remove(tags).replace(spaces, " ").toLower();
            const int at = b.pages.size();
            for (auto m = anchor.globalMatch(html); m.hasNext();)
                b.anchors[m.next().captured(1)].append(at);
            b.pages.append(p);
        }
    }

    // HVSearchURLHandler: the pages that have every word, best first (the title, then the keywords
    // and the description, then the text)
    QVector<QPair<Book *, int>> search(const QString &query, Book *only)
    {
        const QStringList words = query.toLower().split(QRegularExpression("\\W+"), Qt::SkipEmptyParts);
        QVector<std::tuple<int, Book *, int>> scored;
        for (Book &b : books) {
            if (only && &b != only)
                continue;
            index(b);
            for (int i = 0; i < b.pages.size(); ++i) {
                const Page &p = b.pages[i];
                if (p.title.isEmpty() || p.file.endsWith(b.access))
                    continue;
                int score = 0;
                bool all = true;
                for (const QString &w : words) {
                    const int t = p.title.toLower().count(w), k = (p.keywords + " " + p.description).toLower().count(w), x = p.text.count(w);
                    if (t + k + x == 0) {
                        all = false;
                        break;
                    }
                    score += t * 20 + k * 6 + std::min(x, 10);
                }
                if (all && !words.isEmpty())
                    scored.append({score, &b, i});
            }
        }
        std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &c) { return std::get<0>(a) > std::get<0>(c); });
        QVector<QPair<Book *, int>> out;
        for (const auto &[s, b, i] : scored)
            out.append({b, i});
        return out;
    }
};

// ---- XQuery, as much as the templates use: external variables put in, a FLWOR's for ... return with
// its if ... then ... else, data($item/field)

struct Item {
    QHash<QString, QString> fields;
};

QString renderXQuery(QString text, const QHash<QString, QString> &values, const QHash<QString, QString> &rawValues,
                     const QVector<Item> &items)
{
    // the prolog and the comments out
    text.remove(QRegularExpression(R"(\(:.*?:\))", QRegularExpression::DotMatchesEverythingOption));
    text.remove(QRegularExpression(R"(declare\s+variable[^;]*;)"));
    // the FLWOR: { for $x in $list return BODY }
    static const QRegularExpression flwor(R"(\{\s*for\s+\$(\w+)\s+in\s+\$\w+\s+return\s+(.*)\}\s*(?=</))",
                                          QRegularExpression::DotMatchesEverythingOption);
    const auto m = flwor.match(text);
    if (m.hasMatch()) {
        const QString var = m.captured(1);
        QString body = m.captured(2).trimmed(), thenPart = body, elsePart = body, cond;
        static const QRegularExpression ifThenElse(R"(^if\s*\((.*?)\)\s*then\s*(.*?)\s*else\s*(.*)$)", QRegularExpression::DotMatchesEverythingOption);
        if (const auto c = ifThenElse.match(body); c.hasMatch()) {
            cond = c.captured(1);
            thenPart = c.captured(2);
            elsePart = c.captured(3);
        }
        static const QRegularExpression condition(R"re(\$\w+/@(\w+)\s*=\s*"([^"]*)")re");
        const auto cm = condition.match(cond);
        QString out;
        for (const Item &item : items) {
            QString part = cm.hasMatch() && item.fields.value(cm.captured(1)) == cm.captured(2) ? thenPart : elsePart;
            if (!cm.hasMatch())
                part = thenPart;
            static const QRegularExpression data(R"(\{data\(\$\w+/(\w+)\)\})");
            QString filled;
            int last = 0;
            for (auto d = data.globalMatch(part); d.hasNext();) {
                const auto dm = d.next();
                filled += part.mid(last, dm.capturedStart() - last) + escapeHtml(item.fields.value(dm.captured(1)));
                last = dm.capturedEnd();
            }
            filled += part.mid(last);
            Q_UNUSED(var);
            out += filled + "\n";
        }
        text.replace(m.capturedStart(), m.capturedLength(), out);
    }
    for (auto it = rawValues.begin(); it != rawValues.end(); ++it)
        text.replace("{$" + it.key() + "}", it.value());
    for (auto it = values.begin(); it != values.end(); ++it)
        text.replace("{$" + it.key() + "}", escapeHtml(it.value()));
    return text.trimmed();
}

QString readText(const QString &file)
{
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// a generated page, beside nothing: written into the cache, its base the book's folder
QUrl writePage(const QString &name, QString html, const QString &base)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/snowleopard/helpviewer";
    QDir().mkpath(dir);
    if (!base.isEmpty()) {
        const QString tag = "<base href=\"" + QUrl::fromLocalFile(base + "/").toString() + "\"/>";
        const int head = html.indexOf("<head>", 0, Qt::CaseInsensitive);
        html = head >= 0 ? html.insert(head + 6, tag) : tag + html;
    }
    const QString file = dir + "/" + name;
    QFile f(file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(html.toUtf8());
    QUrl u = QUrl::fromLocalFile(file);
    u.setQuery(QString::number(QDateTime::currentMSecsSinceEpoch()));   // a new page each time
    return u;
}

// "help:anchor='access' bookID=com.apple.Foo.help" → its parameters
QHash<QString, QString> parseHelpURL(QString s)
{
    s = QUrl::fromPercentEncoding(s.toUtf8());
    if (s.startsWith("help:"))
        s = s.mid(5);
    while (s.startsWith('/'))
        s = s.mid(1);
    QHash<QString, QString> out;
    static const QRegularExpression pair(R"((\w+)=(.*?)(?=\s+\w+=|$))", QRegularExpression::DotMatchesEverythingOption);
    for (auto m = pair.globalMatch(s); m.hasNext();) {
        const auto mm = m.next();
        QString v = mm.captured(2).trimmed();
        if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
            v = v.mid(1, v.size() - 2);
        out.insert(mm.captured(1).toLower(), v);
    }
    return out;
}

// ---- the pages' scroll bars

// WebKit's WebView drew AppKit's scroll bars: Aqua's, the arrows together at the bottom. Chromium's
// own ::-webkit-scrollbar parts get AquaStyle's pictures: the track (its ends kept, its middle
// repeated), the thumb (blue in the key window, clear otherwise: :window-inactive), the up and down
// arrows under the track. Vertical only, as AquaStyle draws them.
QString aquaScrollbarStyle()
{
    QStyle *style = QApplication::style();
    constexpr int W = 15, H = 240, Up = 14, Down = 16, Groove = H - Up - Down;
    auto render = [&](bool active, int page, int position) {
        QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QStyleOptionSlider o;
        o.rect = QRect(0, 0, W, H);
        o.orientation = Qt::Vertical;
        o.minimum = 0;
        o.maximum = 1000;
        o.pageStep = page;
        o.sliderPosition = o.sliderValue = position;
        o.state = QStyle::State_Enabled | (active ? QStyle::State_Active : QStyle::State_None);
        QPainter p(&img);
        style->drawComplexControl(QStyle::CC_ScrollBar, &o, &p, nullptr);
        p.end();
        QStyleOptionSlider sized = o;
        return std::make_pair(img, style->subControlRect(QStyle::CC_ScrollBar, &sized, QStyle::SC_ScrollBarSlider, nullptr));
    };
    auto url = [](const QImage &img) {
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        return QStringLiteral("url(data:image/png;base64,%1)").arg(QString::fromLatin1(png.toBase64()));
    };
    // a part: its two ends in its transparent top and bottom borders, its middle repeated between
    auto piece = [&](const char *part, const QString &when, const QString &top, const QString &bottom, const QString &middle, int end,
                     int minHeight) {
        return QStringLiteral("::-webkit-scrollbar-%1%2 { min-height: %3px; border-style: solid; border-color: transparent; "
                              "border-width: %4px 0; background: %5 top / %6px %4px no-repeat border-box border-box, "
                              "%7 bottom / %6px %4px no-repeat border-box border-box, %8 0 0 / %6px 1px repeat-y padding-box padding-box; }\n")
            .arg(QString::fromLatin1(part), when).arg(minHeight).arg(end).arg(top).arg(W).arg(bottom, middle);
    };
    QString css = QStringLiteral("::-webkit-scrollbar { width: %1px; height: 0; }\n"
                                 "::-webkit-scrollbar-button { display: none; }\n"
                                 "::-webkit-scrollbar-corner { background: transparent; }\n").arg(W);
    for (bool active : {true, false}) {
        const QString when = active ? QString() : QStringLiteral(":window-inactive");
        // the track, free of the thumb: its top from a thumb at the bottom, its bottom (with the
        // arrows' cap over its end) from a thumb at the top
        const auto [low, lowThumb] = render(active, 1, 1000);
        const auto [high, highThumb] = render(active, 1, 0);
        const int cap = 24;
        const QImage trackTop = low.copy(0, 0, W, cap), trackBottom = high.copy(0, Groove - cap, W, cap),
                     trackMiddle = low.copy(0, cap, W, 1);
        css += piece("track", when, url(trackTop), url(trackBottom), url(trackMiddle), cap, 0);
        css += QStringLiteral("::-webkit-scrollbar-button:vertical:end:decrement%1 { display: block; height: %2px; background: %3; }\n"
                              "::-webkit-scrollbar-button:vertical:end:increment%1 { display: block; height: %4px; background: %5; }\n")
                   .arg(when).arg(Up).arg(url(high.copy(0, Groove, W, Up))).arg(Down).arg(url(high.copy(0, Groove + Up, W, Down)));
        // the thumb: its ends and a row of its middle, from a long one
        const auto [thumb, rect] = render(active, 1000, 0);
        const int end = 12;
        css += piece("thumb", when, url(thumb.copy(rect.x(), rect.y(), W, end)), url(thumb.copy(rect.x(), rect.bottom() + 1 - end, W, end)),
                     url(thumb.copy(rect.x(), rect.y() + end, W, 1)), end, 2 * end);
    }
    return css;
}

// ---- the window

class HelpPage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
    std::function<void(const QUrl &)> onHelpURL;

protected:
    // the pages' help: links, which Chromium won't follow from a file: page: told by the script below
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &message, int, const QString &) override
    {
        static const QString prefix = QStringLiteral("snowleopard-help:");
        if (message.startsWith(prefix) && onHelpURL) {
            const QUrl url(message.mid(prefix.size()));
            QTimer::singleShot(0, [this, url] { onHelpURL(url); });
        }
    }

    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool) override
    {
        if (url.scheme() == "help") {
            if (onHelpURL)
                QTimer::singleShot(0, [this, url] { onHelpURL(url); });
            return false;
        }
        return true;
    }
};

// help: loaded directly (not through a link): handed to the window the same way
class HelpScheme : public QWebEngineUrlSchemeHandler {
public:
    std::function<void(const QUrl &)> onHelpURL;
    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QUrl url = job->requestUrl();
        job->fail(QWebEngineUrlRequestJob::RequestAborted);
        if (onHelpURL)
            QTimer::singleShot(0, [this, url] { onHelpURL(url); });
    }
};

// NSSegmentedControl, NSSegmentStyleTexturedRounded (style 2, -_setupSegmentedButtons): AquaStyle's
// textured segment art (CoreUI's "segment" "fortexture"); side by side ones share their edges
class Segment : public QPushButton {
public:
    Segment(const QImage &image, const QString &tip, const QString &position, int width, bool menuIndicator = false)
        : m_menuIndicator(menuIndicator)
    {
        setProperty("aquaTextured", true);
        if (!position.isEmpty())
            setProperty("aquaSegmentPosition", position);
        setToolTip(tip);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(width, 24);
        m_image = image;
    }
    // -setMenu:forSegment: with the indicator: a press on the arrow, or held, shows it
    QMenu *menu = nullptr;
    std::function<void()> action;

protected:
    void paintEvent(QPaintEvent *e) override
    {
        QPushButton::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QImage img = m_image;
        if (!isEnabled()) {   // a disabled segment's image at half
            img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter ip(&img);
            ip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            ip.fillRect(img.rect(), QColor(0, 0, 0, 110));
        }
        const int iw = std::min(img.width(), 16), ih = std::min(img.height(), 16);
        const int x = m_menuIndicator ? (width() - 8 - iw) / 2 : (width() - iw) / 2;
        p.drawImage(QRect(x, (height() - ih) / 2, iw, ih), img);
        if (m_menuIndicator) {   // NSSegmentedCell's menu indicator: a small black triangle
            QPainterPath t;
            const double cx = width() - 8, cy = height() / 2.0 + 1;
            t.moveTo(cx - 2.5, cy - 1.5);
            t.lineTo(cx + 2.5, cy - 1.5);
            t.lineTo(cx, cy + 1.5);
            t.closeSubpath();
            p.setRenderHint(QPainter::Antialiasing);
            p.fillPath(t, isEnabled() ? QColor(0, 0, 0, 200) : QColor(0, 0, 0, 90));
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        QPushButton::mousePressEvent(e);
        if (menu && (!action || e->position().x() > width() - 12))
            showMenu();
        else if (menu)
            m_hold.start();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        const bool held = m_hold.isValid() && m_hold.elapsed() > 450;
        m_hold.invalidate();
        QPushButton::mouseReleaseEvent(e);
        if (held && menu)
            showMenu();
        else if (action && rect().contains(e->position().toPoint()) && !(menu && e->position().x() > width() - 12))
            action();
    }

private:
    void showMenu()
    {
        setDown(false);
        menu->popup(mapToGlobal(QPoint(0, height() + 1)));
    }
    QImage m_image;
    bool m_menuIndicator;
    QElapsedTimer m_hold;
};

// NSActionTemplate: AppKit draws it (no picture on the DVD): the gear
QImage gear()
{
    QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    const QPointF c(8, 8);
    for (int i = 0; i < 8; ++i) {
        QPainterPath tooth;
        tooth.addRect(QRectF(-1.5, -7.5, 3, 3.2));
        QTransform t;
        t.translate(c.x(), c.y());
        t.rotateRadians(i * M_PI / 4);
        path |= t.map(tooth);
    }
    path.addEllipse(c, 5, 5);
    QPainterPath hole;
    hole.addEllipse(c, 2.1, 2.1);
    p.fillPath(path.subtracted(hole), QColor(50, 50, 50));
    return img;
}

// NSSearchField's magnifier with the arrow of its search menu (its cell's searchMenuTemplate)
QImage magnifier()
{
    QImage img(22, 14, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(90, 90, 90), 1.6));
    p.drawEllipse(QRectF(3, 2, 7, 7));
    p.drawLine(QPointF(9, 8.5), QPointF(12, 11.5));
    QPainterPath t;
    t.moveTo(14.5, 6);
    t.lineTo(19.5, 6);
    t.lineTo(17, 9);
    t.closeSubpath();
    p.setPen(Qt::NoPen);
    p.fillPath(t, QColor(90, 90, 90));
    return img;
}

// An icon-only, regular size NSToolbar (-initWithHelpViewController: setDisplayMode:2, setSizeMode:1)
// under a utility window's title bar, which it continues: a grey falling from 196 to 168 down its 33
// rows, then its baseline (a 10.6 screenshot of Safari Help's window; the inactive colours lighter)
class Toolbar : public QWidget {
public:
    using QWidget::QWidget;
    static constexpr int Height = 34;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const bool active = window()->isActiveWindow();
        QLinearGradient g(0, 0, 0, Height - 1);
        g.setColorAt(0, active ? QColor(196, 196, 196) : QColor(228, 228, 228));
        g.setColorAt(1, active ? QColor(168, 168, 168) : QColor(210, 210, 210));
        p.fillRect(QRect(0, 0, width(), Height - 1), g);
        p.fillRect(0, Height - 1, width(), 1, active ? QColor(101, 101, 101) : QColor(150, 150, 150));
    }
    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::ActivationChange)
            update();
        QWidget::changeEvent(e);
    }
};

class HelpWindow : public QWidget {
public:
    // HelpWindow.nib: an HVWindow, style 159 (titled, closable, miniaturizable, resizable, utility,
    // non-activating panel), 480 × 400, its content no smaller than 360 × 200; at the floating level
    HelpWindow(Library &library) : QWidget(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint), m_library(library)
    {
        setWindowTitle(loc("Help Viewer"));
        setWindowIcon(QIcon(QPixmap::fromImage(Assets::image(Bundle + "/Contents/Resources/HelpViewer.icns", QSize(128, 128)))));
        setAttribute(Qt::WA_ShowWithoutActivating);   // -setBecomesKeyOnlyIfNeeded:
        setAttribute(Qt::WA_X11NetWmWindowTypeUtility);   // NSUtilityWindowMask: the small title bar
        resize(480, 400);
        setMinimumSize(360, 200 + Toolbar::Height);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *bar = new Toolbar;
        bar->setFixedHeight(Toolbar::Height);
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(7, 4, 7, 5);
        row->setSpacing(0);
        // the default items: navButtons, homeButton, actionButton, a flexible space, the spinner, the
        // search field; 8 points between the items (the toolbar's own spacing)
        m_back = new Segment(Assets::image(Bundle + "/Contents/Resources/Back.tif"), loc("Back"), "left", 23);
        m_forward = new Segment(Assets::image(Bundle + "/Contents/Resources/Forward.tif"), loc("Forward"), "right", 23);
        row->addWidget(m_back);
        row->addWidget(m_forward);
        row->addSpacing(8);
        m_home = new Segment(Assets::image(Bundle + "/Contents/Resources/Home.tif"), loc("Go to the home page"), {}, 44, true);
        row->addWidget(m_home);
        row->addSpacing(8);
        m_action = new Segment(gear(), {}, {}, 46, true);
        row->addWidget(m_action);
        // NSSegmentedControl sized to its image and menu indicator: 44 and 46 points (measured on a
        // 10.6 screenshot: AppKit's -sizeToFit isn't read); then the flexible space, the spinner's
        // place (a small one, not displayed when stopped) and the search field, which grows first
        row->addStretch(1);
        row->addSpacing(8 + 16 + 8);
        m_search = new QLineEdit;
        m_search->setProperty("aquaSearch", true);
        m_search->setPlaceholderText(loc("Ask a Question"));
        m_search->setToolTip(loc("Type a word or phrase, then press Return"));
        m_search->setClearButtonEnabled(true);
        m_search->setFixedHeight(22);
        // -[NSToolbar _minimumSizeForSearchFieldToolbarItem] / _maximum…: 140 × 22 to 200 × 22
        m_search->setMinimumWidth(140);
        m_search->setMaximumWidth(200);
        auto *scope = m_search->addAction(QIcon(QPixmap::fromImage(magnifier())), QLineEdit::LeadingPosition);
        connect(scope, &QAction::triggered, this, [this] { scopeMenu()->popup(m_search->mapToGlobal(QPoint(0, m_search->height()))); });
        row->addWidget(m_search, 100);
        layout->addWidget(bar);

        m_view = new QWebEngineView;
        m_page = new HelpPage(m_view);
        m_page->onHelpURL = [this](const QUrl &u) { open(u.toString()); };
        m_view->setPage(m_page);
        // a click on a help: link (WebKit's WebView gave Help Viewer its navigation): to the page above
        QWebEngineScript links;
        links.setName("help-links");
        links.setInjectionPoint(QWebEngineScript::DocumentCreation);
        links.setWorldId(QWebEngineScript::MainWorld);
        links.setRunsOnSubFrames(true);
        links.setSourceCode(QStringLiteral(R"js(
document.addEventListener('click', function (e) {
    var a = e.target.closest ? e.target.closest('a') : null;
    var href = a ? a.getAttribute('href') : null;
    if (href && href.indexOf('help:') === 0) {
        e.preventDefault();
        e.stopPropagation();
        console.log('snowleopard-help:' + href);
    }
}, true);
)js"));
        m_page->scripts().insert(links);
        QWebEngineScript scrollers;
        scrollers.setName("aqua-scrollbars");
        scrollers.setInjectionPoint(QWebEngineScript::DocumentReady);
        scrollers.setWorldId(QWebEngineScript::ApplicationWorld);
        scrollers.setRunsOnSubFrames(true);
        scrollers.setSourceCode(QStringLiteral("(function(){var s=document.createElement('style');s.textContent=%1;"
                                               "(document.head||document.documentElement).appendChild(s);})();")
                                    .arg(QString::fromUtf8(QJsonDocument(QJsonObject{{"c", aquaScrollbarStyle()}}).toJson(QJsonDocument::Compact))
                                             .section("\"c\":", 1).chopped(1)));
        m_page->scripts().insert(scrollers);
        m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
        m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        layout->addWidget(m_view, 1);

        // the find bar (-showFindBar:): at the bottom, Return finds the next
        m_find = new QLineEdit;
        m_find->setPlaceholderText("Find");
        m_find->hide();
        layout->addWidget(m_find);
        connect(m_find, &QLineEdit::returnPressed, this, [this] { m_page->findText(m_find->text()); });

        m_back->action = [this] { m_view->back(); };
        m_forward->action = [this] { m_view->forward(); };
        m_home->action = [this] { goHome(); };
        connect(m_view, &QWebEngineView::urlChanged, this, [this] { updateNav(); });
        connect(m_view, &QWebEngineView::titleChanged, this, [this] { updateTitle(); });
        connect(m_search, &QLineEdit::returnPressed, this, [this] {
            if (!m_search->text().trimmed().isEmpty())
                open((m_searchAll || !m_book ? "help:searchall=" : "help:search=") + m_search->text().trimmed() +
                     (m_book && !m_searchAll ? " bookID=" + m_book->id : QString()));
        });

        // Home's menu: the library (-_setLibraryMenuForHomeButton)
        auto *libraryMenu = new QMenu(this);
        for (const Book &b : m_library.books)
            libraryMenu->addAction(b.title, this, [this, id = b.id] { open("help:openbook=" + id); });
        m_home->menu = libraryMenu;
        // HelpActionMenu.nib
        auto *action = new QMenu(this);
        action->addAction("Make Text Smaller", this, [this] { m_view->setZoomFactor(std::max(0.5, m_view->zoomFactor() - 0.1)); });
        action->addAction("Make Text Bigger", this, [this] { m_view->setZoomFactor(std::min(3.0, m_view->zoomFactor() + 0.1)); });
        action->addSeparator();
        action->addAction("Find", QKeySequence("Ctrl+F"), this, [this] {
            m_find->show();
            m_find->setFocus();
        });
        action->addSeparator();
        action->addAction("Print…", this, [this] {
            auto *printer = new QPrinter;
            QPrintDialog dialog(printer, this);
            if (dialog.exec() == QDialog::Accepted)
                m_view->print(printer);
        });
        m_action->menu = action;
        updateNav();
    }

    // help: URLs and the books' pages
    void open(const QString &target)
    {
        if (!target.startsWith("help:")) {
            m_view->load(QUrl::fromUserInput(target));
            return;
        }
        const auto p = parseHelpURL(target);
        Book *book = m_library.find(p.value("bookid"));
        if (p.contains("openbook")) {
            book = m_library.find(p.value("openbook"));
            if (book)
                load(book, QUrl::fromLocalFile(book->root + "/" + book->access));
        } else if (p.contains("anchor")) {
            showAnchor(book, p.value("anchor"));
        } else if (p.contains("topic_list")) {
            showTopicList(book, p.value("topic_list"), p.value("template"), p.value("stylesheet"), p.value("other"));
        } else if (p.contains("search") || p.contains("searchall")) {
            const bool all = p.contains("searchall");
            showSearch(all ? nullptr : book, p.value(all ? "searchall" : "search"));
        }
        show();
        raise();
        activateWindow();
    }

    void goHome()
    {
        Book *b = m_book ? m_book : m_library.find("com.apple.machelp");
        if (b)
            load(b, QUrl::fromLocalFile(b->root + "/" + b->access));
    }

private:
    void load(Book *book, const QUrl &url)
    {
        m_book = book;
        m_view->load(url);
        updateTitle();
    }

    // HVAnchorURLHandler: the page with it, or the list of them; anchors in every book when no book
    void showAnchor(Book *book, const QString &anchor)
    {
        QVector<QPair<Book *, int>> found;
        for (Book &b : m_library.books) {
            if (book && &b != book)
                continue;
            m_library.index(b);
            for (int i : b.anchors.value(anchor))
                found.append({&b, i});
        }
        if (found.size() == 1 || (!found.isEmpty() && found[0].first->pages[found[0].second].file.endsWith(found[0].first->access))) {
            QUrl u = QUrl::fromLocalFile(found[0].first->pages[found[0].second].file);
            if (anchor != "access")
                u.setFragment(anchor);
            load(found[0].first, u);
            return;
        }
        if (found.isEmpty()) {
            noTopics(anchor);
            return;
        }
        showList(found[0].first, found, {}, {}, found[0].first->title);
    }

    void showTopicList(Book *book, const QString &anchor, const QString &templ, const QString &style, const QString &headline)
    {
        if (!book)
            return;
        m_library.index(*book);
        QVector<QPair<Book *, int>> found;
        for (int i : book->anchors.value(anchor))
            if (!book->pages[i].file.endsWith("/" + book->access))
                found.append({book, i});
        if (found.isEmpty()) {
            noTopics(headline);
            return;
        }
        showList(book, found, templ, style, headline);
    }

    // the book's template (an XQuery), else Help Viewer's topicListTmpl.htm
    void showList(Book *book, const QVector<QPair<Book *, int>> &found, const QString &templ, const QString &style, const QString &headline)
    {
        QVector<Item> items;
        for (const auto &[b, i] : found) {
            Item it;
            it.fields["url"] = QUrl::fromLocalFile(b->pages[i].file).toString();
            it.fields["title"] = b->pages[i].title;
            it.fields["summary"] = b->pages[i].description;
            items.append(it);
        }
        QString html;
        const QString xquery = templ.isEmpty() ? QString() : readText(book->root + "/" + templ);
        if (!xquery.isEmpty()) {
            html = renderXQuery(xquery, {{"AppleTopicListHeadline", headline}},
                                {{"AppleTopicListStyleSheetURL", QUrl::fromLocalFile(book->root + "/" + style).toString()}}, items);
        } else {
            QString t = readText(res("topicListTmpl.htm"));
            const int b = t.indexOf("<!-- AppleTopicListRowBegin -->"), e = t.indexOf("<!-- AppleTopicListRowEnd -->");
            const QString row = b >= 0 && e > b ? t.mid(b + 31, e - b - 31) : QString();
            QString rows;
            for (const Item &it : items)
                rows += QString(row).replace("AppleTopicListURL", it.fields["url"]).replace("AppleTopicListItemTitle", escapeHtml(it.fields["title"]))
                            .replace("AppleTopicListAbstract", escapeHtml(it.fields["summary"]));
            if (b >= 0 && e > b)
                t.replace(b, e + 29 - b, rows);
            html = t.replace("AppleTopicListCSS", QUrl::fromLocalFile(res("topicList.css")).toString());
        }
        load(book, writePage("TopicList.html", html, book->root));
    }

    // HVSearchURLHandler: searchResultTemplate.xquery, a resultList.xquery section, "Found: n Help Topics"
    void showSearch(Book *book, const QString &query)
    {
        const auto found = m_library.search(query, book);
        if (found.isEmpty()) {
            noTopics(query);
            return;
        }
        QVector<Item> items;
        for (int k = 0; k < found.size() && k < 200; ++k) {
            const auto &[b, i] = found[k];
            Item it;
            it.fields["url"] = QUrl::fromLocalFile(b->pages[i].file).toString();
            it.fields["title"] = b->pages[i].title;
            it.fields["summary"] = b->pages[i].description;
            it.fields["iconPath"] = QUrl::fromLocalFile(b->icon).toString();
            it.fields["target"] = "_self";
            it.fields["rowColor"] = k % 2 ? "blue" : "white";
            items.append(it);
        }
        const QString heading = loc(found.size() == 1 ? "Help Topic" : "Help Topics");
        const QString section = renderXQuery(readText(res("resultList.xquery")), {{"sectionID", "helpbook"}, {"sectionTitle", heading}}, {}, items);
        const QString count = QString(loc(found.size() == 1 ? "%@ Help Topic" : "%@ Help Topics")).replace("%@", QString::number(found.size()));
        const QString html = renderXQuery(readText(res("searchResultTemplate.xquery")),
                                          {{"pageTitle", QString(loc("Search Results: %@")).replace("%@", query)}, {"foundText", loc("Found:") + " " + count}},
                                          {{"cssPath", QUrl::fromLocalFile(res("searchResultTemplate.css")).toString()},
                                           {"jsPath", QUrl::fromLocalFile(res("searchResultTemplate.js")).toString()},
                                           {"resultSections", section}},
                                          {});
        m_book = book ? book : m_book;
        m_view->load(writePage("SearchResults.html", html, {}));
        updateTitle();
    }

    void noTopics(const QString &query)
    {
        QString html = readText(res("English.lproj/no_topics_found.html"));
        html.replace("[[QUERY]]", escapeHtml(query));
        m_view->load(writePage("no_topics_found.html", html, res("English.lproj")));
    }

    void updateNav()
    {
        m_back->setEnabled(m_view->history()->canGoBack());
        m_forward->setEnabled(m_view->history()->canGoForward());
    }

    // the window's title: the book's
    void updateTitle()
    {
        setWindowTitle(m_book ? m_book->title : loc("Help Viewer"));
    }

    // -_setupSearchField's scope menu: "Search <the book>", "Search All Books"
    QMenu *scopeMenu()
    {
        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        const QString book = m_book ? m_book->title : loc("Help Viewer");
        auto *current = menu->addAction(QString(loc("Search %@")).replace("%@", book), this, [this] { m_searchAll = false; });
        auto *all = menu->addAction(QString(loc("Search %@")).replace("%@", loc("All Books")), this, [this] { m_searchAll = true; });
        current->setCheckable(true);
        all->setCheckable(true);
        current->setChecked(!m_searchAll);
        all->setChecked(m_searchAll);
        return menu;
    }

    Library &m_library;
    Book *m_book = nullptr;
    bool m_searchAll = false;
    QWebEngineView *m_view;
    HelpPage *m_page;
    Segment *m_back, *m_forward, *m_home, *m_action;
    QLineEdit *m_search, *m_find;
};

QString socketName() { return "snowleopard-helpviewer-" + qEnvironmentVariable("USER") + "-" + qEnvironmentVariable("DISPLAY"); }

QString targetFromArguments(const QStringList &args)
{
    QString book, anchor, url;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--book" && i + 1 < args.size())
            book = args[++i];
        else if (args[i] == "--anchor" && i + 1 < args.size())
            anchor = args[++i];
        else
            url = args[i];
    }
    if (!url.isEmpty())
        return url;
    if (!anchor.isEmpty())
        return "help:anchor=" + anchor + " bookID=" + (book.isEmpty() ? "com.apple.machelp" : book);
    if (!book.isEmpty())
        return "help:openbook=" + book;
    return {};
}

} // namespace

int main(int argc, char **argv)
{
    QWebEngineUrlScheme scheme("help");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    scheme.setFlags(QWebEngineUrlScheme::LocalScheme | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
    Aqua::preinit();
    QApplication app(argc, argv);
    app.setApplicationName("Help Viewer");
    Aqua::init(app);
    const QString target = targetFromArguments(app.arguments());
    // one Help Viewer: a second launch gives its URL to the first
    {
        QLocalSocket s;
        s.connectToServer(socketName());
        if (s.waitForConnected(300)) {
            s.write((target.isEmpty() ? QString("home") : target).toUtf8() + "\n");
            s.waitForBytesWritten(1000);
            return 0;
        }
    }
    Library library;
    HelpWindow window(library);
    auto *handler = new HelpScheme;
    handler->onHelpURL = [&window](const QUrl &u) { window.open(u.toString()); };
    QWebEngineProfile::defaultProfile()->installUrlSchemeHandler("help", handler);
    QLocalServer::removeServer(socketName());
    QLocalServer server;
    server.listen(socketName());
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
        QLocalSocket *c = server.nextPendingConnection();
        QObject::connect(c, &QLocalSocket::readyRead, [c, &window] {
            const QString line = QString::fromUtf8(c->readAll()).trimmed();
            if (line == "home") {
                window.goHome();
                window.show();
                window.raise();
                window.activateWindow();
            } else {
                window.open(line);
            }
            c->deleteLater();
        });
    });
    if (target.isEmpty())
        window.goHome();
    else
        window.open(target);
    window.show();
    return app.exec();
}
