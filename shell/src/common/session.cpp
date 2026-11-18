// SPDX-License-Identifier: LGPL-2.1-or-later
#include "session.h"

#include "assets.h"
#include "x11.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <csignal>
#include <pwd.h>
#include <unistd.h>

namespace Session {

QString userFullName()
{
    if (passwd *pw = getpwuid(getuid())) {
        QString gecos = QString::fromLocal8Bit(pw->pw_gecos).section(',', 0, 0);
        return gecos.isEmpty() ? QString::fromLocal8Bit(pw->pw_name) : gecos;
    }
    return qEnvironmentVariable("USER");
}

QVector<xcb_window_t> windowsOf(const App &app)
{
    QVector<xcb_window_t> out;
    for (xcb_window_t w : X11::clientWindows())
        if (Apps::forWindow(X11::wmClass(w), X11::wmInstance(w), X11::pid(w)).id == app.id)
            out.append(w);
    return out;
}

void hideApp(const App &app)
{
    for (xcb_window_t w : windowsOf(app))
        if (!X11::minimized(w))
            X11::hide(w);
}

void hideOthers(const App &app)
{
    for (xcb_window_t w : X11::clientWindows())
        if (Apps::forWindow(X11::wmClass(w), X11::wmInstance(w), X11::pid(w)).id != app.id && !X11::minimized(w))
            X11::hide(w);
}

void showAll()
{
    for (xcb_window_t w : X11::clientWindows())
        if (X11::hidden(w))   // the hidden ones; the minimized stay in the Dock
            X11::activate(w);
}

void bringToFront(const App &app)
{
    for (xcb_window_t w : windowsOf(app))   // bottom to top: the topmost ends on top
        X11::activate(w);
}

void quitApp(const App &app)
{
    for (xcb_window_t w : windowsOf(app))
        X11::close(w);
}

namespace {

// loginwindow, beside the shell's programs (apps/loginwindow.app/Contents/MacOS/loginwindow)
QString loginWindow()
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 5; ++i) {
        const QString exe = dir.filePath("loginwindow.app/Contents/MacOS/loginwindow");
        if (QFileInfo(exe).isExecutable())
            return exe;
        if (!dir.cdUp())
            break;
    }
    return {};
}

} // namespace

void logOut(bool ask)
{
    const QString exe = loginWindow();
    if (!exe.isEmpty())
        QProcess::startDetached(exe, {ask ? "--logout" : "--logout-now"}, QDir::homePath());
}

void aboutThisMac()
{
    // loginwindow's panel (shell/src/loginwindow, its nib from the DVD): brought forward when it is
    // open, else started (apps/loginwindow.app/Contents/MacOS/loginwindow beside the shell's programs)
    // (the window manager's whole list: the panel is out of clientWindows(), being out of the Dock)
    for (xcb_window_t w : X11::cardinals(X11::root(), X11::atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW))
        if (X11::wmClass(w) == QLatin1String("loginwindow")) {
            X11::activate(w);
            return;
        }
    const QString exe = loginWindow();
    if (!exe.isEmpty())
        QProcess::startDetached(exe, {"--about"}, QDir::homePath());
}

void forceQuit()
{
    // Force Quit Applications: the running applications, by their windows' processes
    static QDialog *dialog = nullptr;
    if (!dialog) {
        dialog = new QDialog(nullptr, Qt::Window);
        dialog->setWindowTitle("Force Quit Applications");
        auto *lay = new QVBoxLayout(dialog);
        auto *intro = new QLabel("If an application doesn't respond for a while, select its name and click Force Quit.");
        intro->setWordWrap(true);
        intro->setFont(Assets::uiFont(11));
        auto *list = new QListWidget;
        list->setIconSize(QSize(16, 16));
        auto *button = new QPushButton("Force Quit");
        button->setDefault(true);
        lay->addWidget(intro);
        lay->addWidget(list);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        buttons->addWidget(button);
        lay->addLayout(buttons);
        dialog->resize(330, 300);
        QObject::connect(dialog, &QDialog::finished, list, &QListWidget::clear);
        QObject::connect(button, &QPushButton::clicked, list, [list] {
            auto *item = list->currentItem();
            if (!item)
                return;
            if (QMessageBox::question(list->window(), "Force Quit",
                                      QString("Do you want to force “%1” to quit?").arg(item->text()) +
                                          "\n\nYou will lose any unsaved changes.") != QMessageBox::Yes)
                return;
            for (const QVariant &pid : item->data(Qt::UserRole).toList())
                ::kill(pid_t(pid.toUInt()), SIGKILL);
            delete item;
        });
        dialog->setProperty("list", QVariant::fromValue<QObject *>(list));
    }
    auto *list = qobject_cast<QListWidget *>(dialog->property("list").value<QObject *>());
    list->clear();
    QHash<QString, QListWidgetItem *> byApp;
    for (xcb_window_t w : X11::clientWindows()) {
        uint32_t pid = X11::pid(w);
        if (!pid)
            continue;
        App app = Apps::forWindow(X11::wmClass(w), X11::wmInstance(w), pid);
        QListWidgetItem *&item = byApp[app.id];
        if (!item) {
            item = new QListWidgetItem(QIcon(QPixmap::fromImage(Apps::icon(app, 32))), app.name, list);
            item->setData(Qt::UserRole, QVariantList());
        }
        QVariantList pids = item->data(Qt::UserRole).toList();
        if (!pids.contains(pid))
            pids.append(pid);
        item->setData(Qt::UserRole, pids);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void emptyTrash()
{
    if (QMessageBox::question(nullptr, "Empty Trash", "Are you sure you want to permanently erase the items in the Trash?\n\n"
                                                      "You can't undo this action.") == QMessageBox::Yes)
        QProcess::startDetached("gio", {"trash", "--empty"});
}

void power(Power what)
{
    bool real = qEnvironmentVariable("SNOWLEOPARD_POWER") == "1";
    if (what == LogOut) {
        logOut(true);
        return;
    }
    if (what == Sleep) {
        if (real)
            QProcess::startDetached("systemctl", {"suspend"});
        return;
    }
    static const char *titles[] = {"", "Are you sure you want to restart your computer now?",
                                   "Are you sure you want to shut down your computer now?"};
    static const char *buttons[] = {"", "Restart", "Shut Down"};
    QMessageBox box;
    box.setText(QString::fromUtf8(titles[what]));
    box.setInformativeText(real ? QString() : QString("This session runs in a window: only Log Out applies here."));
    QPushButton *ok = box.addButton(QString::fromUtf8(buttons[what]), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(ok);
    box.exec();
    if (box.clickedButton() != ok)
        return;
    if (real) {
        QProcess::startDetached("systemctl", {what == Restart ? "reboot" : "poweroff"});
    }
}

} // namespace Session
