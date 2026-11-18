// SPDX-License-Identifier: LGPL-2.1-or-later
// /usr/sbin/screencapture, as 10.6's (its x86_64 code, docs/reverse-status.md, section 21): the same
// options (getopt "PdicmwWsoSxfCMET:t:") and the same interaction.
//
//   screencapture [-icMPmwsWxSCUto] [files]
//
// Without -i: the screen (-C with the cursor drawn in), after -T seconds. With -i: a selection or a
// window, chosen with the pointer: Space goes from one to the other (-s only the selection, -w only the
// window, -W the window first), Control sends the picture to the clipboard, Escape gives up.
//   The selection (0x100003576, 0x100002a25): a rectangle 0.53 grey at a quarter, with a white line
//   round it (screencapture's five windows); the cursor is its 32 x 32 cross (a PNG in its __TEXT,
//   extract.py's /usr/sbin/screencapture-crosshair.png) in a 64 x 40 picture with two numbers beside
//   it, the pointer's place and then the selection's size, Lucida Grande 9 (black over white at 0.66,
//   a point lower and to the right), at the corner away from the selection and from the screen's edges.
//   The window (0x1000017fb): the window under the pointer covered with the highlight colour
//   (AppleHighlightColor, 0.5294 0.7255 0.9059 by default) at half; the cursor the camera (CoreCursor
//   9, or 10 for the clipboard: HIServices' cursors). A click takes it with its shadow (-o without),
//   as the window server draws it (CGSHWCaptureWindowList: WindowServer's _SNOWLEOPARD_CAPTURE_WINDOW).
//   -G (the shell's own, for Grab.app): Grab's look (Grab 1.5's -_variableSelection:, -placeRectSides:,
//   -app:isTrackingMouse:at:for:): the selection a red outline 2 wide, a tool tip by the pointer with
//   its place ("x, y", y from the bottom as Cocoa counts) then the selection's size (negative leftwards
//   and upwards), the cross cursor; the window mode Grab's CameraPointer, no highlight, no shadow.
// The picture goes to the files (the type -t: png by default, jpg, tiff, gif, bmp, pdf), else (or with
// -c) to the clipboard; -P opens it in Preview. Grab.aif plays (-x: not).
#include "aqua.h"
#include "apps.h"
#include "assets.h"
#include "x11.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QtEndian>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLocale>
#include <QMimeData>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <getopt.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

namespace {

struct Options {
    bool interactive = false, clipboard = false, cursor = false, errors = false, sounds = true;
    bool selectionOnly = false, windowOnly = false, windowFirst = false, shadow = true;
    bool preview = false;
    bool grabLook = false;
    int delay = 0;
    QString type = QStringLiteral("png");
    QStringList files;
} opt;

// ---- the screen

QImage screenImage()
{
    xcb_connection_t *c = X11::conn();
    const xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    QImage image;
    // in bands: a reply has a longest length
    const int w = s->width_in_pixels, h = s->height_in_pixels, rows = qMax(1, (1 << 22) / qMax(1, w * 4));
    image = QImage(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; y += rows) {
        const int n = qMin(rows, h - y);
        auto *r = xcb_get_image_reply(c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, s->root, 0, int16_t(y), uint16_t(w), uint16_t(n), ~0u),
                                      nullptr);
        if (!r)
            return {};
        if (r->depth >= 24 && xcb_get_image_data_length(r) >= w * n * 4)
            for (int row = 0; row < n; ++row)
                memcpy(image.scanLine(y + row), xcb_get_image_data(r) + size_t(row) * size_t(w) * 4, size_t(w) * 4);
        free(r);
    }
    return image;
}

// -C: the cursor as it is, drawn where it is (XFixes)
void drawCursor(QImage &image, QPoint origin = {})
{
    xcb_connection_t *c = X11::conn();
    free(xcb_xfixes_query_version_reply(c, xcb_xfixes_query_version(c, 4, 0), nullptr));
    auto *r = xcb_xfixes_get_cursor_image_reply(c, xcb_xfixes_get_cursor_image(c), nullptr);
    if (!r)
        return;
    const uint32_t *px = xcb_xfixes_get_cursor_image_cursor_image(r);
    QImage cur(r->width, r->height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < r->height; ++y)
        memcpy(cur.scanLine(y), px + size_t(y) * r->width, size_t(r->width) * 4);
    QPainter p(&image);
    p.drawImage(QPoint(r->x - r->xhot, r->y - r->yhot) - origin, cur);
    free(r);
}

