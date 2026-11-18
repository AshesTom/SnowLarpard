// SPDX-License-Identifier: LGPL-2.1-or-later
#include "battery.h"

#include "aqua.h"
#include "apps.h"
#include "assets.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QMenu>
#include <QProcess>
#include <QRegularExpression>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

static const QString menuExtras = QStringLiteral("/System/Library/CoreServices/Menu Extras/");

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

SYSTEMUI_MENU_EXTRA(BatteryExtra)
