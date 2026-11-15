// SPDX-License-Identifier: LGPL-2.1-or-later
#include "extras.h"

#include "aqua.h"
#include "apps.h"
#include "assets.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

static const QString menuExtras = QStringLiteral("/System/Library/CoreServices/Menu Extras/");

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

static void drawCentered(QPainter &p, const QRect &r, const QImage &img)
{
    QSizeF s = img.deviceIndependentSize();
    p.drawImage(QPointF(r.x() + (r.width() - s.width()) / 2, r.y() + (r.height() - s.height()) / 2), img);
}

// ---- the clock

ClockExtra::ClockExtra(QObject *parent) : MenuExtra(parent)
{
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this, timer] {
        emit changed();
        timer->start(60000 - QTime::currentTime().second() * 1000 - QTime::currentTime().msec() + 20);
    });
    timer->start(0);
}

QString ClockExtra::text() const
{
    // The day's short name and the time, as 10.6 shows them by default ("Wed 19:30")
    QLocale l = QLocale::system();
    QDateTime now = QDateTime::currentDateTime();
    return l.dayName(now.date().dayOfWeek(), QLocale::ShortFormat) + "  " + l.toString(now.time(), QLocale::ShortFormat);
}

int ClockExtra::width(const QFontMetrics &fm) const { return fm.horizontalAdvance(text()) + 18; }

void ClockExtra::paint(QPainter &p, const QRect &r, bool highlighted)
{
    p.setPen(highlighted ? Qt::white : Qt::black);
    Aqua::drawText(p, r, Qt::AlignCenter, text());
}

QMenu *ClockExtra::menu()
{
    if (!m_menu) {
        m_menu = new QMenu;
        connect(m_menu, &QMenu::aboutToShow, this, [this] {
            m_menu->clear();
            QAction *date = m_menu->addAction(QLocale::system().toString(QDate::currentDate(), QLocale::LongFormat));
            date->setEnabled(false);
            m_menu->addSeparator();
            m_menu->addAction("Open Date & Time…", [] {
                for (const char *id : {"kcm_clock", "org.gnome.Settings"})
                    if (Apps::launch(Apps::byId(id)))
                        return;
                QProcess::startDetached("systemsettings", {"kcm_clock"});
            });
            Aqua::prepareMenu(m_menu);
        });
    }
    return m_menu;
}

// ---- the volume: pactl (PipeWire or PulseAudio)

VolumeExtra::VolumeExtra(QObject *parent) : MenuExtra(parent)
{
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &VolumeExtra::refresh);
    timer->start(3000);
    refresh();
}

void VolumeExtra::refresh()
{
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc](int code) {
        QString out = QString::fromUtf8(proc->readAllStandardOutput());
        proc->deleteLater();
        bool available = code == 0 && !out.isEmpty();
        auto m = QRegularExpression("(\\d+)%").match(out);
        int level = m.hasMatch() ? m.captured(1).toInt() : m_level;
        bool muted = out.contains("Mute: yes");
        if (available != m_available || level != m_level || muted != m_muted) {
            m_available = available, m_level = level, m_muted = muted;
            emit changed();
        }
    });
    proc->start("sh", {"-c", "pactl get-sink-volume @DEFAULT_SINK@ && pactl get-sink-mute @DEFAULT_SINK@"});
}

void VolumeExtra::set(int percent)
{
    m_level = percent;
    m_muted = percent == 0;
    QProcess::startDetached("pactl", {"set-sink-volume", "@DEFAULT_SINK@", QString::number(percent) + "%"});
    QProcess::startDetached("pactl", {"set-sink-mute", "@DEFAULT_SINK@", m_muted ? "1" : "0"});
    emit changed();
}