// ---- the result

void playGrab()
{
    if (!opt.sounds)
        return;
    const QString sound = Assets::path("/System/Library/Components/CoreAudio.component/Contents/Resources/SystemSounds/system/Grab.aif");
    if (!QFileInfo::exists(sound))
        return;
    for (const char *player : {"pw-play", "paplay", "aplay"})
        if (!QStandardPaths::findExecutable(player).isEmpty()) {
            QProcess::startDetached(player, {sound});
            return;
        }
}

bool writeFile(const QImage &image, const QString &path)
{
    const QString t = opt.type.toLower();
    if (t == "pdf") {
        QPdfWriter pdf(path);
        pdf.setResolution(72);
        pdf.setPageSize(QPageSize(QSizeF(image.width(), image.height()), QPageSize::Point));
        pdf.setPageMargins({});
        QPainter p(&pdf);
        p.drawImage(QRect(0, 0, image.width(), image.height()), image);
        return true;
    }
    QByteArray format = t.toLatin1();
    if (format == "jpeg")
        format = "jpg";
    if (format == "tif")
        format = "tiff";
    QImageWriter w(path, format);
    if (format == "jpg")
        w.setQuality(90);
    // a JPEG has no alpha: a window's shadow over white, as ImageIO flattens it
    QImage out = image;
    if (format == "jpg" || format == "bmp") {
        out = QImage(image.size(), QImage::Format_RGB32);
        out.fill(Qt::white);
        QPainter(&out).drawImage(0, 0, image);
    }
    if (!w.write(out)) {
        std::fprintf(stderr, "screencapture: %s\n", qPrintable(w.errorString()));
        return false;
    }
    return true;
}

// Done: the files, or the clipboard (kept until another program takes it, as a pasteboard is)
int finish(QImage image, bool toClipboard)
{
    if (image.isNull())
        return 1;
    playGrab();
    if (toClipboard || opt.files.isEmpty()) {
        QClipboard *cb = QGuiApplication::clipboard();
        cb->setImage(image);
        QObject::connect(cb, &QClipboard::dataChanged, qApp, [cb] {
            if (!cb->ownsClipboard())
                qApp->quit();
        });
        return qApp->exec();
    }
    bool ok = writeFile(image, opt.files.first());
    if (ok && opt.preview)
        Apps::launch(Apps::forMacBundle("/Applications/Preview.app"), {opt.files.first()});
    return ok ? 0 : 1;
}

// ---- interactive

// the window under a point: the root's children top down (WindowServer's frames, the Dock, the
// menu bar, the desktop), mapped, not the overlay
struct Picked {
    xcb_window_t window = 0;
    QRect rect;
};

Picked windowAt(QPoint p, xcb_window_t skip)
{
    xcb_connection_t *c = X11::conn();
    Picked out;
    auto *tree = xcb_query_tree_reply(c, xcb_query_tree(c, X11::root()), nullptr);
    if (!tree)
        return out;
    const xcb_window_t *kids = xcb_query_tree_children(tree);
    for (int i = xcb_query_tree_children_length(tree) - 1; i >= 0; --i) {
        const xcb_window_t w = kids[i];
        if (w == skip)
            continue;
        auto *a = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, w), nullptr);
        const bool shown = a && a->map_state == XCB_MAP_STATE_VIEWABLE && a->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT &&
                           !a->override_redirect;
        free(a);
        if (!shown)
            continue;
        auto *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), nullptr);
        if (!g)
            continue;
        const QRect r(g->x, g->y, g->width, g->height);
        free(g);
        if (r.contains(p)) {
            out = {w, r};
            break;
        }
    }
    free(tree);
    return out;
}

