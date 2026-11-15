// SPDX-License-Identifier: LGPL-2.1-or-later
#include "art.h"

#include "assets.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QHash>
#include <QtEndian>
#include <cstring>

namespace Art {

static const QByteArray &artFile()
{
    static QByteArray bytes = [] {
        QFile f(Assets::path("/System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/Resources/SArtFile.bin"));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    return bytes;
}

QImage shared(int id, int representation)
{
    static QHash<qint64, QImage> cache;
    qint64 key = (qint64(id) << 8) | representation;
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;

    QImage img;
    const QByteArray &b = artFile();
    auto read = [&](qsizetype at, auto zero) {
        decltype(zero) v = 0;
        if (at >= 0 && at + qsizetype(sizeof v) <= b.size())
            memcpy(&v, b.constData() + at, sizeof v);
        return qFromLittleEndian(v);
    };
    auto u16 = [&](qsizetype at) { return read(at, quint16()); };
    auto u32 = [&](qsizetype at) { return read(at, quint32()); };
    const int count = u16(2);
    const quint32 data = u32(4);
    const int index = id - 20000;
    if (index > 0 && index < count) {
        const qsizetype entry = u32(8 + 4 * index);
        const qsizetype rep = entry + 12 * representation;
        const int w = u16(rep + 0xC), h = u16(rep + 0xE);
        const quint32 length = u32(rep + 0x10), offset = u32(rep + 0x14);
        const qsizetype start = qsizetype(data) + offset;
        if (entry && w > 0 && h > 0 && length == quint32(w) * h * 4 && start + length <= b.size()) {
            img = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
            const uchar *src = reinterpret_cast<const uchar *>(b.constData() + start);
            for (int y = 0; y < h; ++y) {
                auto *line = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x, src += 4)   // A R G B in memory: one big-endian 0xAARRGGBB
                    line[x] = qFromBigEndian<quint32>(src);
            }
        }
    }
    cache.insert(key, img);
    return img;
}

QString sharedFile(int id)
{
    QImage img = shared(id);
    if (img.isNull())
        return {};
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/snowleopard/art";
    QString file = dir + QStringLiteral("/%1.png").arg(id);
    if (!QFile::exists(file)) {
        QDir().mkpath(dir);
        img.save(file);
    }
    return file;
}

} // namespace Art
