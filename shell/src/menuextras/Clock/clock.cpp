// SPDX-License-Identifier: LGPL-2.1-or-later
#include "clock.h"

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

SYSTEMUI_MENU_EXTRA(ClockExtra)