// CGSHWCaptureWindowList: WindowServer's picture of a window it manages, with its shadow
QImage captureWindow(xcb_window_t w, xcb_window_t requestor)
{
    xcb_connection_t *c = X11::conn();
    const xcb_atom_t done = X11::atom("_SNOWLEOPARD_WINDOW_CAPTURE_DONE"), prop = X11::atom("_SNOWLEOPARD_WINDOW_CAPTURE");
    xcb_delete_property(c, requestor, done);
    xcb_delete_property(c, requestor, prop);
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = w;
    ev.type = X11::atom("_SNOWLEOPARD_CAPTURE_WINDOW");
    ev.data.data32[0] = opt.shadow ? 1 : 0;
    ev.data.data32[1] = requestor;
    xcb_send_event(c, 0, X11::root(), XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&ev));
    xcb_flush(c);
    QElapsedTimer t;
    t.start();
    while (X11::cardinals(requestor, done, XCB_ATOM_CARDINAL).isEmpty()) {
        if (t.elapsed() > 2000 || !X11::hasWindowManager())
            return {};
        QThread::msleep(10);
    }
    const QVector<uint32_t> data = X11::cardinals(requestor, prop, XCB_ATOM_CARDINAL);
    xcb_delete_property(c, requestor, done);
    xcb_delete_property(c, requestor, prop);
    if (data.size() < 4)
        return {};
    const int iw = int(data[0]), ih = int(data[1]);
    if (iw <= 0 || ih <= 0 || data.size() < 4 + iw * ih)
        return {};
    QImage image(iw, ih, QImage::Format_ARGB32);
    for (int y = 0; y < ih; ++y)
        memcpy(image.scanLine(y), data.constData() + 4 + size_t(y) * size_t(iw), size_t(iw) * 4);
    return image;
}

QColor highlightColor()
{
    // AppleHighlightColor ("r g b"), the Appearance pane's; 10.6's blue otherwise
    QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/appearance.conf",
                QSettings::IniFormat);
    const QStringList v = s.value("AppleHighlightColor").toString().split(' ', Qt::SkipEmptyParts);
    if (v.size() >= 3)
        return QColor::fromRgbF(float(v[0].toDouble()), float(v[1].toDouble()), float(v[2].toDouble()), 0.5f);
    return QColor::fromRgbF(0.5294f, 0.7255f, 0.9059f, 0.5f);
}

// An Xcursor file's largest image (the theme's, extract.py's: snowleopard-camera...)
QCursor themeCursor(const QString &name, Qt::CursorShape fallback)
{
    QFile f(Assets::path("/share/icons/SnowLeopard/cursors/" + name));
    if (!f.open(QIODevice::ReadOnly))
        return QCursor(fallback);
    const QByteArray d = f.readAll();
    auto u32 = [&](qsizetype at) { return at + 4 <= d.size() ? qFromLittleEndian<quint32>(d.constData() + at) : 0u; };
    if (!d.startsWith("Xcur"))
        return QCursor(fallback);
    const quint32 n = u32(12);
    for (quint32 i = 0; i < n; ++i) {
        const quint32 pos = u32(16 + 12 * i + 8);
        if (u32(16 + 12 * i) != 0xFFFD0002)
            continue;
        const int w = int(u32(pos + 16)), h = int(u32(pos + 20)), hx = int(u32(pos + 24)), hy = int(u32(pos + 28));
        if (w <= 0 || h <= 0 || pos + 36 + qsizetype(w) * h * 4 > d.size())
            break;
        QImage img(reinterpret_cast<const uchar *>(d.constData() + pos + 36), w, h, QImage::Format_ARGB32_Premultiplied);
        return QCursor(QPixmap::fromImage(img.copy()), hx, hy);
    }
    return QCursor(fallback);
}

