// SPDX-License-Identifier: LGPL-2.1-or-later
#include "findermenus.h"

#include "apps.h"
#include "aqua.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QWidgetAction>

#include <signal.h>
#include <unistd.h>

namespace FinderMenus {

namespace {

// what the front Finder window says (its state file)
struct State {
    bool valid = false;
    QSet<QString> enabled, checked;
    QMap<QString, QString> titles;
    int label = -1;
};

bool s_desktop = false;   // the desktop in front: its state, its commands

QString displayTag()
{
    QString d = qEnvironmentVariable("DISPLAY");
    d.replace('/', '_').replace(':', '_');
    return d;
}

QString desktopSocket()
{
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = "/tmp";
    return runtime + "/snowleopard-desktop-" + QString::number(getuid()) + "-" + displayTag();
}

QString stateFile()
{
    if (s_desktop)
        return desktopSocket() + ".state";
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = "/tmp";
    QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (display.isEmpty())
        display = qEnvironmentVariable("DISPLAY");
    display.replace('/', '_');
    return runtime + "/snowleopard-finder-" + QString::number(getuid()) + "-" + display + ".state";
}

State readState()
{
    State s;
    QFile f(stateFile());
    if (!f.open(QIODevice::ReadOnly))
        return s;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw);
        if (line.startsWith("pid ")) {
            const int pid = line.mid(4).toInt();
            s.valid = pid > 0 && kill(pid, 0) == 0;
        } else if (line.startsWith("E ")) {
            s.enabled.insert(line.mid(2));
        } else if (line.startsWith("C ")) {
            s.checked.insert(line.mid(2));
        } else if (line.startsWith("T ")) {
            const int tab = line.indexOf('\t');
            if (tab > 2)
                s.titles[line.mid(2, tab - 2)] = line.mid(tab + 1);
        } else if (line.startsWith("L ")) {
            s.label = line.mid(2).toInt();
        }
    }
    return s;
}

QString finderExecutable()
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 4; ++i) {
        const QString exe = dir.filePath("Finder.app/Contents/MacOS/FinderWindows");
        if (QFileInfo(exe).isExecutable())
            return exe;
        if (!dir.cdUp())
            break;
    }
    return {};
}

// the commands that act on a window's items (disabled with the desktop in front)
bool needsWindow(const QString &cmd)
{
    static const QSet<QString> global = {"cmdNewFileViewer:", "cmdNewFolder:", "cmdGoToComputer:", "cmdGoHome:", "cmdGoToDesktop:",
                                         "cmdGoToNetwork:", "cmdGoToApplications:", "cmdGoToDocuments:", "cmdGoToUtilities:",
                                         "cmdGoTo:", "cmdConnectTo:", "cmdPreferences:", "cmdEmptyTrash:", "cmdEmptyTrashSecure:",
                                         "cmdFindFile:", "cmdNewSmartFolder:", "cmdClearRecentFolders:"};
    return !global.contains(cmd) && !cmd.startsWith("cmdRecentFoldersItem:");
}

// HIServices' label colours (_sCachedColors) in the menus' order: Red, Orange, Yellow, Green, Blue, Purple, Gray
const QColor LabelColors[7] = {QColor(0xfb, 0x62, 0x59), QColor(0xf6, 0xa9, 0x41), QColor(0xef, 0xda, 0x44), QColor(0xb3, 0xd6, 0x44),
                               QColor(0x57, 0xa1, 0xff), QColor(0xbf, 0x8c, 0xd8), QColor(0xa8, 0xa8, 0xa8)};
const int LabelOrder[7] = {6, 7, 5, 2, 4, 3, 1};

