// SPDX-License-Identifier: LGPL-2.1-or-later
// Snow Leopard's files, read at run time from what tools/extract.py took from your own install DVD
// (.local/assets, laid out as on the Mac: System/Library/CoreServices/Dock.app/...). Nothing of
// Apple's is in this repository: when a file is missing the shell draws a plain stand-in.
#pragma once

#include <QFont>
#include <QImage>
#include <QJsonValue>
#include <QPixmap>
#include <QString>

namespace Assets {

// The assets folder: $SNOWLEOPARD_ASSETS, else .local/assets next to the build.
QString root();
bool available();
QString path(const QString &macPath);   // "/System/Library/..." -> its file here
bool exists(const QString &macPath);

// An image at a size (icns picks its best representation, PDFs are rendered at it). An empty
// size keeps the file's own.
QImage image(const QString &macPath, const QSize &size = {});
QPixmap pixmap(const QString &macPath, const QSize &size, qreal dpr);
// Every picture is matched to the screen as ColorSync does on 10.6: one tagged with a profile
// (Generic RGB, gamma 1.8; a PNG gAMA of 0.55) is converted to sRGB, lighter in its middle tones
QImage matched(QImage img);
// (the Dock's art too: Core Animation matches a layer's contents to its context's colour space,
// CA::Render::create_image → create_image_by_rendering, "needs color matching")

// A bundle's icon (CFBundleIconFile of its Info.plist), e.g. "/Applications/Safari.app".
QString bundleIcon(const QString &bundlePath);

// A plist or a .strings file, converted to JSON by extract.py.
QJsonValue plist(const QString &macPath);
// A localized string of a bundle's English.lproj table, or `fallback` without the DVD.
QString tr(const QString &bundlePath, const QString &table, const QString &key, const QString &fallback);

// Lucida Grande (the interface font) at a pixel size, loaded from the DVD's fonts.
QFont uiFont(qreal pixelSize, bool bold = false);
void loadFonts();

QString wallpaper();
// The desktop picture filling a screen of `size` (scaled to cover it, centred), as the Finder draws it.
QImage wallpaperFor(const QSize &size);

} // namespace Assets
