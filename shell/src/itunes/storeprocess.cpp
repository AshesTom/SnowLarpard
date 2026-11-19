// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunesStore: the iTunes Store's pages for iTunes 11.4 (docs/itunes-reverse.md, section 8), in a process
// of their own as WebKit2's web content process: iTunes 11 drew them with WebKit 1 in its own process
// (StoreWebKitViewController, ITWebKitViewMacOS); AquaKit has no web engine, so this one renders them
// with Qt's, off screen, and hands iTunes the pictures.
//   As iTunes 11.4 asks for them: its User-Agent ("iTunes/11.4 (Macintosh; OS X 10.6.8)" and
//   " AppleWebKit/…" for the pages, FUN_0096c93b), the store front its URL bag gives
//   (https://init.itunes.apple.com/bag.xml?ix=5: the response's X-Set-Apple-Store-Front, sent back as
//   X-Apple-Store-Front), the bag's "storeFront" page first. The pages' scripts talk to the client
//   through the JavaScript object `iTunes` (StoreiTunesJSObject: the tables at 0x227fe8, 0x229574,
//   0x229e9e, 0x22a2fc, 0x22a674; its window 0x22d996 / 0x22d8d2), rebuilt below with what iTunes 11.4
//   answers (platform "Mac", version "11.4", appBundleName "com.apple.iTunes", osBuildID the system's
//   build); its Storage objects (setValue, valueForKey, removeValue, removeAllItems) over the page's.
//   WebKit 1 in iTunes loaded the store's own cross-origin requests (client-api.itunes.apple.com): so
//   does this process, for the store's pages only.
//
//   iTunesStore <frame file>
// Standard input, a command a line: "size W H", "load URL", "home", "back", "forward", "reload",
// "mouse down|up|move X Y BUTTON CLICKS MODS", "wheel X Y DX DY", "key down|up KEYSYM MODS TEXT",
// "focus 0|1", "ack" (the last frame taken). Standard output: "frame W H" (the picture in the frame
// file, ARGB32 premultiplied, W × H × 4 bytes), "nav BACK FORWARD", "url URL", "loading 0|1",
// "protocol XML" (addProtocol's plist, its newlines as \n: the store's nav bar), "cursor SHAPE".
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QKeySequence>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QBuffer>
#include <QUrlQuery>
#include <QWebEngineView>
#include <QWheelEvent>
#include <QXmlStreamReader>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

const char *const BagURL = "https://init.itunes.apple.com/bag.xml?ix=5";

QString systemVersion() { return qEnvironmentVariable("SNOWLEOPARD_SYSTEM_VERSION", "10.6.8"); }
QString systemBuild() { return qEnvironmentVariable("SNOWLEOPARD_SYSTEM_BUILD", "10K549"); }
// FUN_0096c93b: "iTunes/" version " (Macintosh; OS X " system ")", and " AppleWebKit/" WebKit's for pages
QString userAgent(bool web)
{
    QString ua = "iTunes/11.4 (Macintosh; OS X " + systemVersion() + ")";
    if (web)
        ua += " AppleWebKit/534.59.10";   // 10.6.8's WebKit (Safari 5.1.10)
    return ua;
}

void say(const QString &line)
{
    std::fputs((line + "\n").toUtf8().constData(), stdout);
    std::fflush(stdout);
}

// A plist (XML) as JSON: the bag's dictionaries, arrays, strings, numbers, booleans
QJsonValue plistValue(QXmlStreamReader &x)
{
    const QString tag = x.name().toString();
    if (tag == "dict") {
        QJsonObject o;
        QString key;
        while (x.readNextStartElement()) {
            if (x.name() == QLatin1String("key"))
                key = x.readElementText();
            else
                o.insert(key, plistValue(x));
        }
        return o;
    }
    if (tag == "array") {
        QJsonArray a;
        while (x.readNextStartElement())
            a.append(plistValue(x));
        return a;
    }
    if (tag == "true" || tag == "false") {
        x.skipCurrentElement();
        return tag == "true";
    }
    const QString text = x.readElementText();
    if (tag == "integer")
        return text.toLongLong();
    if (tag == "real")
        return text.toDouble();
    if (tag == "data")
        return QString();
    return text;
}