// File's "Label:" and its swatches (TLabelViewForMenu)
class LabelRow : public QWidget {
public:
    LabelRow(int current, bool enabled, QMenu *menu) : m_current(current), m_menu(menu)
    {
        setEnabled(enabled);
        setMouseTracking(true);
        setFixedSize(21 + 8 * 19 + 20, 40);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(isEnabled() ? Qt::black : QColor(0, 0, 0, 77));
        p.drawText(QRect(21, 0, width() - 21, 19), Qt::AlignVCenter | Qt::AlignLeft, "Label:");
        for (int k = 0; k < 8; ++k) {
            const QRectF r(21 + k * 19 + 0.5, 20.5, 14, 14);
            const int index = k == 0 ? 0 : LabelOrder[k - 1];
            if (k == m_hover || index == m_current) {
                p.setPen(QColor(0, 0, 0, k == m_hover ? 115 : 64));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(-3, -3, 3, 3));
            }
            if (k == 0) {
                p.setPen(QPen(QColor(0, 0, 0, 140), 1.5));
                p.drawLine(r.topLeft() + QPointF(3.5, 3.5), r.bottomRight() - QPointF(3.5, 3.5));
                p.drawLine(QPointF(r.right() - 3.5, r.top() + 3.5), QPointF(r.left() + 3.5, r.bottom() - 3.5));
                continue;
            }
            const QColor c = LabelColors[k - 1];
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0, c.lighter(125));
            g.setColorAt(1, c);
            p.setBrush(g);
            p.setPen(c.darker(165));
            p.drawRect(r);
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int k = swatchAt(e->position().x(), e->position().y());
        if (k != m_hover) {
            m_hover = k;
            update();
        }
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        const int k = swatchAt(e->position().x(), e->position().y());
        if (k < 0 || !isEnabled())
            return;
        send(QString("cmdSetLabel%1:").arg(k == 0 ? 0 : LabelOrder[k - 1]));
        if (m_menu)
            m_menu->close();
    }

private:
    int swatchAt(double x, double y) const
    {
        if (y < 17)
            return -1;
        const int k = int((x - 21 + 2) / 19);
        return x >= 19 && k >= 0 && k < 8 ? k : -1;
    }
    int m_current, m_hover = -1;
    QMenu *m_menu;
};

struct Entry {
    const char *title;   // the nib's; "-" a separator
    const char *keys;    // the key equivalent as drawn
    const char *command;
};

// the desktop's New Folder: "untitled folder", "untitled folder 2"... in the Desktop folder
void newFolderOnDesktop()
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QDir().mkpath(desktop);
    QString name = "untitled folder";
    for (int n = 2; QFileInfo::exists(desktop + "/" + name); ++n)
        name = QString("untitled folder %1").arg(n);
    QDir(desktop).mkdir(name);
}

} // namespace

bool finderRunning() { return readState().valid; }

void send(const QString &command)
{
    if (s_desktop) {
        // the desktop's icons take it (and hand the Finder's windows what isn't theirs)
        QLocalSocket socket;
        socket.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
        socket.connectToServer("snowleopard-desktop-" + QString::number(getuid()) + "-" + displayTag());
        if (socket.waitForConnected(500)) {
            socket.write(("cmd:" + command + "\n").toUtf8());
            socket.waitForBytesWritten(500);
            socket.disconnectFromServer();
            return;
        }
    }
    const QString exe = finderExecutable();
    if (!exe.isEmpty())
        QProcess::startDetached(exe, {"--command", command});
}

