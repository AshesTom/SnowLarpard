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
#include <QPainter>
#include <QPainterPath>
#include <QPrinter>
#include <QPrintDialog>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
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

// the textured segments of the toolbar (NSSegmentedControl, style 2): Aqua's buttons side by side
QToolButton *segment(const QImage &image, const QString &tip)
{
    auto *b = new QToolButton;
    b->setIcon(QIcon(QPixmap::fromImage(image)));
    b->setIconSize(QSize(16, 16));
    b->setToolTip(tip);
    b->setFixedSize(23, 23);
    b->setAutoRaise(false);
    return b;
}

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
        const double a = i * M_PI / 4;
        QPainterPath tooth;
        tooth.addRect(QRectF(-1.6, -7.5, 3.2, 3.5));
        QTransform t;
        t.translate(c.x(), c.y());
        t.rotateRadians(a);
        path |= t.map(tooth);
    }
    path.addEllipse(c, 5.2, 5.2);
    QPainterPath hole;
    hole.addEllipse(c, 2.2, 2.2);
    p.fillPath(path.subtracted(hole), QColor(64, 64, 64));
    return img;
}

class Toolbar : public QWidget {
public:
    using QWidget::QWidget;
    static constexpr int Height = 33;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const bool active = window()->isActiveWindow();
        const int total = 22 + Height;
        for (int y = 0; y < Height - 1; ++y)
            p.fillRect(0, y, width(), 1, X11::unifiedToolbarColor(22 + y, total, active));
        p.fillRect(0, Height - 1, width(), 1, active ? QColor(81, 81, 81) : QColor(135, 135, 135));
    }
};

class HelpWindow : public QWidget {
public:
    HelpWindow(Library &library) : QWidget(nullptr, Qt::Window | Qt::WindowStaysOnTopHint), m_library(library)
    {
        setWindowTitle(loc("Help Viewer"));
        setWindowIcon(QIcon(QPixmap::fromImage(Assets::image(Bundle + "/Contents/Resources/HelpViewer.icns", QSize(128, 128)))));
        resize(480, 600);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *bar = new Toolbar;
        bar->setFixedHeight(Toolbar::Height);
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(8, 4, 8, 6);
        row->setSpacing(0);
        m_back = segment(Assets::image(Bundle + "/Contents/Resources/Back.tif"), loc("Back"));
        m_forward = segment(Assets::image(Bundle + "/Contents/Resources/Forward.tif"), loc("Forward"));
        row->addWidget(m_back);
        row->addWidget(m_forward);
        row->addSpacing(8);
        m_home = segment(Assets::image(Bundle + "/Contents/Resources/Home.tif"), loc("Go to the home page"));
        m_home->setFixedWidth(34);
        m_home->setPopupMode(QToolButton::MenuButtonPopup);
        row->addWidget(m_home);
        row->addSpacing(8);
        m_action = segment(gear(), {});
        m_action->setFixedWidth(34);
        m_action->setPopupMode(QToolButton::InstantPopup);
        row->addWidget(m_action);
        row->addStretch(1);
        m_search = new QLineEdit;
        m_search->setPlaceholderText(loc("Ask a Question"));
        m_search->setClearButtonEnabled(true);
        m_search->setMinimumWidth(110);
        m_search->setMaximumWidth(220);
        row->addWidget(m_search);
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
        m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
        m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        layout->addWidget(m_view, 1);

        // the find bar (-showFindBar:): at the bottom, Return finds the next
        m_find = new QLineEdit;
        m_find->setPlaceholderText("Find");
        m_find->hide();
        layout->addWidget(m_find);
        connect(m_find, &QLineEdit::returnPressed, this, [this] { m_page->findText(m_find->text()); });

        connect(m_back, &QToolButton::clicked, m_view, &QWebEngineView::back);
        connect(m_forward, &QToolButton::clicked, m_view, &QWebEngineView::forward);
        connect(m_home, &QToolButton::clicked, this, [this] { goHome(); });
        connect(m_view, &QWebEngineView::urlChanged, this, [this] { updateNav(); });
        connect(m_view, &QWebEngineView::titleChanged, this, [this] { updateTitle(); });
        connect(m_search, &QLineEdit::returnPressed, this, [this] {
            if (!m_search->text().trimmed().isEmpty())
                open("help:search=" + m_search->text().trimmed() + (m_book ? " bookID=" + m_book->id : QString()));
        });

        // Home's menu: the library (-_setLibraryMenuForHomeButton)
        auto *libraryMenu = new QMenu(this);
        for (const Book &b : m_library.books)
            libraryMenu->addAction(b.title, this, [this, id = b.id] { open("help:openbook=" + id); });
        m_home->setMenu(libraryMenu);
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
        m_action->setMenu(action);
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

    Library &m_library;
    Book *m_book = nullptr;
    QWebEngineView *m_view;
    HelpPage *m_page;
    QToolButton *m_back, *m_forward, *m_home, *m_action;
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