void VolumeExtra::paint(QPainter &p, const QRect &r, bool highlighted)
{
    // Volume1 (no waves) to Volume4 (three)
    int n = m_muted || m_level == 0 ? 1 : m_level < 34 ? 2 : m_level < 67 ? 3 : 4;
    QImage img = menuIcon(menuExtras + QString("Volume.menu/Contents/Resources/Volume%1.pdf").arg(n), 16,
                          p.device()->devicePixelRatioF(), highlighted);
    if (!img.isNull()) {
        drawCentered(p, r, img);
        return;
    }
    p.setPen(highlighted ? Qt::white : Qt::black);
    Aqua::drawText(p, r, Qt::AlignCenter, QString::fromUtf8("♪"));
}

QWidget *VolumeExtra::popup()
{
    if (!m_popup) {
        // A vertical slider in a menu, as 10.6's volume menu has
        auto *menu = new QMenu;
        auto *slider = new QSlider(Qt::Vertical);
        slider->setRange(0, 100);
        slider->setFixedSize(22, 110);
        auto *box = new QWidget;
        auto *lay = new QVBoxLayout(box);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->addWidget(slider, 0, Qt::AlignHCenter);
        auto *action = new QWidgetAction(menu);
        action->setDefaultWidget(box);
        menu->addAction(action);
        connect(menu, &QMenu::aboutToShow, slider, [this, slider] {
            QSignalBlocker block(slider);
            slider->setValue(m_muted ? 0 : m_level);
        });
        connect(slider, &QSlider::valueChanged, this, &VolumeExtra::set);
        Aqua::prepareMenu(menu);
        m_popup = menu;
    }
    return m_popup;
}

// ---- the battery: /sys/class/power_supply

BatteryExtra::BatteryExtra(QObject *parent) : MenuExtra(parent)
{
    for (const QString &d : QDir("/sys/class/power_supply").entryList({"BAT*"}, QDir::Dirs | QDir::System))
        m_battery = "/sys/class/power_supply/" + d;
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &BatteryExtra::refresh);
    timer->start(30000);
    refresh();
}

static QString readSys(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

void BatteryExtra::refresh()
{
    if (m_battery.isEmpty())
        return;
    m_percent = readSys(m_battery + "/capacity").toInt();
    m_status = readSys(m_battery + "/status");
    emit changed();
}

void BatteryExtra::paint(QPainter &p, const QRect &r, bool highlighted)
{
    qreal dpr = p.device()->devicePixelRatioF();
    bool charging = m_status == "Charging", full = m_status == "Full";
    QString base = menuExtras + "Battery.menu/Contents/Resources/";
    QImage img = menuIcon(base + (full ? "BatteryCharged.pdf" : charging ? "BatteryCharging.pdf" : "BatteryEmpty.pdf"), 16, dpr,
                          highlighted);
    QSizeF s = img.isNull() ? QSizeF(26, 12) : img.deviceIndependentSize();
    QRectF box(r.x() + (r.width() - s.width()) / 2, r.y() + (r.height() - s.height()) / 2, s.width(), s.height());
    if (img.isNull()) {
        p.setPen(highlighted ? Qt::white : Qt::black);
        p.drawRect(box.adjusted(0.5, 0.5, -3.5, -0.5));
    } else {
        p.drawImage(box.topLeft(), img);
    }
    if (!full && !charging) {
        // The charge left, inside the empty battery's body
        QRectF body = box.adjusted(2.5, 3, -4.5, -3);
        body.setWidth(body.width() * qBound(0, m_percent, 100) / 100.0);
        p.fillRect(body, m_percent <= 10 ? QColor(200, 0, 0) : highlighted ? Qt::white : Qt::black);
    }
}

QMenu *BatteryExtra::menu()
{
    if (!m_menu) {
        m_menu = new QMenu;
        connect(m_menu, &QMenu::aboutToShow, this, [this] {
            refresh();
            m_menu->clear();
            QString state = m_status == "Charging" ? "Charging" : m_status == "Full" ? "Battery Is Charged" : "Power Source: Battery";
            m_menu->addAction(QString("%1%  —  %2").arg(m_percent).arg(state))->setEnabled(false);
            Aqua::prepareMenu(m_menu);
        });
    }
    return m_menu;
}

// ---- Spotlight: for now it finds applications (the files' index is for later)

namespace {
class SpotlightPopup : public QFrame {
public:
    SpotlightPopup() : QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint)
    {
        setStyleSheet("SpotlightPopup { background: rgba(255,255,255,245); border: 1px solid rgba(0,0,0,50); }"
                      "QLineEdit { border: 1px solid #8a8a8a; border-radius: 10px; padding: 2px 8px; background: white; }"
                      "QListWidget { border: none; background: transparent; }"
                      "QListWidget::item:selected { color: white; background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                      "stop:0 #6a8ef8, stop:1 #2155ef); }");
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(10, 8, 10, 8);
        m_field = new QLineEdit;
        m_field->setPlaceholderText("Spotlight");
        m_field->setFont(Assets::uiFont(13));
        m_list = new QListWidget;
        m_list->setIconSize(QSize(16, 16));
        m_list->setFont(Assets::uiFont(13));
        m_list->hide();
        lay->addWidget(m_field);
        lay->addWidget(m_list);
        setFixedWidth(320);
        connect(m_field, &QLineEdit::textChanged, this, &SpotlightPopup::search);
        connect(m_field, &QLineEdit::returnPressed, this, [this] { open(m_list->currentItem()); });
        connect(m_list, &QListWidget::itemActivated, this, &SpotlightPopup::open);
        m_field->installEventFilter(this);
    }
    void showEvent(QShowEvent *e) override
    {
        QFrame::showEvent(e);
        m_field->clear();
        m_field->setFocus();
    }
protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == m_field && e->type() == QEvent::KeyPress) {
            auto *k = static_cast<QKeyEvent *>(e);
            if ((k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) && m_list->count()) {
                int row = qBound(0, m_list->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1), m_list->count() - 1);
                m_list->setCurrentRow(row);
                return true;
            }
        }
        return QFrame::eventFilter(o, e);
    }