QJsonValue parsePlist(const QByteArray &xml)
{
    QXmlStreamReader x(xml);
    while (x.readNextStartElement())
        if (x.name() != QLatin1String("plist"))
            return plistValue(x);
    return {};
}

// The JavaScript object `iTunes` as StoreiTunesJSObject gives it to the pages; `post` messages reach
// this process through the console (iTunesBridge:{json})
QString bridgeScript(const QJsonObject &bag, const QString &storefront)
{
    QString s = R"JS((function () {
  if (window.iTunes) return;
  var bag = %BAG%;
  var post = function (m) { console.log('iTunesBridge:' + JSON.stringify(m)); };
  function storage(s) {
    return {
      setValue: function (k, v) { try { s.setItem(k, v); } catch (e) {} },
      valueForKey: function (k) { try { return s.getItem(k); } catch (e) { return null; } },
      removeValue: function (k) { try { s.removeItem(k); } catch (e) {} },
      removeAllItems: function () { try { s.clear(); } catch (e) {} },
      getItem: function (k) { try { return s.getItem(k); } catch (e) { return null; } },
      setItem: function (k, v) { try { s.setItem(k, v); } catch (e) {} },
      removeItem: function (k) { try { s.removeItem(k); } catch (e) {} },
      clear: function () { try { s.clear(); } catch (e) {} },
      key: function (i) { try { return s.key(i); } catch (e) { return null; } },
      get length() { try { return s.length; } catch (e) { return 0; } }
    };
  }
  // the window object (0x22d996: id, data, open, the colours, modal, title, style, width, height;
  // close, resizeTo)
  var mainWindow = {
    id: 'main', data: null, open: true, modal: false, title: 'iTunes', style: 1,
    get width() { return window.innerWidth; }, get height() { return window.innerHeight; },
    WindowStyleDocument: 1, WindowStyleSheet: 2, WindowStylePopover: 3, WindowStyleApplicationDefinedPopover: 4,
    close: function () {}, resizeTo: function () {}, addEventListener: function () {}, removeEventListener: function () {}
  };
  var none = function () { return null; };
  var it = {
    version: '11.4', platform: 'Mac', appBundleName: 'com.apple.iTunes', osBuildID: '%BUILD%',
    systemVersion: function () { return '%SYSTEM%'; }, storefront: '%STOREFRONT%',
    acceptedLanguages: navigator.language || 'en', appIsFrontmost: true, screenReaderRunning: false,
    currentPlayerState: 0, currentPlayingTrack: null, currentChapter: null, currentTime: 0, volume: 1, isMuted: false,
    isInFullscreen: false, isWebkitInFullscreen: false, isHDCPAvailable: false, isCoverFlowAvailable: true,
    isCoreMediaAvailable: true, quickTimeVersion: '7.6.6', canPlay1080pVideo: false, usedTempPlaylists: 0,
    availableTempPlaylists: 0, extras2Supported: true, has4GBLimit: false, waveform: null,
    geniusCUID: '', geniusOptInState: 0, cloudID: '', cameraRedeemSupported: false,
    primaryAccount: null, isAccountManaged: false, referringUserAgent: '',
    NavBarColorThemeLight: 0, NavBarColorThemeDark: 1, navbarColorTheme: 0, PlayingState: 1,
    window: mainWindow, localStorage: storage(window.localStorage), sessionStorage: storage(window.sessionStorage),
    deviceLocalStorage: storage(window.localStorage), preferences: {}, storeBag: bag,
    history: {
      back: function () { post({ back: 1 }); }, forward: function () { post({ forward: 1 }); },
      get length() { return history.length; }, canGoBack: function () { return history.length > 1; },
      removeCurrentPage: function () {}, clear: function () {}
    },
    getBag: function () { return bag; },
    getWindowById: function (id) { return id == 'main' ? mainWindow : null; },
    getMachineGUID: function () { return '%GUID%'; }, getClientGUID: function () { return '%GUID%'; },
    getMachineID: function () { return '%GUID%'; }, getMachineName: function () { return 'Mac'; },
    getSerialNumber: function () { return ''; }, getPreferences: function () { return {}; },
    getUserAppleID: function () { return ''; }, getUserDSID: function () { return ''; },
    getUserFullName: function () { return ''; }, getUserFirstName: function () { return ''; },
    getUserLastName: function () { return ''; }, getCloudState: function () { return 0; },
    getStoreItemStatus: function () { return 0; }, getDevices: function () { return []; },
    getPlaylists: function () { return []; }, getHistory: function () { return []; },
    findTracksByStoreID: function () { return []; }, findTracksByXID: function () { return []; },
    findTracksByTextFields: function () { return []; }, findPlaylistsByName: function () { return []; },
    signStorePlatformRequestData: function () { return ''; },
    addProtocol: function (p) { post({ protocol: String(p) }); },
    openURL: function (u) { post({ openURL: String(u) }); },
    openURLInBrowser: function (u) { post({ browser: String(u) }); },
    goToLibrary: function () { post({ library: 1 }); },
    playURL: function (u) { post({ play: u && u.url ? u.url : String(u) }); }, playURLs: function () {},
    pause: function () { post({ pause: 1 }); }, stop: function () { post({ pause: 1 }); }, resume: function () {},
    log: function () {}, recordEvent: function () {}, reportClickEvent: function () {},
    addEventListener: function () {}, removeEventListener: function () {}, dispatchGlobalEvent: function () {},
    showNavBarButton: function () {}, hideNavBarButton: function () {}, openNavBarPopover: function () {},
    setPageInset: function () {}, flagFullHeightContent: function () {}, setSidebarTitle: function () {},
    startProgressSpinner: function () { post({ spinner: 1 }); }, stopProgressSpinner: function () { post({ spinner: 0 }); },
    triggerPostPurchaseLibraryUpdate: function () {}, disableHistoryNavigation: function () {},
    allowDisplaySleep: function () {}, preventDisplaySleep: function () {}, flushGarbageCollector: function () {},
    stringCompare: function (a, b) { return a < b ? -1 : a > b ? 1 : 0; },
    createXHR: function () { return new XMLHttpRequest(); },
    // what needs an account (sign in, buy, rent, gifts): not available
    buy: none, rent: none, signIn: function () { post({ signIn: 1 }); }, signOut: none,
    doDialogXML: none, showExplicitDialog: none, showRestrictedDialog: none, showSharingPicker: none
  };
  // what a page asks for that iTunes 11.4 has and this doesn't: undefined, as for an older client
  window.iTunes = it;
})();)JS";
    s.replace("%BAG%", QString::fromUtf8(QJsonDocument(bag).toJson(QJsonDocument::Compact)));
    s.replace("%STOREFRONT%", storefront);
    s.replace("%SYSTEM%", systemVersion());
    s.replace("%BUILD%", systemBuild());
    s.replace("%GUID%", QString(QSysInfo::machineUniqueId().toHex().left(16).toUpper()));
    return s;
}

