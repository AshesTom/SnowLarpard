// SPDX-License-Identifier: LGPL-2.1-or-later
// WindowServer-wayland: the window server of the Wayland session, a compositor of its own.
//
//   WindowServer-wayland [--socket NAME] [--size WxH] [--windowed] [--no-xwayland] [--session PROGRAM ARGS...]
//
// On the compositor or screen Qt runs it on (QT_QPA_PLATFORM: wayland under KWin or cage, eglfs on
// the screen itself, xcb in a window), full screen unless --windowed. With --session, the program
// runs on it (WAYLAND_DISPLAY, and DISPLAY for Xwayland's X11 programs) and the server ends with it.
#include "aqua.h"
#include "assets.h"
#include "server.h"
#include "xwayland.h"

#include <QGuiApplication>
#include <QFile>
#include <QProcess>
#include <QQuickWindow>
#include <QSize>
#include <QStringList>
#include <QSocketNotifier>
#include <cstdio>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

static int signalPipe[2] = {-1, -1}, quitPipe[2] = {-1, -1};

int main(int argc, char **argv)
{
    QString socket, size;
    bool windowed = false, xwayland = true;
    QStringList session;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--socket" && i + 1 < argc)
            socket = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--size" && i + 1 < argc)
            size = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--windowed")
            windowed = true;
        else if (a == "--no-xwayland")
            xwayland = false;
        else if (a == "--session") {
            for (++i; i < argc; ++i)
                session << QString::fromLocal8Bit(argv[i]);
        } else {
            fprintf(stderr, "usage: WindowServer-wayland [--socket NAME] [--size WxH] [--windowed] [--no-xwayland] [--session PROGRAM ARGS...]\n");
            return 2;
        }
    }
    // the applications' GPU buffers (Firefox's, Chromium's) as dma-bufs, shared memory otherwise
    if (qEnvironmentVariableIsEmpty("QT_WAYLAND_HARDWARE_INTEGRATION"))
        qputenv("QT_WAYLAND_HARDWARE_INTEGRATION", "linux-dmabuf-unstable-v1");
    // its own window's output (under KWin), not a Wayland display of its own for it to use
    QQuickWindow::setDefaultAlphaBuffer(false);
    // on what Qt finds (QT_QPA_PLATFORM), not the shell programs' xcb (Aqua::preinit)
    qunsetenv("SESSION_MANAGER");
    QGuiApplication app(argc, argv);
    app.setApplicationName("WindowServer");
    app.setQuitOnLastWindowClosed(false);
    Assets::loadFonts();
    std::signal(SIGPIPE, SIG_IGN);

    QSize wanted;
    if (const QStringList wh = size.split('x'); wh.size() == 2)
        wanted = QSize(wh[0].toInt(), wh[1].toInt());
    if (socket.isEmpty()) {
        // the first wayland-sl-N free in the runtime directory
        const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
        for (int n = 0; n < 32; ++n) {
            const QString name = QStringLiteral("wayland-sl-%1").arg(n);
            if (!QFile::exists(dir + "/" + name + ".lock") || !QFile::exists(dir + "/" + name)) {
                socket = name;
                break;
            }
        }
    }
    Server server;
    if (!server.start(socket, !windowed, wanted)) {
        fprintf(stderr, "WindowServer-wayland: no compositor (socket %s)\n", qPrintable(socket));
        return 1;
    }
    fprintf(stderr, "WindowServer-wayland: WAYLAND_DISPLAY=%s\n", qPrintable(server.socketName()));
    if (xwayland)
        server.xwayland()->listen();

    // SIGTERM, SIGINT, SIGHUP: quit properly (Xwayland's display given back)
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, quitPipe) == 0) {
        for (int sig : {SIGTERM, SIGINT, SIGHUP})
            std::signal(sig, [](int) { (void)!write(quitPipe[1], "q", 1); });
        auto *n = new QSocketNotifier(quitPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(n, &QSocketNotifier::activated, &app, [&app] { app.exit(0); });
    }
    // SIGUSR1: the screen as it is drawn, to $SNOWLEOPARD_SCREENSHOT (the tests' way to look at it)
    if (!qEnvironmentVariableIsEmpty("SNOWLEOPARD_SCREENSHOT") && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, signalPipe) == 0) {
        std::signal(SIGUSR1, [](int) { (void)!write(signalPipe[1], "s", 1); });
        auto *n = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(n, &QSocketNotifier::activated, &server, [&server] {
            char c;
            (void)!read(signalPipe[0], &c, 1);
            server.grabScreen([](const QImage &image) { image.save(qEnvironmentVariable("SNOWLEOPARD_SCREENSHOT"), "PNG"); });
        });
    }

    QProcess child;
    if (!session.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("WAYLAND_DISPLAY", server.socketName());
        env.insert("XDG_SESSION_TYPE", "wayland");
        env.insert("SNOWLEOPARD_WINDOW_SERVER", "wayland");
        env.remove("QT_QPA_PLATFORM");
        env.remove("QT_WAYLAND_HARDWARE_INTEGRATION");
        env.remove("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION");
        if (xwayland && !server.xwayland()->displayName().isEmpty())
            env.insert("DISPLAY", server.xwayland()->displayName());
        else
            env.remove("DISPLAY");
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        QObject::connect(&child, &QProcess::finished, &app, [&app](int code) { app.exit(code); });
        QObject::connect(&child, &QProcess::errorOccurred, &app, [&app](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart)
                app.exit(127);
        });
        child.start(session.first(), session.mid(1));
    }
    const int code = app.exec();
    if (child.state() != QProcess::NotRunning) {
        child.terminate();
        child.waitForFinished(2000);
    }
    return code;
}
