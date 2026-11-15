// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktop.h"

#include "aqua.h"
#include "assets.h"
#include "x11.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <xcb/screensaver.h>

static const QString finderBundle = QStringLiteral("/System/Library/CoreServices/Finder.app");

DesktopWindow::DesktopWindow(QScreen *screen) : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnBottomHint)
{
    setAttribute(Qt::WA_X11NetWmWindowTypeDesktop);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setScreen(screen);
    setGeometry(screen->geometry());
    winId();
    X11::setDesktop(this);
    // ~/.config/snowleopard/desktop.conf: Desktop & Screen Saver writes it, the desktop follows at once
    auto *watch = new QFileSystemWatcher(this);
    watch->addPath(QFileInfo(Desktop::configFile()).path());
    connect(watch, &QFileSystemWatcher::directoryChanged, this, &DesktopWindow::reload);
    connect(screen, &QScreen::geometryChanged, this, [this](const QRect &g) {
        setGeometry(g);
        m_loaded.clear();
        reload();
    });
    m_change.setSingleShot(false);
    connect(&m_change, &QTimer::timeout, this, &DesktopWindow::nextPicture);
    reload();
    // The screen saver: after the idle time Desktop & Screen Saver sets (screensaver.conf), its slideshow
    auto *idle = new QTimer(this);
    connect(idle, &QTimer::timeout, this, [] {
        QSettings conf(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/screensaver.conf",
                       QSettings::IniFormat);
        const int seconds = conf.value("idle", 20 * 60).toInt();
        static QProcess *saver = nullptr;
        if (seconds <= 0 || !X11::conn() || (saver && saver->state() != QProcess::NotRunning))
            return;
        auto *info = xcb_screensaver_query_info_reply(X11::conn(), xcb_screensaver_query_info(X11::conn(), X11::root()), nullptr);
        if (!info)
            return;
        const bool start = info->ms_since_user_input >= uint32_t(seconds) * 1000;
        free(info);
        if (!start)
            return;
        if (!saver)
            saver = new QProcess(qApp);
        QString prefs = QCoreApplication::applicationDirPath() + "/../../../System Preferences.app/Contents/MacOS/System Preferences";
        saver->start(QFileInfo(prefs).absoluteFilePath(), {"--screensaver"});
    });
    idle->start(5000);
    // "When logging in": a new picture now
    if (Desktop::changeIntervals().value(Desktop::load().change).seconds == -1)
        nextPicture();
}

void DesktopWindow::reload()
{
    Desktop::Settings s = Desktop::load();
    QString key = QStringList{Desktop::picturePath(s), QString::number(s.placement), s.color.name()}.join('|');
    if (key != m_loaded) {
        m_loaded = key;
        m_picture = Desktop::render(s, size() * devicePixelRatioF());
        m_picture.setDevicePixelRatio(devicePixelRatioF());
        update();
    }
    // the picture changing by itself, every so often
    const int seconds = Desktop::changeIntervals().value(s.change).seconds;
    if (seconds > 0) {
        if (!m_change.isActive() || m_change.interval() != seconds * 1000)
            m_change.start(seconds * 1000);
    } else {
        m_change.stop();
    }
}

void DesktopWindow::nextPicture()
{
    Desktop::Settings s = Desktop::load();
    QString folder = s.changeFolder.isEmpty() ? QFileInfo(Desktop::picturePath(s)).path() : s.changeFolder;
    QStringList all = Desktop::pictures(folder);
    if (all.isEmpty())
        return;
    int i = all.indexOf(Desktop::picturePath(s));
    if (s.random && all.size() > 1) {
        int j;
        do
            j = int(QRandomGenerator::global()->bounded(all.size()));
        while (j == i);
        i = j;
    } else {
        i = (i + 1) % all.size();
    }
    s.picture = all[i];
    Desktop::save(s);   // the Dock and the menu bar follow the file too
}

void DesktopWindow::paintEvent(QPaintEvent *e)
{
    QPainter p(this);
    p.drawImage(e->rect(), m_picture, QRectF(QPointF(e->rect().topLeft()) * devicePixelRatioF(),
                                              QSizeF(e->rect().size()) * devicePixelRatioF()));
}

void DesktopWindow::mousePressEvent(QMouseEvent *e)
{
    // Clicking the desktop makes the Finder the active application, as on the Mac
    X11::activate(xcb_window_t(winId()));
    if (e->button() == Qt::RightButton || (e->button() == Qt::LeftButton && e->modifiers() & Qt::ControlModifier))
        showMenu(e->globalPosition().toPoint());
}

void DesktopWindow::showMenu(const QPoint &global)
{
    // The desktop's contextual menu, with the Finder's own words (its Localizable.strings on the DVD).
    // The Finder's code is protected: the order is 10.6's as seen on screen.
    auto t = [](const char *key, const char *fallback) { return Assets::tr(finderBundle, "Localizable", key, fallback); };
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(t("N156", "New Folder"), [] {
        // "untitled folder", then "untitled folder 2"..., as the Finder names them
        QDir desktop(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        desktop.mkpath(".");
        QString name = "untitled folder";
        for (int n = 2; desktop.exists(name); ++n)
            name = QStringLiteral("untitled folder %1").arg(n);
        desktop.mkdir(name);
    });
    menu->addSeparator();
    menu->addAction(t("N165", "Get Info"), [] {
        // the Desktop folder's Info window: its icon, kind, place, contents and dates (the Finder's full
        // Info window is for later)
        QFileInfo d(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        auto *info = new QWidget(nullptr, Qt::Window);
        info->setAttribute(Qt::WA_DeleteOnClose);
        info->setWindowTitle("Desktop Info");
        auto *lay = new QFormLayout(info);
        auto *icon = new QLabel;
        icon->setPixmap(QPixmap::fromImage(Assets::image("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/DesktopFolderIcon.icns", QSize(32, 32))));
        lay->addRow(icon, new QLabel("<b>Desktop</b>"));
        lay->addRow("Kind:", new QLabel("Folder"));
        lay->addRow("Where:", new QLabel(d.absolutePath().section('/', 0, -2)));
        lay->addRow("Contains:", new QLabel(QStringLiteral("%1 items").arg(QDir(d.absoluteFilePath()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size())));
        lay->addRow("Created:", new QLabel(QLocale().toString(d.birthTime(), QLocale::LongFormat)));
        lay->addRow("Modified:", new QLabel(QLocale().toString(d.lastModified(), QLocale::LongFormat)));
        info->show();
    });
    menu->addSeparator();
    menu->addAction(t("N164", "Change Desktop Background…"), [] {
        // System Preferences, on Desktop & Screen Saver's Desktop tab
        QString prefs = QCoreApplication::applicationDirPath() + "/../../../System Preferences.app/Contents/MacOS/System Preferences";
        QProcess::startDetached(QFileInfo(prefs).absoluteFilePath(), {"--pane", "desktop"});
    });
    // Clean Up comes with icons on the desktop (they are for later); Arrange By and Show View Options are there
    QMenu *arrange = menu->addMenu(t("N148", "Arrange By"));
    for (const char *by : {"None", "Name", "Date Modified", "Date Created", "Size", "Kind", "Label"}) {
        QAction *a = arrange->addAction(QString::fromLatin1(by));
        a->setCheckable(true);
        a->setChecked(QByteArray(by) == "None");
    }
    menu->addAction(t("N169.5", "Show View Options"));
    Aqua::prepareMenu(menu);
    menu->popup(global);
}
