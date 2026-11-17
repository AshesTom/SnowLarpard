// SPDX-License-Identifier: LGPL-2.1-or-later
#include "apps.h"

#include "assets.h"
#include "x11.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <signal.h>

namespace {

// Snow Leopard's applications and the Linux programs that stand for them, first found first.
struct MacApp {
    const char *bundle;
    const char *name;
    QStringList candidates;   // .desktop ids
};
const QVector<MacApp> &macApps()
{
    static const QVector<MacApp> table = {
        {"/Applications/Safari.app", "Safari",
         {"firefox", "org.mozilla.firefox", "chromium", "google-chrome", "brave-browser", "org.gnome.Epiphany", "librewolf",
          "vivaldi-stable", "zen", "zen-browser"}},
        {"/Applications/Mail.app", "Mail",
         {"org.mozilla.Thunderbird", "thunderbird", "net.thunderbird.Thunderbird", "org.kde.kmail2", "org.gnome.Evolution",
          "org.gnome.Geary", "betterbird"}},
        {"/Applications/iChat.app", "iChat", {"org.telegram.desktop", "signal-desktop", "discord", "im.dino.Dino", "pidgin"}},
        {"/Applications/Address Book.app", "Address Book", {"org.gnome.Contacts", "org.kde.kaddressbook"}},
        {"/Applications/iCal.app", "iCal", {"org.gnome.Calendar", "org.kde.korganizer", "org.kde.merkuro.calendar"}},
        {"/Applications/Preview.app", "Preview",
         {"org.kde.okular", "okularApplication_pdf", "org.gnome.Evince", "org.gnome.Papers", "org.kde.gwenview", "org.gnome.Loupe",
          "eog", "org.gnome.eog"}},
        {"/Applications/iTunes.app", "iTunes",
         {"org.kde.elisa", "org.gnome.Rhythmbox3", "rhythmbox", "org.strawberrymusicplayer.strawberry", "audacious",
          "org.gnome.Lollypop"}},
        {"/Applications/Photo Booth.app", "Photo Booth", {"org.gnome.Cheese", "org.kde.kamoso", "org.gnome.Snapshot"}},
        {"/Applications/QuickTime Player.app", "QuickTime Player", {"mpv", "vlc", "org.kde.haruna", "org.gnome.Totem", "io.mpv.Mpv"}},
        {"/Applications/DVD Player.app", "DVD Player", {"vlc"}},
        {"/Applications/TextEdit.app", "TextEdit", {"org.kde.kwrite", "org.gnome.TextEditor", "org.xfce.mousepad", "gedit"}},
        {"/Applications/Calculator.app", "Calculator", {"org.kde.kcalc", "org.gnome.Calculator", "qalculate-gtk"}},
        {"/Applications/Stickies.app", "Stickies", {"org.kde.knotes", "xpad"}},
        {"/Applications/Dictionary.app", "Dictionary", {"org.gnome.Dictionary"}},
        {"/Applications/Font Book.app", "Font Book", {"org.gnome.font-viewer", "org.kde.kfontview"}},
        {"/Applications/Image Capture.app", "Image Capture", {"org.kde.skanlite", "simple-scan", "org.gnome.SimpleScan"}},
        {"/Applications/Chess.app", "Chess", {"org.kde.knights", "org.gnome.Chess"}},
        {"/Applications/System Preferences.app", "System Preferences",
         {"systemsettings", "org.kde.systemsettings", "org.gnome.Settings", "gnome-control-center", "xfce-settings-manager"}},
        {"/Applications/Utilities/Terminal.app", "Terminal",
         {"org.kde.konsole", "org.gnome.Console", "org.gnome.Terminal", "xfce4-terminal", "Alacritty", "kitty", "xterm"}},
        {"/Applications/Utilities/Activity Monitor.app", "Activity Monitor",
         {"org.kde.plasma-systemmonitor", "org.gnome.SystemMonitor", "gnome-system-monitor", "htop"}},
        {"/Applications/Utilities/Disk Utility.app", "Disk Utility", {"org.kde.partitionmanager", "org.gnome.DiskUtility", "gparted"}},
        {"/Applications/Utilities/Console.app", "Console", {"org.gnome.Logs", "org.kde.kjournaldbrowser"}},
        {"/Applications/Utilities/Grab.app", "Grab", {"org.kde.spectacle", "org.gnome.Screenshot"}},
    };
    return table;
}

const QStringList &fileManagers()
{
    static const QStringList list = {"org.kde.dolphin", "org.gnome.Nautilus", "thunar", "pcmanfm-qt", "pcmanfm", "nemo", "caja"};
    return list;
}

QStringList splitExec(const QString &exec)
{
    // Desktop Entry quoting, then the field codes dropped (%f %F %u %U %i %c %k...)
    QStringList args;
    QString cur;
    bool quoted = false, any = false;
    for (int i = 0; i < exec.size(); ++i) {
        QChar c = exec[i];
        if (quoted) {
            if (c == '\\' && i + 1 < exec.size())
                cur += exec[++i];
            else if (c == '"')
                quoted = false;
            else
                cur += c;
        } else if (c == '"') {
            quoted = any = true;
        } else if (c.isSpace()) {
            if (any || !cur.isEmpty())
                args << cur;
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.isEmpty())
        args << cur;
    QStringList out;
    for (const QString &a : args) {
        if (a.size() == 2 && a[0] == '%')
            continue;
        out << QString(a).replace("%%", "%");
    }
    return out;
}

struct Catalog {
    QVector<App> apps;
    QHash<QString, int> byId;
};

Catalog load()
{
    Catalog c;
    QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "applications",
                                                 QStandardPaths::LocateDirectory);
    for (const QString &dir : dirs) {   // the user's first: they win
        QDirIterator it(dir, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString file = it.next();
            QString id = QDir(dir).relativeFilePath(file).chopped(8).replace('/', '-');
            if (c.byId.contains(id))
                continue;
            QSettings s(file, QSettings::IniFormat);
            s.beginGroup("Desktop Entry");
            App a;
            a.id = id;
            // QSettings reads "a;b" as a list and doesn't know the format's escapes: Name and Exec are plain.
            auto text = [&](const char *key) {
                QVariant v = s.value(key);
                return v.metaType().id() == QMetaType::QStringList ? v.toStringList().join(',') : v.toString();
            };
            if (text("Type") != "Application" || text("Hidden") == "true") {
                c.byId.insert(id, -1);   // hidden by the user's own copy
                continue;
            }
            a.name = text("Name");
            a.exec = splitExec(text("Exec"));
            a.iconName = text("Icon");
            a.wmClass = text("StartupWMClass");
            if (a.wmClass.isEmpty() && !a.exec.isEmpty())
                a.wmClass = QFileInfo(a.exec.first()).fileName();
            if (a.exec.isEmpty())
                continue;
            c.byId.insert(id, int(c.apps.size()));
            c.apps.append(a);
        }
    }
    return c;
}

const Catalog &catalog()
{
    static Catalog c = load();
    return c;
}

// The shell's own Mac applications, built beside it (.local/build/apps/<Name>.app/Contents/MacOS/<Name>):
// they stand for themselves, before any Linux program
App ownApp(const QString &bundle)
{
    const QString name = QFileInfo(bundle).completeBaseName();
    // from any of the shell's programs: apps/X, apps/X.app/Contents/MacOS/X
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 4; ++i) {
        const QString exe = dir.filePath(name + ".app/Contents/MacOS/" + name);
        if (QFileInfo(exe).isExecutable()) {
            App a;
            a.id = "mac-" + name;
            a.name = name;
            a.macBundle = bundle;
            a.exec = {exe};
            a.wmClass = name;   // AquaKit's WM_CLASS (and Wayland app_id): the program's name
            return a;
        }
        if (!dir.cdUp())
            break;
    }
    return {};
}