class Overlay : public QWidget {
public:
    explicit Overlay(const QImage &screen) : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint), m_screen(screen)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMouseTracking(true);
        setGeometry(QRect(QPoint(0, 0), screen.size()));
        m_cross = Assets::image("/usr/sbin/screencapture-crosshair.png");
        if (m_cross.isNull()) {   // without the DVD's: a cross drawn alike
            m_cross = QImage(32, 32, QImage::Format_ARGB32_Premultiplied);
            m_cross.fill(Qt::transparent);
            QPainter p(&m_cross);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(QColor(200, 200, 200, 160));
            p.setPen(QColor(150, 150, 150, 200));
            p.drawEllipse(QRectF(10.5, 10.5, 10, 10));
            p.fillRect(QRect(15, 4, 1, 23), QColor(60, 60, 60));
            p.fillRect(QRect(4, 15, 23, 1), QColor(60, 60, 60));
        }
        m_camera = themeCursor("snowleopard-camera", Qt::PointingHandCursor);
        m_cameraClipboard = themeCursor("snowleopard-camera-window", Qt::PointingHandCursor);
        if (opt.grabLook) {   // Grab's -selectedCursor: CameraPointer, its hot spot in its middle
            const QImage pointer = Assets::image("/Applications/Utilities/Grab.app/Contents/Resources/CameraPointer.tiff");
            if (!pointer.isNull())
                m_camera = m_cameraClipboard = QCursor(QPixmap::fromImage(pointer), pointer.width() / 2, pointer.height() / 2);
            m_cross = {};
        }
        m_windowMode = opt.windowOnly || opt.windowFirst;
        m_clipboard = opt.clipboard || opt.files.isEmpty() || (QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier);
        m_highlight = highlightColor();
    }

    void start()
    {
        showFullScreen();
        grabKeyboard();
        grabMouse();
        activateWindow();
        hover(QCursor::pos());
    }

    QImage result;
    bool toClipboard = false;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.drawImage(0, 0, m_screen);
        if (opt.grabLook) {
            if (!m_windowMode && m_dragging) {   // the rectangle less itself inset by 2: red (autofill 1, 0, 0)
                const QRect r = selection();
                if (r.width() >= 2 && r.height() >= 2) {
                    const QRegion ring = QRegion(r) - QRegion(r.adjusted(2, 2, -2, -2));
                    for (const QRect &part : ring)
                        p.fillRect(part, Qt::red);
                }
            }
            if (!m_windowMode)
                paintToolTip(p);
            return;
        }
        if (!m_windowMode && m_dragging) {
            const QRect r = selection();
            p.fillRect(r, QColor::fromRgbF(0.53f, 0.53f, 0.53f, 0.25f));
            p.setPen(Qt::white);
            p.drawRect(r.adjusted(-1, -1, 0, 0));
        } else if (m_windowMode && !m_picked.rect.isEmpty()) {
            p.fillRect(m_picked.rect, m_highlight);
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        if (m_windowMode) {
            takeWindow();
            return;
        }
        m_dragging = true;
        m_anchor = m_point = e->globalPosition().toPoint();
        update();
    }

    void mouseMoveEvent(QMouseEvent *e) override { hover(e->globalPosition().toPoint()); }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || m_windowMode || !m_dragging)
            return;
        m_dragging = false;
        const QRect r = selection();
        if (r.width() < 1 || r.height() < 1) {   // a click: nothing selected
            update();
            return;
        }
        result = m_screen.copy(r);
        if (opt.cursor)
            drawCursor(result, r.topLeft());
        done();
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            result = {};
            done();
        } else if (e->key() == Qt::Key_Control) {
            m_clipboard = !m_clipboard || opt.clipboard || opt.files.isEmpty();
            hover(QCursor::pos());
        } else if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
            if (m_dragging) {   // held while dragging: the selection moves
                m_moving = true;
                m_moveFrom = QCursor::pos();
            } else if (!opt.selectionOnly && !opt.windowOnly) {
                m_windowMode = !m_windowMode;
                hover(QCursor::pos());
                update();
            }
        }
    }

    void keyReleaseEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Space && !e->isAutoRepeat())
            m_moving = false;
        else if (e->key() == Qt::Key_Control && !opt.clipboard && !opt.files.isEmpty()) {
            m_clipboard = false;
            hover(QCursor::pos());
        }
    }