// The store's catalog lookups (StorePlatform's client-api.itunes.apple.com/…/lookup, "lockup" and
// "product" profiles): their requests are signed by the client (X-JS-SP-TOKEN,
// signStorePlatformRequestData), which this process doesn't do. For a proof of concept their answers
// come from Apple's public lookup API instead (itunes.apple.com/lookup, no signature), made into
// the shape the pages read: {results: {id: {id, kind, name, artistName, url, artwork: {url
// "…/{w}x{h}bb.{f}", width, height}, offers, …}}, version: 2} (its.sf.StorePlatform's
// _cacheStorePlatformData; its.sf6.Artwork's _bestArtworkForSizeWithV2StorePlatformArtwork).
const char *const CatalogScheme = "itunes-catalog";

QString platformKind(const QJsonObject &r)
{
    const QString wrapper = r.value("wrapperType").toString(), kind = r.value("kind").toString();
    if (wrapper == "collection")
        return r.value("collectionType").toString() == "TV Season" ? "tvSeason" : "album";
    if (wrapper == "artist")
        return "artist";
    if (wrapper == "audiobook")
        return "audiobook";
    if (kind == "song")
        return "song";
    if (kind == "music-video")
        return "musicVideo";
    if (kind == "feature-movie")
        return "movie";
    if (kind == "tv-episode")
        return "tvEpisode";
    if (kind == "podcast")
        return "podcast";
    if (kind == "ebook")
        return "book";
    if (kind == "software" || kind == "mac-software")
        return "iosSoftware";
    return kind.isEmpty() ? QString("album") : kind;
}

