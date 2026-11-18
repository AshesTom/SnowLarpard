// SPDX-License-Identifier: LGPL-2.1-or-later
#include "menuextra.h"

#include "assets.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMenu>
#include <QProcess>
#include <QStandardPaths>

QImage menuIcon(const QString &macPath, int height, qreal dpr, bool white)
{
    QImage img = Assets::image(macPath, QSize(height * 4, height) * dpr);
    if (img.isNull())
        return img;
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    if (white) {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), Qt::white);
    }
    return img;
}

void drawCentered(QPainter &p, const QRect &r, const QImage &img)
{
    QSizeF s = img.deviceIndependentSize();
    p.drawImage(QPointF(r.x() + (r.width() - s.width()) / 2, r.y() + (r.height() - s.height()) / 2), img);
}

void openPreferencePane(const QString &pane)
{
    // System Preferences.app beside SystemUIServer.app, as the Finder finds it
    const QString prefs = QCoreApplication::applicationDirPath() + "/../../../System Preferences.app/Contents/MacOS/System Preferences";
    QProcess::startDetached(QFileInfo(prefs).absoluteFilePath(), {"--pane", pane});
}

void removeActions(QMenu *menu, const QList<QAction *> &actions)
{
    for (QAction *a : actions) {
        menu->removeAction(a);
        if (a->parent() == menu)
            a->deleteLater();
    }
}

bool startFirst(const QStringList &programs, const QStringList &args)
{
    for (const QString &program : programs)
        if (!QStandardPaths::findExecutable(program).isEmpty())
            return QProcess::startDetached(program, args);
    return false;
}

