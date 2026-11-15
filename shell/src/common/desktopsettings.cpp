// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktopsettings.h"

#include "assets.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>

namespace Desktop {

QString configFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/desktop.conf";
}

Settings load()
{
    QSettings q(configFile(), QSettings::IniFormat);
    Settings s;
    s.picture = q.value("picture").toString();
    s.placement = qBound(1, q.value("placement", int(FillScreen)).toInt(), 5);
    QColor c(q.value("color").toString());
    if (c.isValid())
        s.color = c;
    s.change = qBound(0, q.value("change", 0).toInt(), int(changeIntervals().size()) - 1);
    s.random = q.value("random", false).toBool();
    s.changeFolder = q.value("changeFolder").toString();
    s.folders = q.value("folders").toStringList();
    return s;
}

void save(const Settings &s)
{
    QDir().mkpath(QFileInfo(configFile()).path());
    QSettings q(configFile(), QSettings::IniFormat);
    q.setValue("picture", s.picture);
    q.setValue("placement", s.placement);
    q.setValue("color", s.color.name());
    q.setValue("change", s.change);
    q.setValue("random", s.random);
    q.setValue("changeFolder", s.changeFolder);
    q.setValue("folders", s.folders);
}

QString picturePath(const Settings &s)
{
    if (!s.picture.isEmpty() && QFileInfo::exists(s.picture))
        return s.picture;
    // 10.6's default: Aurora (DefaultDesktop.jpg stands for it)
    for (const char *p : {"/System/Library/CoreServices/DefaultDesktop.jpg", "/Library/Desktop Pictures/Nature/Aurora.jpg",
                          "/Library/Desktop Pictures/Aqua Blue.jpg"})
        if (Assets::exists(p))
            return Assets::path(p);
    return {};
}

QImage render(const Settings &s, const QSize &size)
{
    QImage out(size, QImage::Format_RGB32);
    out.fill(s.color);
    QImageReader reader(picturePath(s));
    reader.setAutoTransform(true);
    const QSize own = reader.size();
    if (!own.isValid())
        return out;
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    auto readAt = [&](const QSize &target) {
        reader.setScaledSize(target);
        return reader.read();
    };
    switch (s.placement) {
    case FitToScreen: {
        QSize target = own.scaled(size, Qt::KeepAspectRatio);
        p.drawImage(QPoint((size.width() - target.width()) / 2, (size.height() - target.height()) / 2), readAt(target));
        break;
    }
    case StretchToFillScreen:
        p.drawImage(QPoint(0, 0), readAt(size));
        break;
    case Center:
        p.drawImage(QPoint((size.width() - own.width()) / 2, (size.height() - own.height()) / 2), reader.read());
        break;
    case Tile: {
        QImage tile = reader.read();
        if (!tile.isNull())
            p.fillRect(out.rect(), QBrush(tile));   // from the top left corner
        break;
    }
    case FillScreen:
    default: {
        QSize target = own.scaled(size, Qt::KeepAspectRatioByExpanding);
        p.drawImage(QPoint((size.width() - target.width()) / 2, (size.height() - target.height()) / 2), readAt(target));
        break;
    }
    }
    return out;
}

QVector<Source> appleSources()
{
    // DSKAppleRootSource::init: the folders' paths and the names of DesktopPictures' Localizable.strings
    static const char *const folders[][2] = {{"", "Desktop Pictures"},    {"/Nature", "Nature"},
                                             {"/Plants", "Plants"},        {"/Art", "Art"},
                                             {"/Black & White", "Black & White"}, {"/Abstract", "Abstract"},
                                             {"/Patterns", "Patterns"},    {"/Solid Colors", "Solid Colors"}};
    QVector<Source> out;
    for (const auto &f : folders) {
        QString path = Assets::path(QStringLiteral("/Library/Desktop Pictures") + QLatin1String(f[0]));
        if (QFileInfo(path).isDir())
            out.append({QString::fromUtf8(f[1]), path, true});
    }
    return out;
}

QStringList pictures(const QString &folder)
{
    QStringList filters;
    for (const QByteArray &f : QImageReader::supportedImageFormats())
        filters << "*." + QString::fromLatin1(f);
    QStringList names = QDir(folder).entryList(filters, QDir::Files | QDir::Readable);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), collator);
    QStringList out;
    for (const QString &n : names)
        out << QDir(folder).filePath(n);
    return out;
}

const QVector<Interval> &changeIntervals()
{
    // the "Change picture:" popup of DSKDesktopWindow.nib, in its order (0 stands for the box unchecked)
    static const QVector<Interval> list = {{QString(), 0},
                                           {"When logging in", -1},
                                           {"When waking from sleep", -2},
                                           {"Every 5 seconds", 5},
                                           {"Every minute", 60},
                                           {"Every 5 minutes", 300},
                                           {"Every 15 minutes", 900},
                                           {"Every 30 minutes", 1800},
                                           {"Every hour", 3600},
                                           {"Every day", 86400}};
    return list;
}

} // namespace Desktop