QJsonObject platformItem(const QJsonObject &r)
{
    const QString wrapper = r.value("wrapperType").toString();
    const bool collection = wrapper == "collection";
    QJsonObject o;
    const qint64 id = collection ? r.value("collectionId").toVariant().toLongLong()
                      : wrapper == "artist" ? r.value("artistId").toVariant().toLongLong()
                                            : r.value("trackId").toVariant().toLongLong();
    o["id"] = QString::number(id);
    o["kind"] = platformKind(r);
    o["name"] = collection ? r.value("collectionName").toString() : r.value("trackName").toString(r.value("artistName").toString());
    o["artistName"] = r.value("artistName").toString();
    o["artistId"] = QString::number(r.value("artistId").toVariant().toLongLong());
    o["artistUrl"] = r.value("artistViewUrl").toString();
    o["url"] = collection ? r.value("collectionViewUrl").toString() : r.value("trackViewUrl").toString(r.value("artistLinkUrl").toString());
    if (!r.value("collectionName").toString().isEmpty() && !collection)
        o["collectionName"] = r.value("collectionName").toString();
    o["releaseDate"] = r.value("releaseDate").toString().left(10);
    o["genreNames"] = QJsonArray{r.value("primaryGenreName").toString()};
    o["trackCount"] = r.value("trackCount").toInt();
    o["contentRatingsBySystem"] = QJsonObject{};
    if (wrapper == "track") {
        o["trackNumber"] = r.value("trackNumber").toInt();
        o["discNumber"] = r.value("discNumber").toInt(1);
        o["duration"] = r.value("trackTimeMillis").toInt() / 1000;
        o["durationInMilliseconds"] = r.value("trackTimeMillis").toInt();
        o["collectionId"] = QString::number(r.value("collectionId").toVariant().toLongLong());
    }
    o["childrenIds"] = QJsonArray{};
    o["children"] = QJsonObject{};
    o["copyright"] = r.value("copyright").toString();
    // the artwork: the public API's 100-point picture, its size made a template
    QString art = r.value("artworkUrl512").toString(r.value("artworkUrl100").toString(r.value("artworkUrl60").toString()));
    const int slash = art.lastIndexOf('/');
    if (slash > 0)
        art = art.left(slash + 1) + "{w}x{h}bb.{f}";
    const bool tall = o["kind"] == "movie" || o["kind"] == "book";
    o["artwork"] = QJsonObject{{"url", art}, {"width", tall ? 400 : 600}, {"height", 600}, {"hasAlpha", false}};
    // what it costs, for the buy buttons (no buying: they do nothing)
    const double price = collection ? r.value("collectionPrice").toDouble() : r.value("trackPrice").toDouble(r.value("price").toDouble());
    const QString currency = r.value("currency").toString();
    // the price as the store writes it in its country ($9.99, 9,99 €, £7.99)
    QString formatted = "Free";
    if (price > 0) {
        const QString n = QString::number(price, 'f', 2);
        formatted = currency == "USD" ? "$" + n : currency == "GBP" ? "£" + n : currency == "EUR" ? QString(n).replace('.', ',') + " €"
                                                                                                   : n + " " + currency;
    }
    // the button shows the price itself; its action words only for what is free
    QJsonObject action{{"short", price > 0 ? QString() : QString("Get")}, {"medium", price > 0 ? QString() : QString("Get")},
                       {"long", price > 0 ? QString() : QString("Get")}, {"downloaded", "Purchased"}, {"downloading", "Purchasing"}};
    // a song's preview (its 30 seconds) as an asset of its offer
    QJsonArray assets;
    if (!r.value("previewUrl").toString().isEmpty())
        assets.append(QJsonObject{{"flavor", "plus"}, {"preview", QJsonObject{{"url", r.value("previewUrl").toString()}, {"duration", 30}}},
                                  {"duration", r.value("trackTimeMillis").toInt() / 1000}});
    o["offers"] = QJsonArray{QJsonObject{{"type", "buy"}, {"price", price}, {"priceFormatted", formatted}, {"actionText", action},
                                         {"assets", assets}, {"buyParams", ""}}};
    return o;
}

