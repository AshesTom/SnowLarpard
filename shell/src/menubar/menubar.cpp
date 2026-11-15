// SPDX-License-Identifier: LGPL-2.1-or-later
#include "menubar.h"

#include "appmenu.h"
#include "aqua.h"
#include "art.h"
#include "assets.h"
#include "extras.h"
#include "menubarfilter.h"
#include "session.h"
#include "x11.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>

static const QString dockBundle = QStringLiteral("/System/Library/CoreServices/Dock.app");

MenuBar::MenuBar(QWidget *parent) : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_X11NetWmWindowTypeDock);
    setFont(Assets::uiFont(14));
    QRect screen = QGuiApplication::primaryScreen()->geometry();
    setGeometry(screen.x(), screen.y(), screen.width(), Height);
    reloadDesktop();
    // Desktop & Screen Saver changes the picture and the translucency: the bar follows
    auto *prefsWatch = new QFileSystemWatcher(this);
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard");
    prefsWatch->addPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard");
    auto *coalesce = new QTimer(this);
    coalesce->setSingleShot(true);
    coalesce->setInterval(150);
    connect(prefsWatch, &QFileSystemWatcher::directoryChanged, coalesce, qOverload<>(&QTimer::start));
    connect(coalesce, &QTimer::timeout, this, &MenuBar::reloadDesktop);

    // Right to left, as SystemUIServer lays them: Spotlight at the edge, then the clock...
    m_extras = {new SpotlightExtra(this), new ClockExtra(this), new BatteryExtra(this), new VolumeExtra(this)};
    for (MenuExtra *e : m_extras)
        connect(e, &MenuExtra::changed, this, [this] { layoutItems(); update(); });

    m_watcher = new X11::Watcher(this);
    connect(m_watcher, &X11::Watcher::changed, this, &MenuBar::activeChanged);
    m_registrar = new AppMenuRegistrar(this);
    connect(m_registrar, &AppMenuRegistrar::changed, this, [this](uint window) {
        if (window == m_active) {
            m_menuKey.clear();
            m_app = App{};
            activeChanged();
        }
    });
    winId();
    X11::setDock(this, Height, 0);
    rebuild();
    activeChanged();
}

void MenuBar::reloadDesktop()
{
    QSettings prefs(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/menubar.conf",
                    QSettings::IniFormat);
    m_transparent = prefs.value("AppleEnableMenuBarTransparency", true).toBool();
    // What is behind the bar is always the desktop picture (windows stay under it): the bar is made
    // from it, as the window server does (menubarfilter.h), and drawn opaque.
    const QSize screen = QGuiApplication::primaryScreen()->geometry().size();
    m_wallpaper = Assets::wallpaperFor(screen);
    m_generated = QImage();
    m_generatedApple = false;
    if (m_transparent) {
        const QImage behind = m_wallpaper.copy(0, 0, screen.width(), Height);
        m_generated = generateMenuBar(behind);
        m_generatedApple = drawMenuApple(m_generated, behind);   // the window server's Apple, in the bar
    }
    update();
}

