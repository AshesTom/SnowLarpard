// SPDX-License-Identifier: LGPL-2.1-or-later
#include "spotlight.h"

#include "apps.h"
#include "assets.h"

#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainterPath>
#include <QVBoxLayout>

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