class Catalog : public QWebEngineUrlSchemeHandler {
public:
    QString country = "us";
    QNetworkAccessManager *net = nullptr;
    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QUrlQuery q(job->requestUrl());
        const QStringList ids = q.queryItemValue("id", QUrl::FullyDecoded).split(',', Qt::SkipEmptyParts);
        QUrl u("https://itunes.apple.com/lookup");
        // a product page (profile "product-…"): the album with its songs
        const bool product = q.queryItemValue("p").startsWith("product");
        QUrlQuery lookup{{"id", ids.join(',')}, {"country", country}};
        if (product)
            lookup.addQueryItem("entity", "song");
        u.setQuery(lookup);
        QNetworkReply *r = net->get(QNetworkRequest(u));
        QPointer<QWebEngineUrlRequestJob> alive(job);
        connect(r, &QNetworkReply::finished, this, [r, alive] {
            r->deleteLater();
            if (!alive)
                return;
            QJsonObject results;
            QList<QJsonObject> songs;
            for (const QJsonValue &v : QJsonDocument::fromJson(r->readAll()).object().value("results").toArray()) {
                const QJsonObject raw = v.toObject();
                const QJsonObject item = platformItem(raw);
                if (raw.value("wrapperType").toString() == "track" && results.contains(item.value("collectionId").toString()))
                    songs.append(item);
                else
                    results[item.value("id").toString()] = item;
            }
            // the songs as their album's children, in their order
            for (const QJsonObject &song : songs) {
                const QString album = song.value("collectionId").toString();
                QJsonObject a = results[album].toObject();
                QJsonArray order = a["childrenIds"].toArray();
                QJsonObject children = a["children"].toObject();
                order.append(song.value("id").toString());
                children[song.value("id").toString()] = song;
                a["childrenIds"] = order;
                a["children"] = children;
                results[album] = a;
            }
            auto *buf = new QBuffer(alive);
            buf->setData(QJsonDocument(QJsonObject{{"results", results}, {"version", 2}}).toJson(QJsonDocument::Compact));
            alive->setAdditionalResponseHeaders({{"Access-Control-Allow-Origin", "*"}});
            alive->reply("application/json", buf);
        });
    }
};

class Interceptor : public QWebEngineUrlRequestInterceptor {
public:
    QByteArray storefront;
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QString host = info.requestUrl().host();
        if (host == "client-api.itunes.apple.com" && info.requestUrl().path().endsWith("/lookup")) {
            QUrl to;
            to.setScheme(CatalogScheme);
            to.setPath("lookup");
            to.setQuery(info.requestUrl().query());
            info.redirect(to);
            return;
        }
        if (qEnvironmentVariableIsSet("ITUNES_STORE_DEBUG"))
            std::fprintf(stderr, "req: %s\n", qPrintable(info.requestUrl().toString()));
        if (!host.endsWith("apple.com") && !host.endsWith("mzstatic.com"))
            return;
        if (!storefront.isEmpty())
            info.setHttpHeader("X-Apple-Store-Front", storefront);
        info.setHttpHeader("X-Apple-Tz", QByteArray::number(QDateTime::currentDateTime().offsetFromUtc()));
    }
};

class StorePage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
    std::function<void(const QJsonObject &)> onMessage;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &message, int line, const QString &source) override
    {
        if (qEnvironmentVariableIsSet("ITUNES_STORE_DEBUG"))
            std::fprintf(stderr, "js: %s (%s:%d)\n", qPrintable(message.left(400)), qPrintable(source.section('/', -1)), line);
        if (!message.startsWith("iTunesBridge:"))
            return;
        const QJsonObject m = QJsonDocument::fromJson(message.mid(13).toUtf8()).object();
        if (onMessage)
            onMessage(m);
    }
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool) override
    {
        // itms: and itmss: are the store's own schemes: iTunes takes them as https
        if (url.scheme() == "itms" || url.scheme() == "itmss") {
            QUrl u = url;
            u.setScheme("https");
            QTimer::singleShot(0, this, [this, u] { load(u); });
            return false;
        }
        return url.scheme() == "https" || url.scheme() == "http" || url.scheme() == "about" || url.scheme() == "data";
    }
};

