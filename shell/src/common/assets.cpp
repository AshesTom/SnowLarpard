// SPDX-License-Identifier: LGPL-2.1-or-later
#include "assets.h"

#include <QColorSpace>

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

static QImage icnsColorSpace(QImage img);
static QImage read(const QString &macPath, const QSize &size, bool match);

QImage image(const QString &macPath, const QSize &size) { return read(macPath, size, true); }

static QImage read(const QString &macPath, const QSize &size, bool match)
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
            if (match)
                best = matched(icnsColorSpace(best));
            return best.size() == size ? best : best.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QSize own = reader.size();
        reader.setScaledSize(own.isValid() ? own.scaled(size, Qt::KeepAspectRatio) : size);
    }
    const bool icns = reader.format() == "icns";
    QImage img = reader.read();
    return match ? matched(icns ? icnsColorSpace(img) : img) : img;
}

// The colour space ImageIO gives an icns' raw data (_CGImagePluginInitICNS: kCGColorSpaceGenericRGB):
// the DVD's Generic RGB Profile.icc, else its values (gamma 1.8, D65, Apple's primaries)
static QColorSpace genericRgb()
{
    static const QColorSpace cs = [] {
        QFile f(path("/System/Library/ColorSync/Profiles/Generic RGB Profile.icc"));
        QColorSpace icc = f.open(QIODevice::ReadOnly) ? QColorSpace::fromIccProfile(f.readAll()) : QColorSpace();
        if (icc.isValid())
            return icc;
        return QColorSpace(QPointF(0.3127, 0.3290), QPointF(0.6300, 0.3400), QPointF(0.2950, 0.6050), QPointF(0.1550, 0.0770),
                           QColorSpace::TransferFunction::Gamma, 1.8f);
    }();
    return cs;
}

// An icns' picture without a colour space of its own (its raw is32...it32 data) is Generic RGB
static QImage icnsColorSpace(QImage img)
{
    if (!img.isNull() && !img.colorSpace().isValid())
        img.setColorSpace(genericRgb());
    return img;
}

// Colour matching, as ColorSync does on 10.6: a picture tagged with a profile (most of the Mac's
// art: Generic RGB, gamma 1.8, or a PNG gAMA of 0.55) is converted to the screen's (taken as sRGB),
// which lightens its middle tones; the alpha is kept. Untagged pictures are left as they are.
QImage matched(QImage img)
{
    const QColorSpace cs = img.colorSpace();
    if (img.isNull() || !cs.isValid() || cs == QColorSpace(QColorSpace::SRgb) || cs.colorModel() != QColorSpace::ColorModel::Rgb)
        return img;
    if (img.format() == QImage::Format_Indexed8 || img.format() == QImage::Format_Mono || img.format() == QImage::Format_MonoLSB)
        img = img.convertToFormat(img.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    img.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    return img;
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