QVector<QMenu *> build(bool desktop)
{
    s_desktop = desktop;
    QVector<QMenu *> menus;
    // each menu's items made again when it opens, from the state then
    auto make = [desktop](const QString &title, QVector<Entry> entries, std::function<void(QMenu *, const State &)> extra = {}) {
        auto *menu = new QMenu(title);
        QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, entries, desktop, extra] {
            menu->clear();
            s_desktop = desktop;
            State s = readState();
            const bool window = s.valid;
            for (const Entry &e : entries) {
                if (QByteArray(e.title) == "-") {
                    menu->addSeparator();
                    continue;
                }
                const QString cmd = e.command;
                QString title = window && s.titles.contains(cmd) ? s.titles.value(cmd) : QString::fromUtf8(e.title);
                if (*e.keys)
                    title += "\t" + QString::fromUtf8(e.keys);
                const bool onDesktop = desktop && cmd == "cmdNewFolder:" && !s.valid;
                QAction *a = menu->addAction(title, [cmd, onDesktop] {
                    if (onDesktop)
                        newFolderOnDesktop();
                    else
                        send(cmd);
                });
                bool enabled;
                if (cmd.isEmpty())
                    enabled = false;
                else if (onDesktop)
                    enabled = true;
                else if (needsWindow(cmd))
                    enabled = window && s.enabled.contains(cmd);
                else
                    enabled = window ? s.enabled.contains(cmd) : cmd != "cmdGoToNetwork:";
                a->setEnabled(enabled);
                if (window && s.checked.contains(cmd)) {
                    a->setCheckable(true);
                    a->setChecked(true);
                }
            }
            if (extra)
                extra(menu, s);
            Aqua::prepareMenu(menu);
        });
        return menu;
    };

    menus.append(make("File",
                      {{"New Finder Window", "⌘N", "cmdNewFileViewer:"},
                       {"New Folder", "⇧⌘N", "cmdNewFolder:"},
                       {"New Smart Folder", "⌥⌘N", "cmdNewSmartFolder:"},
                       {"New Burn Folder", "", ""},
                       {"Open", "⌘O", "cmdOpen:"},
                       {"Print", "", ""},
                       {"Close Window", "⌘W", "performClose:"},
                       {"-", "", ""},
                       {"Get Info", "⌘I", "cmdShowStaticInfo:"},
                       {"-", "", ""},
                       {"Compress", "", "cmdArchive:"},
                       {"-", "", ""},
                       {"Duplicate", "⌘D", "cmdDuplicate:"},
                       {"Make Alias", "⌘L", "cmdMakeAlias:"},
                       {"Quick Look", "⌘Y", "cmdQuickLook:"},
                       {"Show Original", "⌘R", "cmdShowOriginal:"},
                       {"Add to Sidebar", "⌘T", "cmdAddToSidebar:"},
                       {"-", "", ""},
                       {"Move to Trash", "⌘⌫", "cmdMoveToTrash:"},
                       {"Eject", "⌘E", "cmdEject:"},
                       {"Burn Disc", "", ""},
                       {"-", "", ""},
                       {"Find", "⌘F", "cmdFindFile:"},
                       {"-", "", ""}},
                      [desktop](QMenu *menu, const State &s) {
                          auto *action = new QWidgetAction(menu);
                          action->setDefaultWidget(new LabelRow(s.label, s.valid && s.enabled.contains("cmdSetLabel:"), menu));
                          menu->addAction(action);
                      }));
    menus.append(make("Edit", {{"Undo", "⌘Z", "undo:"},
                               {"Redo", "⇧⌘Z", ""},
                               {"-", "", ""},
                               {"Cut", "⌘X", ""},
                               {"Copy", "⌘C", "copy:"},
                               {"Paste", "⌘V", "paste:"},
                               {"Select All", "⌘A", "selectAll:"},
                               {"-", "", ""},
                               {"Show Clipboard", "", ""}}));
    menus.append(make("View",
                      {{"as Icons", "⌘1", "cmdViewAsIcons:"},
                       {"as List", "⌘2", "cmdViewAsList:"},
                       {"as Columns", "⌘3", "cmdViewAsColumns:"},
                       {"as Cover Flow", "⌘4", "cmdViewAsFlow:"},
                       {"-", "", ""},
                       {"Clean Up", "", "cmdCleanUp:"}},
                      [](QMenu *menu, const State &s) {
                          const bool window = s.valid;
                          QMenu *arrange = menu->addMenu("Arrange By");
                          arrange->setEnabled(window && s.enabled.contains("cmdArrange:"));
                          static const char *const by[][3] = {{"Name", "⌃⌘1", "Name"},          {"Date Modified", "⌃⌘2", "ModDate"},
                                                              {"Date Created", "⌃⌘3", "CreationDate"}, {"Size", "⌃⌘4", "PhysicalSize"},
                                                              {"Kind", "⌃⌘5", "Kind"},           {"Label", "⌃⌘6", "Label"}};
                          for (const auto &b : by) {
                              const QString keep = QString("cmdKeepArrangedBy%1:").arg(b[2]);
                              QAction *a = arrange->addAction(QString("%1\t%2").arg(b[0], b[1]),
                                                              [b] { send(QString("cmdArrangeBy%1:").arg(b[2])); });
                              if (s.checked.contains(keep)) {
                                  a->setCheckable(true);
                                  a->setChecked(true);
                              }
                          }
                          Aqua::prepareMenu(arrange);
                          menu->addSeparator();
                          for (const char *c : {"cmdToggleBottombar:", "cmdToggleStatusBar:", "cmdToggleSidebar:"}) {
                              static const QMap<QString, QString> defaults = {{"cmdToggleBottombar:", "Show Path Bar"},
                                                                              {"cmdToggleStatusBar:", "Hide Status Bar"},
                                                                              {"cmdToggleSidebar:", "Hide Sidebar\t⌥⌘S"}};
                              QString t = window && s.titles.contains(c) ? s.titles.value(c) : defaults.value(c);
                              if (QString(c) == "cmdToggleSidebar:" && !t.contains('\t'))
                                  t += "\t⌥⌘S";
                              menu->addAction(t, [c] { send(c); })->setEnabled(window);
                          }
                          menu->addSeparator();
                          QString toolbar = window && s.titles.contains("cmdToggleToolbar:") ? s.titles.value("cmdToggleToolbar:") : "Hide Toolbar";
                          menu->addAction(toolbar + "\t⌥⌘T", [] { send("cmdToggleToolbar:"); })->setEnabled(window);
                          menu->addAction("Customize Toolbar…")->setEnabled(false);
                          menu->addSeparator();
                          QString options = window && s.titles.contains("cmdToggleViewOptions:") ? s.titles.value("cmdToggleViewOptions:")
                                                                                                    : "Show View Options";
                          menu->addAction(options + "\t⌘J", [] { send("cmdToggleViewOptions:"); })->setEnabled(window);
                      }));
    menus.append(make("Go",
                      {{"Back", "⌘[", "cmdBack:"},
                       {"Forward", "⌘]", "cmdForward:"},
                       {"Enclosing Folder", "⌘↑", "cmdOpenParent:"},
                       {"-", "", ""},
                       {"Computer", "⇧⌘C", "cmdGoToComputer:"},
                       {"Home", "⇧⌘H", "cmdGoHome:"},
                       {"Desktop", "⇧⌘D", "cmdGoToDesktop:"},
                       {"Network", "⇧⌘K", "cmdGoToNetwork:"},
                       {"iDisk", "", ""},
                       {"Applications", "⇧⌘A", "cmdGoToApplications:"},
                       {"Documents", "⇧⌘O", "cmdGoToDocuments:"},
                       {"Utilities", "⇧⌘U", ""},
                       {"-", "", ""}},
                      [](QMenu *menu, const State &) {
                          // Recent Folders (FXRecentFolders), then Go to Folder… and Connect to Server…
                          QMenu *recent = menu->addMenu("Recent Folders");
                          const QString conf = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/finder.conf";
                          QSettings settings(conf, QSettings::IniFormat);
                          const QStringList folders = settings.value("FXRecentFolders").toStringList();
                          for (const QString &f : folders) {
                              if (f.isEmpty())
                                  continue;
                              const QString name = f == QDir::homePath() ? QFileInfo(f).fileName() : f == "/" ? QString("/") : QFileInfo(f).fileName();
                              recent->addAction(name, [f] { send("cmdRecentFoldersItem:" + f); });
                          }
                          recent->addSeparator();
                          recent->addAction("Clear Menu", [] { send("cmdClearRecentFolders:"); })->setEnabled(!folders.isEmpty());
                          Aqua::prepareMenu(recent);
                          menu->addSeparator();
                          menu->addAction("Go to Folder…\t⇧⌘G", [] { send("cmdGoTo:"); });
                          menu->addAction("Connect to Server…\t⌘K", [] { send("cmdConnectTo:"); });
                      }));
    return menus;
}

} // namespace FinderMenus