class Store : public QObject {
public:
    explicit Store(const QString &frameFile) : m_frameFile(frameFile)
    {
        m_profile = new QWebEngineProfile("iTunesStore", this);
        m_profile->setHttpUserAgent(userAgent(true));
        m_profile->setUrlRequestInterceptor(&m_interceptor);
        m_catalog.net = &m_net;
        m_profile->installUrlSchemeHandler(CatalogScheme, &m_catalog);
        m_page = new StorePage(m_profile, this);
        m_page->settings()->setAttribute(QWebEngineSettings::ShowScrollBars, true);
        m_page->onMessage = [this](const QJsonObject &m) { message(m); };
        m_view = new QWebEngineView;
        m_view->setPage(m_page);
        m_view->setAttribute(Qt::WA_DontShowOnScreen);
        m_view->resize(800, 600);
        m_view->show();
        m_page->setVisible(true);   // visible to the page (document.visibilityState): its lazy pictures load
        connect(m_page, &QWebEnginePage::urlChanged, this, [this](const QUrl &u) {
            // the store's country (its pages' cc=) for the catalog's answers
            const QString cc = QUrlQuery(u).queryItemValue("cc");
            if (!cc.isEmpty())
                m_catalog.country = cc;
            say("url " + u.toString());
            say(QString("nav %1 %2").arg(m_page->history()->canGoBack()).arg(m_page->history()->canGoForward()));
        });
        connect(m_page, &QWebEnginePage::loadStarted, this, [] { say("loading 1"); });
        connect(m_page, &QWebEnginePage::loadFinished, this, [this](bool) {
            say("loading 0");
            say(QString("nav %1 %2").arg(m_page->history()->canGoBack()).arg(m_page->history()->canGoForward()));
        });
        // the pictures: when the page changes (repaints are not signalled off screen, so a short
        // timer), one at a time until iTunes has taken the last
        m_timer.setInterval(66);
        connect(&m_timer, &QTimer::timeout, this, [this] { frame(); });
        m_timer.start();
        auto *in = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
        connect(in, &QSocketNotifier::activated, this, [this] { readInput(); });
        fetchBag();
    }

private:
    // the URL bag: its "storeFront" the first page, the store front sent back with every request
    void fetchBag()
    {
        QNetworkRequest req{QUrl(BagURL)};
        req.setHeader(QNetworkRequest::UserAgentHeader, userAgent(false));
        QNetworkReply *r = m_net.get(req);
        connect(r, &QNetworkReply::finished, this, [this, r] {
            r->deleteLater();
            const QByteArray front = r->rawHeader("x-set-apple-store-front");
            if (!front.isEmpty())
                m_interceptor.storefront = front;
            // {bag: <data>, certs, signature}: the bag itself a plist, base64 in the data
            const QByteArray raw = r->readAll();
            const int a = raw.indexOf("<key>bag</key>");
            const int s = a >= 0 ? raw.indexOf("<data>", a) : -1, e = s >= 0 ? raw.indexOf("</data>", s) : -1;
            m_bag = s >= 0 && e > s ? parsePlist(QByteArray::fromBase64(raw.mid(s + 6, e - s - 6).simplified().replace(" ", ""))).toObject()
                                    : parsePlist(raw).toObject();
            start();
        });
    }

    void start()
    {
        QWebEngineScript script;
        script.setName("iTunes");
        script.setSourceCode(bridgeScript(m_bag, QString::fromLatin1(m_interceptor.storefront)));
        script.setInjectionPoint(QWebEngineScript::DocumentCreation);
        script.setWorldId(QWebEngineScript::MainWorld);
        script.setRunsOnSubFrames(true);
        m_profile->scripts()->insert(script);
        m_home = m_bag.value("storeFront").toString();
        if (m_home.isEmpty())
            m_home = "https://itunes.apple.com/us/store";
        m_page->load(m_pending.isEmpty() ? QUrl(m_home) : QUrl(m_pending));
        m_started = true;
    }