void MenuBar::activeChanged()
{
    xcb_window_t active = X11::activeWindow();
    // a non-activating panel (loginwindow's About This Mac: a utility window out of the Dock) leaves
    // the menu bar to the application that was in front
    bool panel = false;
    for (uint32_t s : X11::cardinals(active, X11::atom("_NET_WM_STATE"), XCB_ATOM_ATOM))
        panel |= s == X11::atom("_NET_WM_STATE_SKIP_TASKBAR");
    for (uint32_t t : X11::cardinals(active, X11::atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM))
        panel &= t == X11::atom("_NET_WM_WINDOW_TYPE_UTILITY");
    if (active && panel && !m_app.id.isEmpty())
        return;
    // the desktop is the Finder's
    bool desktop = false;
    for (uint32_t t : X11::cardinals(active, X11::atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM))
        desktop |= t == X11::atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    App app = active && !desktop ? Apps::forWindow(X11::wmClass(active), X11::wmInstance(active), X11::pid(active)) : Apps::finder();
    m_active = active;
    // the window's own menus, when it publishes them
    auto menu = active && !desktop ? m_registrar->menuFor(active) : QPair<QString, QString>();
    QString key = menu.first.isEmpty() ? QString() : menu.first + menu.second;
    if (app.id == m_app.id && key == m_menuKey)
        return;
    m_app = app;
    if (key != m_menuKey) {
        m_menuKey = key;
        if (m_imported)
            m_imported->deleteLater();
        m_imported = nullptr;
        if (!key.isEmpty()) {
            m_imported = new DBusMenu(menu.first, menu.second, this);
            connect(m_imported, &DBusMenu::layoutChanged, this, [this] {
                if (m_open < 0)
                    rebuild();
                else
                    m_rebuildPending = true;
            });
        }
    }
    if (m_open < 0)
        rebuild();
    else
        m_rebuildPending = true;
}

void MenuBar::rebuild()
{
    for (Item &i : m_items)
        if (i.menu && !i.extra)   // the extras keep their menus
            i.menu->deleteLater();
    m_items.clear();
    Item apple;
    apple.apple = true;
    apple.menu = appleMenu();
    m_items.append(apple);
    bool finder = m_app.id == "finder" || !m_app.valid();
    QString name = finder ? QStringLiteral("Finder") : m_app.name;
    QVector<QMenu *> menus = appMenus(name, finder);
    if (m_imported && m_imported->ready() && !m_imported->topLevel().isEmpty()) {
        // the application's own menus after its name, instead of the common ones
        for (int i = 1; i < menus.size(); ++i)
            menus[i]->deleteLater();
        menus.resize(1);
        for (const DBusMenuItem &top : m_imported->topLevel())
            menus.append(m_imported->createMenu(top.id, dbusMenuLabel(top.properties.value("label").toString())));
    }
    for (int i = 0; i < menus.size(); ++i) {
        Item it;
        it.title = i == 0 ? name : menus[i]->title();
        it.bold = i == 0;
        it.menu = menus[i];
        m_items.append(it);
    }
    for (MenuExtra *e : m_extras) {
        Item it;
        it.extra = e;
        it.menu = e->menu();
        m_items.append(it);
    }
    for (Item &i : m_items)
        if (i.menu) {
            i.menu->setProperty("aquaMenu", "straighttop");   // the menu bar's: square at the top
            Aqua::prepareMenu(i.menu);
            i.menu->installEventFilter(this);
        }
    layoutItems();
    update();
}

void MenuBar::layoutItems()
{
    QFontMetrics fm(font()), bold(Assets::uiFont(14, true));
    int x = 10;
    int right = width() - 6;
    for (Item &i : m_items) {
        if (i.extra) {
            if (!i.extra->visible()) {
                i.rect = {};
                continue;
            }
            int w = i.extra->width(fm);
            right -= w;
            i.rect = QRect(right, 0, w, Height);
        } else {
            int w = i.apple ? 37 : (i.bold ? bold : fm).horizontalAdvance(i.title) + 20;
            i.rect = QRect(x, 0, w, Height);
            x += w;
        }
    }
}

int MenuBar::itemAt(const QPoint &p) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].rect.contains(p))
            return i;
    return -1;
}

void MenuBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QImage bar = Art::shared(Art::MenuBar);
    if (m_transparent && !m_generated.isNull()) {
        // 10.6 with a graphics card: the window server's filter, from the desktop (menubarfilter.h)
        p.drawImage(QPoint(0, 0), m_generated);
    } else if (!bar.isNull()) {
        // Without one, or with HIToolbox's preference AppleEnableMenuBarTransparency off: CoreUI's bar
        // (CUIRenderer::DrawMenuBar: shared art 20102, stretched); measured on 10.6, nothing of the desktop
        // shows, as if the art lay over a backing white to mid-height, then grey to 150; the edge is 75.
        QLinearGradient backing(0, 0, 0, bar.height());
        backing.setColorAt(0, QColor(255, 255, 255));
        backing.setColorAt(0.5, QColor(255, 255, 255));
        backing.setColorAt(0.6, QColor(233, 233, 233));
        backing.setColorAt(1, QColor(150, 150, 150));
        p.fillRect(0, 0, width(), bar.height(), backing);
        p.drawImage(QRect(0, 0, width(), bar.height()), bar);
        p.fillRect(0, bar.height(), width(), Height - bar.height(), QColor(75, 75, 75));
    } else {
        QLinearGradient g(0, 0, 0, Height);
        g.setColorAt(0, QColor(250, 250, 250));
        g.setColorAt(1, QColor(212, 212, 212));
        p.fillRect(0, 0, width(), Height - 1, g);
        p.fillRect(0, Height - 1, width(), 1, QColor(70, 70, 70));
    }

    for (int idx = 0; idx < m_items.size(); ++idx) {
        const Item &i = m_items[idx];
        if (i.rect.isEmpty())
            continue;
        bool open = idx == m_open;
        if (open && !(i.extra && dynamic_cast<SpotlightExtra *>(i.extra))) {
            // CUIRenderer::DrawMenuTitle's selected title (shared art 20103), stretched over it
            static const QImage tab = Art::shared(Art::MenuTitleSelected);
            QRect r = i.rect.adjusted(0, 0, 0, -1);
            if (!tab.isNull()) {
                p.drawImage(r, tab);
            } else {
                QLinearGradient sel(0, 0, 0, Height);
                sel.setColorAt(0, QColor(0x6a, 0x8e, 0xf8));
                sel.setColorAt(1, QColor(0x21, 0x55, 0xef));
                p.fillRect(r, sel);
            }
        }
        if (i.extra) {
            i.extra->paint(p, i.rect.adjusted(0, 0, 0, -1), open);
            continue;
        }
        if (i.apple) {
            // over the translucent bar the window server draws the Apple (CIMenuAppleGenerator), unless open
            if (!open && m_transparent && m_generatedApple)
                continue;
            // CUIRenderer::DrawMenuTitle, kind 0xCF: art 20061 (20062 open), centred, one pixel right
            QImage apple = Art::shared(open ? Art::AppleMenuTitleSelected : Art::AppleMenuTitle);
            if (!apple.isNull()) {
                p.drawImage(QPoint(i.rect.center().x() + 1 - apple.width() / 2, (Height - 1 - apple.height()) / 2), apple);
                continue;
            }
            QFont f = Assets::uiFont(17);
            p.setFont(f);
            p.setPen(open ? Qt::white : Qt::black);
            p.drawText(i.rect.adjusted(0, -1, 0, -1), Qt::AlignCenter, QString(QChar(0xF8FF)));
            continue;
        }
        p.setFont(i.bold ? Assets::uiFont(14, true) : font());
        // the engraved white under the text, as on the Mac
        if (!open) {
            p.setPen(QColor(255, 255, 255, 140));
            p.drawText(i.rect.translated(0, 1).adjusted(0, 0, 0, -1), Qt::AlignCenter, i.title);
        }
        p.setPen(open ? Qt::white : Qt::black);
        p.drawText(i.rect.adjusted(0, 0, 0, -1), Qt::AlignCenter, i.title);
    }
}

void MenuBar::mousePressEvent(QMouseEvent *e)
{
    int idx = itemAt(e->position().toPoint());
    if (idx >= 0)
        open(idx);
}

