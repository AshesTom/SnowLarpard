// SPDX-License-Identifier: LGPL-2.1-or-later
#include "volume.h"

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

SYSTEMUI_MENU_EXTRA(VolumeExtra)