    void message(const QJsonObject &m)
    {
        if (m.contains("protocol")) {
            QString x = m.value("protocol").toString();
            x.replace("\\", "\\\\").replace("\n", "\\n");
            say("protocol " + x);
        } else if (m.contains("openURL")) {
            load(m.value("openURL").toString());
        } else if (m.contains("back")) {
            m_page->triggerAction(QWebEnginePage::Back);
        } else if (m.contains("forward")) {
            m_page->triggerAction(QWebEnginePage::Forward);
        } else if (m.contains("library")) {
            say("library");
        } else if (m.contains("browser")) {
            say("browser " + m.value("browser").toString());
        } else if (m.contains("spinner")) {
            say(QString("loading %1").arg(m.value("spinner").toInt()));
        }
    }

    void load(const QString &url)
    {
        if (!m_started) {
            m_pending = url;
            return;
        }
        QUrl u(url);
        if (u.scheme() == "itms" || u.scheme() == "itmss")
            u.setScheme("https");
        m_page->load(u);
    }

    void frame()
    {
        if (!m_taken || m_view->size().isEmpty())
            return;
        const QSize sz = m_view->size();
        QImage img(sz, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        m_view->render(&p);
        p.end();
        if (img == m_last)
            return;
        m_last = img;
        const size_t bytes = size_t(sz.width()) * sz.height() * 4;
        const int fd = ::open(m_frameFile.toLocal8Bit().constData(), O_RDWR);
        if (fd < 0)
            return;
        if (ftruncate(fd, off_t(bytes)) == 0) {
            void *map = mmap(nullptr, bytes, PROT_WRITE, MAP_SHARED, fd, 0);
            if (map != MAP_FAILED) {
                for (int y = 0; y < sz.height(); ++y)
                    std::memcpy(static_cast<char *>(map) + size_t(y) * sz.width() * 4, img.constScanLine(y), size_t(sz.width()) * 4);
                munmap(map, bytes);
                m_taken = false;
                say(QString("frame %1 %2").arg(sz.width()).arg(sz.height()));
            }
        }
        ::close(fd);
    }

    QWidget *target() const { return m_view->focusProxy() ? m_view->focusProxy() : m_view; }

    static Qt::KeyboardModifiers modifiers(int m)
    {
        Qt::KeyboardModifiers k;
        if (m & 1)
            k |= Qt::ShiftModifier;
        if (m & 2)
            k |= Qt::ControlModifier;
        if (m & 4)
            k |= Qt::AltModifier;
        if (m & 8)
            k |= Qt::MetaModifier;
        return k;
    }

    void readInput()
    {
        char buf[4096];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) {
            QApplication::quit();
            return;
        }
        m_input.append(buf, int(n));
        int nl;
        while ((nl = m_input.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_input.left(nl));
            m_input.remove(0, nl + 1);
            command(line);
        }
    }

