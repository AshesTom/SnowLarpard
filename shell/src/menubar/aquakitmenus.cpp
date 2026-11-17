// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aquakitmenus.h"

#include "aqua.h"
#include "session.h"

#include <QLocalSocket>
#include <QMenu>

namespace AquaKitMenus {

namespace {

struct Entry {
    int depth = 0;
    QChar kind;   // M submenu, I item, S separator
    bool enabled = false, checked = false;
    QString keys, action, path, title;
};

bool connectTo(QLocalSocket &socket, int pid)
{
    socket.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
    socket.connectToServer("snowleopard-menus-" + QString::number(pid));
    return socket.waitForConnected(150);
}

QVector<Entry> fetch(int pid)
{
    QVector<Entry> entries;
    QLocalSocket socket;
    if (!connectTo(socket, pid))
        return entries;
    socket.write("menus\n");
    socket.waitForBytesWritten(150);
    QByteArray data;
    // the application answers between two of its events: a menu waits for it a little
    while (socket.state() == QLocalSocket::ConnectedState && socket.waitForReadyRead(400))
        data += socket.readAll();
    data += socket.readAll();
    for (const QByteArray &raw : data.split('\n')) {
        const QStringList f = QString::fromUtf8(raw).split('\t');
        if (f.size() < 8)
            continue;
        Entry e;
        e.depth = f[0].toInt();
        e.kind = f[1].isEmpty() ? QChar('I') : f[1][0];
        e.enabled = f[2] == "1";
        e.checked = f[3] == "1";
        e.keys = f[4];
        e.action = f[5];
        e.path = f[6];
        e.title = f[7];
        entries.append(e);
    }
    return entries;
}

void send(int pid, const QString &path)
{
    QLocalSocket socket;
    if (!connectTo(socket, pid))
        return;
    socket.write(("do " + path + "\n").toUtf8());
    socket.waitForBytesWritten(150);
    socket.disconnectFromServer();
}

// the items after `from` one level deeper than it, into `menu`; returns the index after them
int fill(QMenu *menu, const QVector<Entry> &entries, int from, int pid, const App &app)
{
    const int depth = entries[from].depth + 1;
    int i = from + 1;
    while (i < entries.size() && entries[i].depth >= depth) {
        const Entry &e = entries[i];
        if (e.depth > depth) {   // under a submenu already done
            ++i;
            continue;
        }
        if (e.kind == 'S') {
            menu->addSeparator();
        } else if (e.kind == 'M') {
            QMenu *sub = menu->addMenu(e.title);
            fill(sub, entries, i, pid, app);
            sub->menuAction()->setEnabled(e.enabled);
            Aqua::prepareMenu(sub);
        } else {
            QAction *a = menu->addAction(e.keys.isEmpty() ? e.title : e.title + '\t' + e.keys);
            a->setEnabled(e.enabled);
            if (e.checked) {
                a->setCheckable(true);
                a->setChecked(true);
            }
            const QString action = e.action, path = e.path;
            QObject::connect(a, &QAction::triggered, a, [pid, app, action, path] {
                // the windows' business, done by the shell as for every application
                if (action == "hide:")
                    Session::hideApp(app);
                else if (action == "hideOtherApplications:")
                    Session::hideOthers(app);
                else if (action == "unhideAllApplications:")
                    Session::showAll();
                else
                    send(pid, path);
            });
        }
        ++i;
    }
    return i;
}

} // namespace

bool available(int pid)
{
    if (pid <= 0)
        return false;
    QLocalSocket socket;
    return connectTo(socket, pid);
}

QVector<QMenu *> build(int pid, const App &app, const std::function<QMenu *()> &windowMenu)
{
    QVector<QMenu *> menus;
    const QVector<Entry> entries = fetch(pid);
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &top = entries[i];
        if (top.depth != 0 || top.kind != 'M')
            continue;
        if (top.title == "Window") {
            // the shell's, unless the application's has actions of its own (Stickies' Collapse
            // Window, Arrange By): then the application's, its windows listed by it
            static const QStringList standard = {"performMiniaturize:", "miniaturize:", "performZoom:", "zoom:",
                                                 "arrangeInFront:", "makeKeyAndOrderFront:", "performClose:", ""};
            bool own = false;
            for (int j = i + 1; j < entries.size() && entries[j].depth > 0; ++j)
                if (entries[j].kind == 'I' && !standard.contains(entries[j].action))
                    own = true;
            if (!own) {
                menus.append(windowMenu());
                continue;
            }
        }
        auto *menu = new QMenu(top.title);
        const QString path = top.path;
        // asked for again when it opens: its items as the application validates them now
        QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, pid, app, path] {
            menu->clear();
            const QVector<Entry> now = fetch(pid);
            for (int j = 0; j < now.size(); ++j)
                if (now[j].depth == 0 && now[j].path == path) {
                    fill(menu, now, j, pid, app);
                    break;
                }
            Aqua::prepareMenu(menu);
        });
        fill(menu, entries, i, pid, app);
        menus.append(menu);
    }
    return menus;
}

} // namespace AquaKitMenus