private:
    // Grab's coordinates window: NSToolTip's colours, the system font at 10, "%ld, %ld" 2 in; its top
    // right by the pointer (2 to its left), kept on the screen
    void paintToolTip(QPainter &p)
    {
        const QPoint at = m_point;
        long a, b;
        if (m_dragging) {
            const QRect r = selection();
            a = at.x() < m_anchor.x() ? -r.width() : r.width();
            b = at.y() < m_anchor.y() ? -r.height() : r.height();
        } else {
            a = at.x() + 1;
            b = height() - at.y();
        }
        const QString text = QStringLiteral("%1, %2").arg(a).arg(b);
        QFont f("Lucida Grande");
        f.setPixelSize(10);
        const QFontMetrics fm(f);
        const int w = fm.horizontalAdvance(text) + 4, h = 14;
        const QRect box(qMax(0, at.x() - w - 2), qMin(height() - h, at.y()), w, h);
        p.fillRect(box, QColor(255, 255, 199));
        p.setPen(Qt::black);
        p.drawRect(box.adjusted(0, 0, -1, -1));
        p.setFont(f);
        Aqua::drawText(p, QRectF(box.x() + 2, box.y(), w, h - 1), Qt::AlignLeft | Qt::AlignVCenter, text);
    }

    // from the press to the pointer, the pointer's pixel not in it (a drag of 150 is 150 wide)
    QRect selection() const
    {
        return QRect(QPoint(qMin(m_anchor.x(), m_point.x()), qMin(m_anchor.y(), m_point.y())),
                     QSize(qAbs(m_point.x() - m_anchor.x()), qAbs(m_point.y() - m_anchor.y())));
    }

    void hover(QPoint at)
    {
        if (m_windowMode) {
            m_picked = windowAt(at, xcb_window_t(winId()));
            setCursor(m_clipboard ? m_cameraClipboard : m_camera);
            update();
            return;
        }
        if (m_dragging) {
            if (m_moving) {
                m_anchor += at - m_moveFrom;
                m_moveFrom = at;
            }
        }
        if (m_dragging || opt.grabLook) {
            m_point = at;
            update();
        }
        setCursor(opt.grabLook ? QCursor(Qt::CrossCursor) : crossCursor(at));
    }

    // 0x100003576: the corner the numbers go to, away from the selection and the screen's edges
    QCursor crossCursor(QPoint at)
    {
        const QPoint from = m_dragging ? m_anchor : QPoint(0, 0);
        bool left = at.x() < from.x(), above = at.y() < from.y();
        const QRect screen = rect();
        if (at.x() - 32 <= screen.left())
            left = false;
        else if (at.x() + 32 >= screen.right())
            left = true;
        if (at.y() - 32 <= screen.top())
            above = false;
        else if (at.y() + 32 >= screen.bottom())
            above = true;
        // the numbers: where the pointer is, then the selection's size
        const QSize size = selection().size();
        const int a = m_dragging ? size.width() : at.x(), b = m_dragging ? size.height() : at.y();
        // 0x100002a25's layout of the 64 x 40 picture: the cross at a corner, the numbers beside it
        // (baselines at 29 and 19 from the bottom over the cross, 14 and 5 under it; from 23, or ending at 40)
        QImage img(64, 40, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        const QPoint cross(left ? 32 : 0, above ? 8 : 0);
        p.drawImage(cross, m_cross);
        QFont f("Lucida Grande");
        f.setPixelSize(9);
        p.setFont(f);
        const QFontMetrics fm(f);
        const QString s1 = QLocale::system().toString(a), s2 = QLocale::system().toString(b);
        const int y1 = 40 - (above ? 29 : 14), y2 = 40 - (above ? 19 : 5);
        auto x = [&](const QString &s) { return left ? 40 - fm.horizontalAdvance(s) : 23; };
        auto line = [&](const QString &s, int bx, int by, const QColor &c) {
            p.setPen(c);
            Aqua::drawText(p, QRectF(bx, by - fm.ascent(), 64, fm.height()), Qt::AlignLeft | Qt::AlignTop, s);
        };
        line(s1, x(s1), y1, QColor::fromRgbF(1, 1, 1, 0.66f));
        line(s2, x(s2), y2, QColor::fromRgbF(1, 1, 1, 0.66f));
        line(s1, x(s1) - 1, y1 - 1, Qt::black);
        line(s2, x(s2) - 1, y2 - 1, Qt::black);
        p.end();
        return QCursor(QPixmap::fromImage(img), cross.x() + 15, cross.y() + 15);
    }

    void takeWindow()
    {
        if (m_picked.window == 0)
            return;
        hide();
        QApplication::processEvents();
        result = captureWindow(m_picked.window, xcb_window_t(winId()));
        if (result.isNull())   // not one of WindowServer's (the Dock, the menu bar): as it is on the screen
            result = m_screen.copy(m_picked.rect);
        done();
    }

    void done()
    {
        toClipboard = m_clipboard;
        releaseKeyboard();
        releaseMouse();
        hide();
        QApplication::exit(0);
    }

    QImage m_screen, m_cross;
    QCursor m_camera, m_cameraClipboard;
    QColor m_highlight;
    bool m_windowMode = false, m_clipboard = false, m_dragging = false, m_moving = false;
    QPoint m_anchor, m_point, m_moveFrom;
    Picked m_picked;
};