void MenuBar::open(int index)
{
    if (m_popup)
        m_popup->hide();
    m_open = index;
    update();
    Item &i = m_items[index];
    QPoint at = mapToGlobal(QPoint(i.rect.left(), Height));
    QWidget *popup = i.menu ? static_cast<QWidget *>(i.menu) : i.extra ? i.extra->popup() : nullptr;
    if (!popup) {
        m_open = -1;
        update();
        return;
    }
    m_popup = popup;
    popup->installEventFilter(this);
    if (auto *menu = qobject_cast<QMenu *>(popup)) {
        // Right-hand menus end at the screen's edge
        int w = menu->sizeHint().width();
        at.setX(qMin(at.x(), mapToGlobal(QPoint(width(), 0)).x() - w));
        menu->popup(at);
    } else {
        popup->adjustSize();
        at.setX(qMin(at.x(), mapToGlobal(QPoint(width(), 0)).x() - popup->width()));
        popup->move(at);
        popup->show();
        popup->activateWindow();
    }
}

bool MenuBar::eventFilter(QObject *o, QEvent *e)
{
    if (o != m_popup)
        return QWidget::eventFilter(o, e);
    if (e->type() == QEvent::Hide) {
        m_open = -1;
        update();
        if (m_rebuildPending || !m_app.valid() || m_items.size() < 2 || m_items[1].title != (m_app.id == "finder" ? "Finder" : m_app.name)) {
            m_rebuildPending = false;
            QMetaObject::invokeMethod(this, &MenuBar::rebuild, Qt::QueuedConnection);
        }
    } else if (e->type() == QEvent::MouseMove) {
        // Sliding along the bar with a menu open opens the menu under the pointer, as on the Mac
        QPoint local = mapFromGlobal(static_cast<QMouseEvent *>(e)->globalPosition().toPoint());
        int idx = rect().contains(local) ? itemAt(local) : -1;
        if (idx >= 0 && idx != m_open && (m_items[idx].menu || !m_items[idx].extra)) {
            QMetaObject::invokeMethod(this, [this, idx] { open(idx); }, Qt::QueuedConnection);
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

// ---- the Apple menu

static void setDockDefault(const char *key, const QVariant &value)
{
    QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/dock.conf",
                QSettings::IniFormat);
    s.setValue(key, value);
}

static QVariant dockDefault(const char *key, const QVariant &fallback)
{
    QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/dock.conf",
                QSettings::IniFormat);
    return s.value(key, fallback);
}

QMenu *MenuBar::appleMenu()
{
    auto *m = new QMenu;
    m->addAction("About This Mac", [] { Session::aboutThisMac(); });
    QAction *update = m->addAction("Software Update…", [] {
        for (const char *id : {"org.kde.discover", "org.gnome.Software", "pamac-manager", "octopi"})
            if (Apps::launch(Apps::byId(id)))
                return;
    });
    update->setEnabled(Apps::byId("org.kde.discover").valid() || Apps::byId("org.gnome.Software").valid() ||
                       Apps::byId("pamac-manager").valid() || Apps::byId("octopi").valid());
    m->addSeparator();
    App prefs = Apps::forMacBundle("/Applications/System Preferences.app");
    QAction *sp = m->addAction("System Preferences…", [prefs] { Apps::launch(prefs); });
    sp->setEnabled(prefs.valid());

    // Dock ▸, with the Dock's own words
    auto t = [](const char *key, const char *fallback) { return Assets::tr(dockBundle, "DockMenus", key, fallback); };
    QMenu *dock = m->addMenu("Dock");
    bool hiding = dockDefault("autohide", false).toBool(), mag = dockDefault("magnification", false).toBool();
    dock->addAction(hiding ? t("TURN_HIDING_OFF", "Turn Hiding Off") : t("TURN_HIDING_ON", "Turn Hiding On"),
                    [hiding] { setDockDefault("autohide", !hiding); });
    dock->addAction(mag ? t("TURN_MAG_OFF", "Turn Magnification Off") : t("TURN_MAG_ON", "Turn Magnification On"),
                    [mag] { setDockDefault("magnification", !mag); });
    dock->addSeparator();
    for (auto [key, fallback, value] : {std::tuple{"LEFT", "Position on Left", "left"}, std::tuple{"BOTTOM", "Position at Bottom", "bottom"},
                                        std::tuple{"RIGHT", "Position on Right", "right"}}) {
        QAction *a = dock->addAction(t(key, fallback));
        a->setCheckable(true);
        a->setChecked(dockDefault("orientation", "bottom").toString() == value);
        a->setEnabled(QString(value) == "bottom");   // the sides are for later
    }
    dock->addSeparator();
    dock->addAction(t("DOCK_PREFS", "Dock Preferences…"))->setEnabled(false);

    m->addSeparator();
    recentItems(m);
    m->addSeparator();
    m->addAction("Force Quit…\t⌥⌘⎋", [] { Session::forceQuit(); });
    m->addSeparator();
    m->addAction("Sleep", [] { Session::power(Session::Sleep); });
    m->addAction("Restart…", [] { Session::power(Session::Restart); });
    m->addAction("Shut Down…", [] { Session::power(Session::ShutDown); });
    m->addSeparator();
    m->addAction(QString("Log Out %1…\t⇧⌘Q").arg(Session::userFullName()), [] { Session::power(Session::LogOut); });
    return m;
}

QMenu *MenuBar::recentItems(QMenu *parent)
{
    // The freedesktop list of recent files (~/.local/share/recently-used.xbel), newest first
    QMenu *menu = parent->addMenu("Recent Items");
    connect(menu, &QMenu::aboutToShow, menu, [menu] {
        menu->clear();
        QFile f(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel");
        QList<QPair<QString, QString>> items;   // modified, href
        if (f.open(QIODevice::ReadOnly)) {
            QXmlStreamReader xml(&f);
            while (!xml.atEnd())
                if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == u"bookmark")
                    items.append({xml.attributes().value("modified").toString(), xml.attributes().value("href").toString()});
        }
        std::sort(items.begin(), items.end(), [](auto &a, auto &b) { return a.first > b.first; });
        menu->addAction("Documents")->setEnabled(false);
        int n = 0;
        for (auto &[when, href] : items) {
            QUrl url(href);
            if (url.isLocalFile() && !QFile::exists(url.toLocalFile()))
                continue;
            menu->addAction(url.fileName(), [url] { QProcess::startDetached("xdg-open", {url.toString()}); });
            if (++n == 10)
                break;
        }
        menu->addSeparator();
        menu->addAction("Clear Menu", [] {
            QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel");
        });
        Aqua::prepareMenu(menu);
    });
    return menu;
}

// ---- the application's menus: the Finder's, or the ones every application has

QVector<QMenu *> MenuBar::appMenus(const QString &name, bool finder)
{
    QVector<QMenu *> menus;
    xcb_window_t active = m_active;
    App app = m_app;
    auto *appMenu = new QMenu;
    appMenu->addAction(QString("About %1").arg(name), [finder] {
        if (finder)
            Session::aboutThisMac();
    })->setEnabled(finder);
    appMenu->addSeparator();
    appMenu->addAction("Preferences…\t⌘,")->setEnabled(false);
    if (finder) {
        appMenu->addSeparator();
        appMenu->addAction("Empty Trash…\t⇧⌘⌫", [] { Session::emptyTrash(); });
    }
    appMenu->addSeparator();
    appMenu->addMenu("Services")->setEnabled(false);
    appMenu->addSeparator();
    appMenu->addAction(QString("Hide %1\t⌘H").arg(name), [app] { Session::hideApp(app); });
    appMenu->addAction("Hide Others\t⌥⌘H", [app] { Session::hideOthers(app); });
    appMenu->addAction("Show All", [] { Session::showAll(); });
    if (!finder) {
        appMenu->addSeparator();
        appMenu->addAction(QString("Quit %1\t⌘Q").arg(name), [app] { Session::quitApp(app); });
    }
    menus.append(appMenu);

    auto *file = new QMenu("File");
    if (finder) {
        App f = Apps::finder();
        file->addAction("New Finder Window\t⌘N", [f] { Apps::launch(f, {QDir::homePath()}); });
        file->addAction("New Folder\t⇧⌘N")->setEnabled(false);
        file->addAction("New Smart Folder\t⌥⌘N")->setEnabled(false);
        file->addSeparator();
        file->addAction("Open\t⌘O")->setEnabled(false);
        file->addSeparator();
        file->addAction("Get Info\t⌘I")->setEnabled(false);
        file->addSeparator();
        file->addAction("Find\t⌘F", [this] {
            for (int i = 0; i < m_items.size(); ++i)
                if (dynamic_cast<SpotlightExtra *>(m_items[i].extra))
                    QMetaObject::invokeMethod(this, [this, i] { open(i); }, Qt::QueuedConnection);
        });
    } else {
        file->addAction("Close Window\t⌘W", [active] { X11::close(active); })->setEnabled(active);
    }
    menus.append(file);

    auto *edit = new QMenu("Edit");
    for (const char *e : {"Undo\t⌘Z", "Redo\t⇧⌘Z", "-", "Cut\t⌘X", "Copy\t⌘C", "Paste\t⌘V", "Select All\t⌘A"}) {
        if (QByteArray(e) == "-")
            edit->addSeparator();
        else
            edit->addAction(QString::fromUtf8(e))->setEnabled(false);   // the applications' own menus come with the global menu
    }
    menus.append(edit);

    auto *view = new QMenu("View");
    if (finder)
        for (const char *v : {"as Icons\t⌘1", "as List\t⌘2", "as Columns\t⌘3", "as Cover Flow\t⌘4"})
            view->addAction(QString::fromUtf8(v))->setEnabled(false);
    else
        view->addAction("Enter Full Screen")->setEnabled(false);
    menus.append(view);

    if (finder) {
        auto *go = new QMenu("Go");
        App f = Apps::finder();
        auto place = [&](const QString &title, const QString &dir) {
            go->addAction(title, [f, dir] { Apps::launch(f, {dir}); })->setEnabled(QDir(dir).exists());
        };
        place("Computer\t⇧⌘C", "/");
        place("Home\t⇧⌘H", QDir::homePath());
        place("Desktop\t⇧⌘D", QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        place("Downloads\t⌥⌘L", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        place("Documents", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
        place("Applications\t⇧⌘A", "/usr/share/applications");
        menus.append(go);
    }
    menus.append(windowMenu());

    auto *help = new QMenu("Help");
    help->addAction(QString("%1 Help").arg(finder ? "Mac" : name))->setEnabled(false);
    menus.append(help);
    return menus;
}

QMenu *MenuBar::windowMenu()
{
    auto *m = new QMenu("Window");
    App app = m_app;
    connect(m, &QMenu::aboutToShow, m, [m, app] {
        m->clear();
        xcb_window_t active = X11::activeWindow();
        m->addAction("Minimize\t⌘M", [active] { X11::minimize(active); })->setEnabled(active);
        m->addAction("Zoom")->setEnabled(false);
        m->addSeparator();
        m->addAction("Bring All to Front", [app] { Session::bringToFront(app); });
        bool first = true;
        for (xcb_window_t w : X11::clientWindows()) {
            if (Apps::forWindow(X11::wmClass(w), X11::wmInstance(w), X11::pid(w)).id != app.id)
                continue;
            if (first)
                m->addSeparator(), first = false;
            QAction *a = m->addAction(X11::title(w), [w] { X11::activate(w); });
            a->setCheckable(true);
            a->setChecked(w == active);
        }
        Aqua::prepareMenu(m);
    });
    return m;
}