    void command(const QString &line)
    {
        const QStringList w = line.split(' ');
        const QString c = w.value(0);
        if (c == "ack") {
            m_taken = true;
        } else if (c == "size" && w.size() >= 3) {
            m_view->resize(w[1].toInt(), w[2].toInt());
            m_last = QImage();
        } else if (c == "load" && w.size() >= 2) {
            load(w[1]);
        } else if (c == "home") {
            load(m_home);
        } else if (c == "back") {
            m_page->triggerAction(QWebEnginePage::Back);
        } else if (c == "forward") {
            m_page->triggerAction(QWebEnginePage::Forward);
        } else if (c == "reload") {
            m_page->triggerAction(QWebEnginePage::Reload);
        } else if (c == "mouse" && w.size() >= 7) {
            const QPointF pos(w[2].toDouble(), w[3].toDouble());
            const Qt::MouseButton b = w[4] == "2" ? Qt::RightButton : w[4] == "3" ? Qt::MiddleButton : Qt::LeftButton;
            const Qt::KeyboardModifiers mods = modifiers(w[6].toInt());
            QEvent::Type t = w[1] == "down" ? (w[5].toInt() >= 2 ? QEvent::MouseButtonDblClick : QEvent::MouseButtonPress)
                             : w[1] == "up" ? QEvent::MouseButtonRelease : QEvent::MouseMove;
            if (t == QEvent::MouseButtonDblClick) {   // Qt wants the press first
                QMouseEvent press(QEvent::MouseButtonPress, pos, pos, b, b, mods);
                QApplication::sendEvent(target(), &press);
            }
            const Qt::MouseButtons held = t == QEvent::MouseMove ? m_buttons : t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(b);
            if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonDblClick)
                m_buttons = b;
            else if (t == QEvent::MouseButtonRelease)
                m_buttons = Qt::NoButton;
            QMouseEvent e(t, pos, pos, t == QEvent::MouseMove ? Qt::NoButton : b, held, mods);
            QApplication::sendEvent(target(), &e);
            const Qt::CursorShape shape = target()->cursor().shape();
            if (shape != m_cursor) {
                m_cursor = shape;
                say(QString("cursor %1").arg(shape == Qt::PointingHandCursor ? "hand" : shape == Qt::IBeamCursor ? "text" : "arrow"));
            }
        } else if (c == "wheel" && w.size() >= 5) {
            const QPointF pos(w[1].toDouble(), w[2].toDouble());
            QWheelEvent e(pos, pos, QPoint(), QPoint(int(w[3].toDouble() * 120), int(w[4].toDouble() * 120)), m_buttons,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(target(), &e);
        } else if (c == "key" && w.size() >= 4) {
            const int keysym = w[2].toInt();
            const QString text = w.size() >= 5 ? QString::fromUtf8(QByteArray::fromHex(w[4].toLatin1())) : QString();
            QKeyEvent e(w[1] == "down" ? QEvent::KeyPress : QEvent::KeyRelease, qtKey(keysym, text), modifiers(w[3].toInt()), text);
            QApplication::sendEvent(target(), &e);
        } else if (c == "focus" && w.size() >= 2) {
            QFocusEvent e(w[1] == "1" ? QEvent::FocusIn : QEvent::FocusOut);
            QApplication::sendEvent(target(), &e);
        }
    }

    // X keysyms (xkbcommon's) to Qt's keys, those a page needs
    static int qtKey(int sym, const QString &text)
    {
        switch (sym) {
        case 0xff08: return Qt::Key_Backspace;
        case 0xff09: return Qt::Key_Tab;
        case 0xff0d: return Qt::Key_Return;
        case 0xff1b: return Qt::Key_Escape;
        case 0xff50: return Qt::Key_Home;
        case 0xff51: return Qt::Key_Left;
        case 0xff52: return Qt::Key_Up;
        case 0xff53: return Qt::Key_Right;
        case 0xff54: return Qt::Key_Down;
        case 0xff55: return Qt::Key_PageUp;
        case 0xff56: return Qt::Key_PageDown;
        case 0xff57: return Qt::Key_End;
        case 0xffff: return Qt::Key_Delete;
        case 0x20: return Qt::Key_Space;
        default: break;
        }
        if (!text.isEmpty())
            return QKeySequence(text.toUpper()).count() ? QKeySequence(text.toUpper())[0].key() : Qt::Key_unknown;
        return Qt::Key_unknown;
    }

    QString m_frameFile, m_home, m_pending;
    QWebEngineProfile *m_profile = nullptr;
    StorePage *m_page = nullptr;
    QWebEngineView *m_view = nullptr;
    Interceptor m_interceptor;
    Catalog m_catalog;
    QNetworkAccessManager m_net;
    QJsonObject m_bag;
    QByteArray m_input;
    QTimer m_timer;
    QImage m_last;
    bool m_taken = true, m_started = false;
    Qt::MouseButtons m_buttons = Qt::NoButton;
    Qt::CursorShape m_cursor = Qt::ArrowCursor;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "iTunesStore <frame file>\n");
        return 2;
    }
    // off screen, whatever the session: iTunes shows the pictures; Chromium's own GPU process left out
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    flags += " --disable-gpu --disable-web-security";
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
    QWebEngineUrlScheme catalog(CatalogScheme);
    catalog.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    catalog.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled | QWebEngineUrlScheme::FetchApiAllowed |
                     QWebEngineUrlScheme::ContentSecurityPolicyIgnored);
    QWebEngineUrlScheme::registerScheme(catalog);
    QApplication app(argc, argv);
    app.setApplicationName("iTunesStore");
    Store store(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