void usage()
{
    std::fputs("usage: screencapture [-icMPmwsWxSCUto] [files]\n"
               "  -c         force screen capture to go to the clipboard\n"
               "  -C         capture the cursor as well as the screen. only in non-interactive modes\n"
               "  -d         display errors to the user graphically\n"
               "  -i         capture screen interactively, by selection or window\n"
               "               control key - causes screen shot to go to clipboard\n"
               "               space key   - toggle between mouse selection and\n"
               "                             window selection modes\n"
               "               escape key  - cancels interactive screen shot\n"
               "  -m         only capture the main monitor, undefined if -i is set\n"
               "  -M         screen capture output will go to a new Mail message\n"
               "  -o         in window capture mode, do not capture the shadow of the window\n"
               "  -P         screen capture output will open in Preview\n"
               "  -s         only allow mouse selection mode\n"
               "  -S         in window capture mode, capture the screen not the window\n"
               "  -t<format> image format to create, default is png (other options include pdf, jpg, tiff and other formats)\n"
               "  -T<seconds> Take the picture after a delay of <seconds>, default is 5\n"
               "  -w         only allow window selection mode\n"
               "  -W         start interaction in window selection mode\n"
               "  -x         do not play sounds\n"
               "  files   where to save the screen capture, 1 file per screen\n",
               stdout);
}

} // namespace

int main(int argc, char **argv)
{
    // the options first: QApplication would take some of them
    opterr = 0;
    for (int ch; (ch = getopt(argc, argv, "PdicmwWsoSxfCGMET:t:")) != -1;) {
        switch (ch) {
        case 'C': opt.cursor = true; break;
        case 'G': opt.grabLook = true; opt.shadow = false; break;
        case 'P': opt.preview = true; break;
        case 'M': break;   // a new Mail message: the file only, for now
        case 'E': case 'S': case 'f': case 'm': break;
        case 'T': opt.delay = optarg ? atoi(optarg) : 5; break;
        case 'c': opt.clipboard = true; break;
        case 'd': opt.errors = true; break;
        case 'i': opt.interactive = true; break;
        case 'o': opt.shadow = false; break;
        case 's': opt.selectionOnly = true; opt.interactive = true; break;
        case 'w': opt.windowOnly = true; opt.windowFirst = true; opt.interactive = true; break;
        case 'W': opt.windowFirst = true; opt.interactive = true; break;
        case 'x': opt.sounds = false; break;
        case 't': opt.type = QString::fromLocal8Bit(optarg); break;
        default:
            std::fprintf(stderr, "screencapture: illegal option -- %c\n", optopt);
            usage();
            return 1;
        }
    }
    for (int i = optind; i < argc; ++i)
        if (*argv[i])
            opt.files << QString::fromLocal8Bit(argv[i]);
    if (!opt.clipboard && opt.files.isEmpty() && !opt.interactive) {
        std::fputs("screencapture: no file specified\n", stderr);
        return 255;
    }
    Aqua::preinit();
    int qargc = 1;
    QApplication app(qargc, argv);
    app.setApplicationName("screencapture");
    app.setQuitOnLastWindowClosed(false);
    Aqua::init(app);
    if (!X11::conn()) {
        std::fputs("screencapture: failed to make a connection to the windowserver\n", stderr);
        return 1;
    }
    if (!opt.interactive) {
        if (opt.delay > 0)
            QThread::sleep(unsigned(opt.delay));
        QImage image = screenImage();
        if (opt.cursor)
            drawCursor(image);
        return finish(image, opt.clipboard);
    }
    Overlay overlay(screenImage());
    overlay.start();
    app.exec();
    if (overlay.result.isNull())
        return 0;
    return finish(overlay.result, overlay.toClipboard);
}
