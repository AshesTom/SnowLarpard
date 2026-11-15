// SPDX-License-Identifier: LGPL-2.1-or-later
// The menu extras at the right of the menu bar: the volume, the battery, the clock and Spotlight,
// drawn with the icons of their .menu bundles on the DVD.
#pragma once

#include <QImage>
#include <QObject>
#include <QPainter>
#include <QRect>

class QMenu;
class QWidget;

class MenuExtra : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual int width(const QFontMetrics &fm) const = 0;
    virtual void paint(QPainter &p, const QRect &r, bool highlighted) = 0;
    // A menu to drop down, or nullptr when clicked() does the work.
    virtual QMenu *menu() { return nullptr; }
    // A popup of its own (the volume's slider, Spotlight's field) at the given point.
    virtual QWidget *popup() { return nullptr; }
    virtual bool visible() const { return true; }
signals:
    void changed();
};

class ClockExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit ClockExtra(QObject *parent);
    int width(const QFontMetrics &fm) const override;
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QMenu *menu() override;
private:
    QString text() const;
    QMenu *m_menu = nullptr;
};

class VolumeExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit VolumeExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 30; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QWidget *popup() override;
    bool visible() const override { return m_available; }
private:
    void refresh();
    void set(int percent);
    int m_level = 50;
    bool m_muted = false, m_available = false;
    QWidget *m_popup = nullptr;
};

class BatteryExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit BatteryExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 34; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QMenu *menu() override;
    bool visible() const override { return !m_battery.isEmpty(); }
private:
    void refresh();
    QString m_battery;
    int m_percent = 100;
    QString m_status;
    QMenu *m_menu = nullptr;
};

class SpotlightExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit SpotlightExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 28; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QWidget *popup() override;
private:
    QWidget *m_popup = nullptr;
};

// A menu-bar icon (black PDF art) for a state: white when highlighted, as the Mac draws templates.
QImage menuIcon(const QString &macPath, int height, qreal dpr, bool white);