private:
    void search(const QString &text)
    {
        m_list->clear();
        if (!text.trimmed().isEmpty()) {
            for (const App &a : Apps::all()) {
                App shown = Apps::forWindow(a.wmClass, a.id, 0);
                if (!a.name.contains(text, Qt::CaseInsensitive) && !shown.name.contains(text, Qt::CaseInsensitive))
                    continue;
                auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(Apps::icon(shown, 32))), shown.name);
                item->setData(Qt::UserRole, a.id);
                m_list->addItem(item);
                if (m_list->count() == 12)
                    break;
            }
        }
        m_list->setVisible(m_list->count());
        m_list->setCurrentRow(0);
        m_list->setFixedHeight(qMin(m_list->count(), 12) * 20 + 4);
        adjustSize();
    }
    void open(QListWidgetItem *item)
    {
        if (item)
            Apps::launch(Apps::byId(item->data(Qt::UserRole).toString()));
        hide();
    }
    QLineEdit *m_field;
    QListWidget *m_list;
};
} // namespace

SpotlightExtra::SpotlightExtra(QObject *parent) : MenuExtra(parent) {}

void SpotlightExtra::paint(QPainter &p, const QRect &r, bool highlighted)
{
    // The magnifying glass on Spotlight's blue square when open, as the Mac draws it
    if (highlighted) {
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0, QColor(0x5d, 0x8c, 0xf3));
        g.setColorAt(1, QColor(0x1a, 0x52, 0xe0));
        p.fillRect(r, g);
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(highlighted ? Qt::white : Qt::black, 1.6);
    p.setPen(pen);
    QPointF c(r.center().x() - 1.5, r.center().y() - 1.5);
    p.drawEllipse(c, 4.6, 4.6);
    pen.setWidthF(2.6);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(c + QPointF(3.6, 3.6), c + QPointF(7.4, 7.4));
    p.restore();
}

QWidget *SpotlightExtra::popup()
{
    if (!m_popup)
        m_popup = new SpotlightPopup;
    return m_popup;
}
