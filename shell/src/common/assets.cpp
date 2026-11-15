// SPDX-License-Identifier: LGPL-2.1-or-later
#include "assets.h"

#include "desktopsettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace Assets {

QString root()
{
    static QString r = [] {
        QString env = qEnvironmentVariable("SNOWLEOPARD_ASSETS");
        if (!env.isEmpty())
            return QDir(env).absolutePath();
        // .local/build/Dock.app/Contents/MacOS/Dock -> .local/assets
        QDir dir(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 6; ++i) {
            if (QFileInfo::exists(dir.filePath("assets/.complete")))
                return dir.filePath("assets");
            if (!dir.cdUp())
                break;
        }
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/snowleopard/assets";
    }();
    return r;
}

bool available() { return QFileInfo::exists(root() + "/.complete"); }

QString path(const QString &macPath) { return root() + (macPath.startsWith('/') ? macPath : '/' + macPath); }

bool exists(const QString &macPath) { return QFileInfo::exists(path(macPath)); }

QImage image(const QString &macPath, const QSize &size)
{
    QImageReader reader(path(macPath));
    if (!size.isEmpty()) {
        // icns: the representation closest above the size; PDFs and the rest: rendered at it.
        if (reader.format() == "icns") {
            QImage best;
            do {
                QImage frame = reader.read();
                if (frame.isNull())
                    break;
                bool better = best.isNull() || (best.width() < size.width() ? frame.width() > best.width()
                                                                            : frame.width() >= size.width() && frame.width() < best.width());
                if (better)
                    best = frame;
            } while (reader.jumpToNextImage());
            if (best.isNull())
                return {};
            return best.size() == size ? best : best.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QSize own = reader.size();
        reader.setScaledSize(own.isValid() ? own.scaled(size, Qt::KeepAspectRatio) : size);
    }
    return reader.read();
}

QPixmap pixmap(const QString &macPath, const QSize &size, qreal dpr)
{
    QImage img = image(macPath, size * dpr);
    QPixmap pm = QPixmap::fromImage(img);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

QJsonValue plist(const QString &macPath)
{
    static QHash<QString, QJsonValue> cache;
    auto it = cache.constFind(macPath);
    if (it != cache.constEnd())
        return *it;
    QFile f(path(macPath) + ".json");
    QJsonValue v;
    if (f.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        v = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
    }
    cache.insert(macPath, v);
    return v;
}

QString bundleIcon(const QString &bundlePath)
{
    QString name = plist(bundlePath + "/Contents/Info.plist")["CFBundleIconFile"].toString();
    if (name.isEmpty())
        return {};
    if (!name.endsWith(".icns"))
        name += ".icns";
    return bundlePath + "/Contents/Resources/" + name;
}

QString tr(const QString &bundlePath, const QString &table, const QString &key, const QString &fallback)
{
    QString s = plist(bundlePath + "/Contents/Resources/English.lproj/" + table + ".strings")[key].toString();
    return s.isEmpty() ? fallback : s;
}

void loadFonts()
{
    for (const char *font : {"/System/Library/Fonts/LucidaGrande.ttc", "/System/Library/Fonts/HelveticaNeue.ttc"})
        if (exists(font))
            QFontDatabase::addApplicationFont(path(font));
}

QFont uiFont(qreal pixelSize, bool bold)
{
    QFont f(QStringLiteral("Lucida Grande"));
    f.setFamilies({QStringLiteral("Lucida Grande"), QStringLiteral("Helvetica Neue"), QStringLiteral("sans-serif")});
    f.setPixelSize(qRound(pixelSize));
    f.setBold(bold);
    f.setHintingPreference(QFont::PreferNoHinting);   // Quartz doesn't hint
    return f;
}

QString wallpaper() { return Desktop::picturePath(Desktop::load()); }

QImage wallpaperFor(const QSize &size) { return Desktop::render(Desktop::load(), size); }

} // namespace Assets