const QStringList &ownBundles()
{
    static const QStringList list = {"/Applications/System Preferences.app", "/Applications/Preview.app",
                                     "/Applications/Mail.app", "/Applications/Photo Booth.app",
                                     "/Applications/Calculator.app", "/Applications/Stickies.app",
                                     "/System/Library/CoreServices/Software Update.app"};
    return list;
}

App withMac(App a, const MacApp &mac)
{
    a.name = QString::fromUtf8(mac.name);
    a.macBundle = QString::fromUtf8(mac.bundle);
    return a;
}

} // namespace

namespace Apps {

const QVector<App> &all() { return catalog().apps; }

App byId(const QString &id)
{
    int i = catalog().byId.value(id, -1);
    return i < 0 ? App{} : catalog().apps[i];
}

App forMacBundle(const QString &bundle)
{
    if (bundle.endsWith("/Finder.app"))
        return finder();
    if (ownBundles().contains(bundle)) {
        App own = ownApp(bundle);
        if (own.valid())
            return own;
    }
    for (const MacApp &mac : macApps()) {
        if (bundle != QLatin1String(mac.bundle))
            continue;
        for (const QString &id : mac.candidates) {
            App a = byId(id);
            if (a.valid())
                return withMac(a, mac);
        }
    }
    return {};
}

App finder()
{
    App f;
    f.id = "finder";
    f.name = "Finder";
    f.macBundle = "/System/Library/CoreServices/Finder.app";
    // Our Finder's windows (FinderWindows, on AquaKit, its windows' WM_CLASS "Finder"); the desktop's
    // file manager when it isn't built
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 4; ++i) {
        const QString exe = dir.filePath("Finder.app/Contents/MacOS/FinderWindows");
        if (QFileInfo(exe).isExecutable()) {
            f.exec = {exe};
            f.wmClass = "Finder";
            return f;
        }
        if (!dir.cdUp())
            break;
    }
    for (const QString &id : fileManagers()) {
        App fm = byId(id);
        if (fm.valid()) {
            f.exec = fm.exec;
            f.wmClass = fm.wmClass;
            f.iconName = fm.iconName;
            break;
        }
    }
    if (f.exec.isEmpty())
        f.exec = {"xdg-open", QDir::homePath()};
    return f;
}

