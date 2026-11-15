// SPDX-License-Identifier: LGPL-2.1-or-later
#include "artfile.h"

#include "assets.h"

#include <QFile>
#include <QHash>
#include <QtEndian>
#include <algorithm>

namespace ArtFile {

namespace {

struct File {
    QByteArray bytes;
    QHash<QString, quint8> tokens;     // name -> value
    QHash<QByteArray, quint32> paths;  // key -> record offset (from the data)
    quint32 data = 0;
    int keyLength = 0;

    template <typename T> T read(qsizetype at) const
    {
        // little endian, byte by byte
        quint64 v = 0;
        if (at >= 0 && at + qsizetype(sizeof(T)) <= bytes.size())
            for (int i = int(sizeof(T)) - 1; i >= 0; --i)
                v = (v << 8) | quint8(bytes.at(at + i));
        return T(v);
    }
};

const File &file()
{
    static File f = [] {
        File f;
        QFile in(Assets::path("/System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/Resources/ArtFile.bin"));
        if (!in.open(QIODevice::ReadOnly))
            return f;
        f.bytes = in.readAll();
        const int count = f.read<quint16>(0), tokenCount = f.read<quint16>(4);
        f.keyLength = f.read<quint16>(2);
        const quint32 tokenTable = f.read<quint32>(8), names = f.read<quint32>(12), pathTable = f.read<quint32>(16);
        f.data = f.read<quint32>(20);
        for (int i = 0; i < tokenCount; ++i) {
            const qsizetype entry = tokenTable + 8 * i;
            const qsizetype name = names + f.read<quint32>(entry);
            if (name >= f.bytes.size())
                continue;
            f.tokens.insert(QString::fromLatin1(f.bytes.constData() + name), f.read<quint8>(entry + 4));
        }
        const int entrySize = f.keyLength + 4;
        for (int i = 0; i < count; ++i) {
            const qsizetype entry = pathTable + qsizetype(entrySize) * i;
            if (entry + entrySize > f.bytes.size())
                break;
            f.paths.insert(f.bytes.mid(entry + 4, f.keyLength), f.read<quint32>(entry));
        }
        return f;
    }();
    return f;
}

} // namespace

Element element(const QStringList &path)
{
    static QHash<QString, Element> cache;
    if (path.isEmpty())
        return {};
    QStringList rest = path.mid(1);
    rest.sort();
    const QString cacheKey = path.first() + '/' + rest.join('/');
    auto it = cache.constFind(cacheKey);
    if (it != cache.constEnd())
        return *it;

    Element e;
    const File &f = file();
    QVector<quint8> values;
    for (const QString &name : path) {
        auto t = f.tokens.constFind(name);
        if (t == f.tokens.constEnd())
            return cache[cacheKey] = e;
        values.append(*t);
    }
    // the widget first, then the rest by value (as every key in the path table is)
    std::sort(values.begin() + 1, values.end());
    QByteArray key(f.keyLength, '\0');
    for (int i = 0; i < values.size() && i < f.keyLength; ++i)
        key[i] = char(values[i]);
    auto p = f.paths.constFind(key);
    if (p == f.paths.constEnd())
        return cache[cacheKey] = e;

    const qsizetype record = qsizetype(f.data) + *p;
    e.rows = f.read<quint16>(record);
    e.columns = f.read<quint16>(record + 2);
    for (int i = 0; i < e.rows * e.columns && i < 9; ++i) {
        const int w = f.read<quint16>(record + 0x44 + 2 * i), h = f.read<quint16>(record + 0x56 + 2 * i);
        const qsizetype start = record + f.read<quint32>(record + 0x20 + 4 * i);
        QImage img;
        if (w > 0 && h > 0 && start + qsizetype(w) * h * 4 <= f.bytes.size()) {
            img = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
            const uchar *src = reinterpret_cast<const uchar *>(f.bytes.constData() + start);
            for (int y = 0; y < h; ++y) {
                auto *line = reinterpret_cast<quint32 *>(img.scanLine(y));
                for (int x = 0; x < w; ++x, src += 4)
                    line[x] = qFromBigEndian<quint32>(src);
            }
        }
        e.pieces.append(img);
    }
    return cache[cacheKey] = e;
}

} // namespace ArtFile
