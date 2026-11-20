// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aqua.h"

#include "art.h"
#include "artfile.h"
#include "aquastyle.h"
#include "assets.h"
#include "quartztext.h"
#include "x11.h"

#include <QApplication>
#include <QFontMetricsF>
#include <QMenu>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace Aqua {

void preinit()
{
    // the session's window server: the Wayland one's (WindowServer-wayland says so to its session)
    // or the X11 one's
    qputenv("QT_QPA_PLATFORM", qgetenv("SNOWLEOPARD_WINDOW_SERVER") == "wayland" ? "wayland" : "xcb");
    qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1");
    // the shell's programs belong to the Snow Leopard session, not to the desktop they are started
    // from: without this, KDE's session manager saves them and starts them again at the next login
    qunsetenv("SESSION_MANAGER");
}

// 10.6's menus: 14 px Lucida Grande, 20 px rows, the selection a blue gradient with white text.
static const char *menuStyle = R"(
QMenu {
    background: rgba(255, 255, 255, 243);
    border: 1px solid rgba(0, 0, 0, 40);
    border-top: none;
    padding: 4px 0px 5px 0px;
}
QMenu::item {
    color: black;
    padding: 2px 22px 2px 20px;
    min-height: 16px;
    background: transparent;
}
QMenu::item:selected {
    color: white;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #6a8ef8, stop:1 #2155ef);
}
QMenu::item:disabled { color: rgba(0, 0, 0, 90); }
QMenu::item:disabled:selected { color: rgba(0, 0, 0, 90); background: transparent; }
QMenu::separator { height: 1px; background: rgba(0, 0, 0, 25); margin: 5px 1px 6px 1px; }
QMenu::indicator { width: 14px; left: 4px; }
QMenu::right-arrow { right: 8px; }
)";

void init(QApplication &app)
{
    Assets::loadFonts();
    // CoreUI's controls (aquastyle.h); the menu style sheet only where CoreUI's art is missing
    app.setStyle(new AquaStyle);
    app.setFont(Assets::uiFont(13));
    if (ArtFile::element({"menu", "allround"}).isNull()) {
        QString style = QString::fromLatin1(menuStyle);
        QString selected = Art::sharedFile(Art::MenuItemSelected);
        if (!selected.isEmpty())
            style += QStringLiteral("QMenu::item:selected { background: transparent; border-image: url(%1) 0 0 0 0 stretch stretch; }")
                         .arg(selected);
        app.setStyleSheet(style);
    }
    app.setQuitOnLastWindowClosed(false);
}

void prepareMenu(QMenu *menu)
{
    menu->setFont(Assets::uiFont(14));
    for (QAction *a : menu->actions())
        if (a->menu()) {
            a->menu()->setProperty("aquaMenu", menu->property("aquaMenu"));
            prepareMenu(a->menu());
        }
}

// As Quartz draws it with font smoothing (quartztext.h): on an image (a widget's backing store, a title
// bar's) the LCD coverage mixed into the pixels under the text; elsewhere the dilated outlines in grey.
void drawText(QPainter &p, const QRectF &rect, int flags, const QString &text)
{
    QString line = text;
    if (flags & Qt::TextShowMnemonic)
        line.remove(QLatin1Char('&'));
    if (line.isEmpty())
        return;
    QFont font = p.font();
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(font);
    const qreal width = fm.horizontalAdvance(line), height = fm.ascent() + fm.descent();
    qreal x = rect.left();
    if (flags & Qt::AlignHCenter)
        x = rect.left() + (rect.width() - width) / 2;
    else if (flags & Qt::AlignRight)
        x = rect.right() - width;
    qreal top = rect.top();
    if (flags & Qt::AlignVCenter)
        top = rect.top() + (rect.height() - height) / 2;
    else if (flags & Qt::AlignBottom)
        top = rect.bottom() - height;
    const qreal baseline = std::round(top + fm.ascent());

    QPainterPath glyphs;
    glyphs.addText(QPointF(x, baseline), font, line);
    const qreal size = font.pixelSize() > 0 ? font.pixelSize() : fm.height();
    // the outlines in the device's pixels, the curves flattened sixteen times finer than a pixel
    constexpr qreal fine = 16;
    const QTransform device = p.deviceTransform();
    const bool plain = device.type() <= QTransform::TxTranslate;
    QuartzText::Contours contours;
    for (const QPolygonF &polygon : glyphs.toSubpathPolygons((plain ? device : QTransform()) * QTransform::fromScale(fine, fine))) {
        contours.emplace_back();
        for (const QPointF &point : polygon)
            contours.back().push_back({point.x() / fine, point.y() / fine});
    }
    contours = QuartzText::dilatedForSize(std::move(contours), size);

    const QColor colour = p.pen().color();
    QPaintDevice *target = p.paintEngine() ? p.paintEngine()->paintDevice() : nullptr;
    if (QuartzText::smoothing() && plain && target && target->devType() == QInternal::Image) {
        auto *image = static_cast<QImage *>(target);
        const bool alpha = image->format() == QImage::Format_ARGB32_Premultiplied;
        if ((alpha || image->format() == QImage::Format_RGB32) && image->isDetached()) {
            QRegion clip(image->rect());
            if (p.hasClipping())
                clip &= device.map(p.clipRegion());
            const QRegion system = p.paintEngine()->systemClip();
            if (!system.isEmpty())
                clip &= system;
            const QuartzText::Mask mask = QuartzText::lcdMask(contours);
            for (const QRect &r : clip)
                QuartzText::blend(reinterpret_cast<uint32_t *>(image->bits()), int(image->bytesPerLine()), image->width(),
                                  image->height(), alpha, r.left(), r.top(), r.right() + 1, r.bottom() + 1, mask, colour.redF(),
                                  colour.greenF(), colour.blueF(), colour.alphaF() * p.opacity());
            return;
        }
    }
    QPainterPath filled;
    filled.setFillRule(Qt::WindingFill);
    for (const std::vector<QuartzText::Point> &c : contours) {
        QPolygonF polygon;
        for (const QuartzText::Point &point : c)
            polygon << QPointF(point.x, point.y);
        filled.addPolygon(polygon);
        filled.closeSubpath();
    }
    p.save();
    if (plain)
        p.setWorldTransform(device.inverted(), true);   // the outlines are the device's already
    p.setRenderHint(QPainter::Antialiasing);
    p.fillPath(filled, colour);
    p.restore();
}

} // namespace Aqua