App forWindow(const QString &wmClass, const QString &instance, uint32_t pid)
{
    auto same = [](const QString &a, const QString &b) { return !a.isEmpty() && a.compare(b, Qt::CaseInsensitive) == 0; };
    // the Finder's own windows (FinderWindows names them "Finder")
    if (same(wmClass, "Finder") || same(instance, "Finder"))
        return finder();
    for (const QString &bundle : ownBundles()) {
        const QString name = QFileInfo(bundle).completeBaseName();
        if (same(name, wmClass) || same(name, instance)) {
            App own = ownApp(bundle);
            if (own.valid())
                return own;
        }
    }
    App found;
    for (const App &a : all()) {
        if (same(a.wmClass, wmClass) || same(a.wmClass, instance) || same(a.id, wmClass) || same(a.id, instance) ||
            same(a.id.section('.', -1), wmClass)) {
            found = a;
            break;
        }
    }
    if (!found.valid() && pid) {
        // The process' executable against the entries' commands (Electron and Java programs' classes are no help)
        QString exe = QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(pid))).fileName();
        for (const App &a : all())
            if (!exe.isEmpty() && QFileInfo(a.exec.first()).fileName() == exe) {
                found = a;
                break;
            }
    }
    if (!found.valid()) {
        found.id = "x-window-" + (wmClass.isEmpty() ? instance : wmClass);
        found.name = wmClass.isEmpty() ? instance : wmClass;
        found.wmClass = wmClass;
        return found;
    }
    App f = finder();
    if (same(found.wmClass, f.wmClass))
        return f;
    for (const MacApp &mac : macApps())
        if (mac.candidates.contains(found.id))
            return withMac(found, mac);
    return found;
}

QImage icon(const App &app, int size)
{
    if (!app.macBundle.isEmpty()) {
        QString icns = app.id == "finder" ? "/System/Library/CoreServices/Finder.app/Contents/Resources/Finder.icns"
                                          : Assets::bundleIcon(app.macBundle);
        if (!icns.isEmpty() && Assets::exists(icns)) {
            QImage img = Assets::image(icns, QSize(size, size));
            if (!img.isNull())
                return img;
        }
    }
    QIcon icon = QFileInfo(app.iconName).isAbsolute() ? QIcon(app.iconName) : QIcon::fromTheme(app.iconName);
    if (icon.isNull())
        icon = QIcon::fromTheme("application-x-executable");
    return icon.pixmap(size, size).toImage();
}

bool launch(const App &app, const QStringList &files)
{
    if (app.exec.isEmpty())
        return false;
    QStringList args = app.exec.mid(1) + files;
    return QProcess::startDetached(app.exec.first(), args, QDir::homePath());
}

bool openFile(const QString &path)
{
    // PDFs and pictures are Preview's (LaunchServices' default on 10.6): ours once built, else the
    // Linux viewer standing for it; the rest goes to the desktop's own choice
    static const QStringList preview = {"pdf", "png", "jpg", "jpeg", "tif", "tiff", "gif", "bmp", "icns"};
    if (preview.contains(QFileInfo(path).suffix().toLower())) {
        const App a = forMacBundle("/Applications/Preview.app");
        if (a.valid() && launch(a, {path}))
            return true;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QVector<QPair<uint32_t, App>> aquaKitApps()
{
    // the abstract sockets' names in /proc/net/unix ("@snowleopard-menus-<pid>"), the processes of
    // this display
    QVector<QPair<uint32_t, App>> out;
    QFile f(QStringLiteral("/proc/net/unix"));
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QByteArray display = qgetenv("DISPLAY");
    QSet<uint32_t> seen;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const int at = line.indexOf("@snowleopard-menus-");
        if (at < 0)
            continue;
        const uint32_t pid = line.mid(at + 19).trimmed().toUInt();
        if (!pid || seen.contains(pid) || kill(pid_t(pid), 0) != 0)
            continue;
        seen.insert(pid);
        QFile env(QStringLiteral("/proc/%1/environ").arg(pid));
        if (!env.open(QIODevice::ReadOnly))
            continue;
        bool same = false;
        for (const QByteArray &v : env.readAll().split('\0'))
            if (v.startsWith("DISPLAY="))
                same = v.mid(8) == display;
        if (!same)
            continue;
        const App a = forPid(pid);
        if (a.valid())
            out.append({pid, a});
    }
    return out;
}

App forPid(uint32_t pid)
{
    // AquaKit names its windows by the executable (WM_CLASS): the same name finds the application
    const QString exe = QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(pid))).fileName();
    return exe.isEmpty() ? App{} : forWindow(exe, exe, pid);
}

uint32_t frontWithoutWindows(uint32_t *serial)
{
    const QVector<uint32_t> v = X11::cardinals(X11::root(), X11::atom("_SNOWLEOPARD_FRONT_APP"), XCB_ATOM_CARDINAL);
    if (v.size() < 2 || kill(pid_t(v[0]), 0) != 0)
        return 0;
    if (serial)
        *serial = v[1];
    return v[0];
}

} // namespace Apps
