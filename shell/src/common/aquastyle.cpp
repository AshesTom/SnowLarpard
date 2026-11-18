// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aqua.h"
#include "aquastyle.h"

#include "artfile.h"
#include "assets.h"
#include "x11.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>

namespace {

ArtFile::Element art(QStringList path)
{
    ArtFile::Element e = ArtFile::element(path);
    // the state an element doesn't have: without it ("active" mostly)
    for (const char *drop : {"active", "clear", "inactive"}) {
        if (!e.isNull())
            break;
        if (path.removeAll(QString::fromLatin1(drop)))
            e = ArtFile::element(path);
    }
    return e;
}

// An element in a rectangle: 1 x 1 centred, 1 x 3 across (centred vertically), 3 x 1 down, 3 x 3 all round
void drawElement(QPainter *p, const ArtFile::Element &e, const QRect &r)
{
    if (e.isNull())
        return;
    const auto &pc = e.pieces;
    if (e.rows == 1 && e.columns == 1) {
        QImage img = pc[0];
        p->drawImage(QPoint(r.x() + (r.width() - img.width()) / 2, r.y() + (r.height() - img.height()) / 2), img);
    } else if (e.rows == 1 && e.columns == 3) {
        const int h = pc[0].height(), y = r.y() + (r.height() - h) / 2;
        const int l = pc[0].width(), rr = pc[2].width();
        p->drawImage(QRect(r.x(), y, l, h), pc[0]);
        p->drawImage(QRect(r.x() + l, y, qMax(0, r.width() - l - rr), h), pc[1]);
        p->drawImage(QRect(r.right() + 1 - rr, y, rr, h), pc[2]);
    } else if (e.rows == 1 && e.columns == 2) {
        // a popup: its left end, its middle (the left end's last column, stretched), its arrows
        const int h = pc[0].height(), y = r.y() + (r.height() - h) / 2;
        const int l = pc[0].width(), rr = pc[1].width();
        p->drawImage(QRect(r.x(), y, l, h), pc[0]);
        p->drawImage(QRect(r.x() + l, y, qMax(0, r.width() - l - rr), h), pc[0], QRect(l - 1, 0, 1, h));
        p->drawImage(QRect(r.right() + 1 - rr, y, rr, h), pc[1]);
    } else if (e.rows == 3 && e.columns == 1) {
        const int w = pc[0].width(), x = r.x() + (r.width() - w) / 2;
        const int t = pc[0].height(), b = pc[2].height();
        p->drawImage(QRect(x, r.y(), w, t), pc[0]);
        p->drawImage(QRect(x, r.y() + t, w, qMax(0, r.height() - t - b)), pc[1]);
        p->drawImage(QRect(x, r.bottom() + 1 - b, w, b), pc[2]);
    } else if (e.rows == 3 && e.columns == 3) {
        const int l = pc[0].width(), rt = pc[2].width(), t = pc[0].height(), b = pc[6].height();
        const int mw = qMax(0, r.width() - l - rt), mh = qMax(0, r.height() - t - b);
        const int xs[] = {r.x(), r.x() + l, r.right() + 1 - rt}, ws[] = {l, mw, rt};
        const int ys[] = {r.y(), r.y() + t, r.bottom() + 1 - b}, hs[] = {t, mh, b};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                p->drawImage(QRect(xs[col], ys[row], ws[col], hs[row]), pc[row * 3 + col]);
    }
}

QString windowState(const QStyleOption *opt)
{
    if (!(opt->state & QStyle::State_Enabled))
        return "disabled";
    if (opt->state & QStyle::State_Sunken)
        return "pressed";
    return (opt->state & QStyle::State_Active) ? "active" : "inactive";
}

bool prop(const QWidget *w, const char *name) { return w && w->property(name).toBool(); }

// the scroll bar's parts (vertical): the track, then the two arrows together at the bottom (10.6's default)
constexpr int ScrollWidth = 15, ArrowCap = 8, UpArrow = 14, DownArrow = 16;

} // namespace

AquaStyle::AquaStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}

void AquaStyle::polish(QWidget *w)
{
    QProxyStyle::polish(w);
    if (qobject_cast<QMenu *>(w)) {
        // the menus' round corners: translucent with a compositor, else what is under them, taken first
        if (X11::compositing())
            w->setAttribute(Qt::WA_TranslucentBackground);
        w->installEventFilter(this);
    }
    if (qobject_cast<QPushButton *>(w) || qobject_cast<QCheckBox *>(w) || qobject_cast<QComboBox *>(w) || qobject_cast<QTabBar *>(w) ||
        qobject_cast<QScrollBar *>(w))
        w->setAttribute(Qt::WA_Hover);
}

void AquaStyle::unpolish(QWidget *w)
{
    if (qobject_cast<QMenu *>(w))
        w->removeEventFilter(this);
    QProxyStyle::unpolish(w);
}

bool AquaStyle::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::Show && !X11::compositing()) {
        // before the menu is on screen: what is under it, for its corners
        auto *w = qobject_cast<QWidget *>(o);
        if (QScreen *s = w->screen())
            w->setProperty("aquaUnder", s->grabWindow(0, w->x() - s->geometry().x(), w->y() - s->geometry().y(), w->width(), w->height()));
    }
    return QProxyStyle::eventFilter(o, e);
}

int AquaStyle::styleHint(StyleHint sh, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const
{
    switch (sh) {
    case SH_ScrollBar_Transient:
    case SH_Menu_FlashTriggeredItem:
        return 0;
    case SH_ScrollBar_LeftClickAbsolutePosition:
        return 0;
    case SH_TabBar_Alignment:
        return Qt::AlignCenter;
    case SH_Menu_SubMenuPopupDelay:
        return 100;
    default:
        return QProxyStyle::styleHint(sh, opt, w, ret);
    }
}

int AquaStyle::pixelMetric(PixelMetric pm, const QStyleOption *opt, const QWidget *w) const
{
    switch (pm) {
    case PM_ScrollBarExtent:
        return ScrollWidth;
    case PM_ScrollBarSliderMin:
        return 26;
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
        return 18;
    case PM_MenuPanelWidth:
    case PM_MenuHMargin:
        return 0;
    case PM_MenuVMargin:
        return 5;   // measured: the first item's text 8 px under the menu's top, the last 9 above its bottom
    case PM_TabBarTabOverlap:
    case PM_TabBarTabHSpace:
        return pm == PM_TabBarTabHSpace ? 28 : 0;
    case PM_TabBarBaseOverlap:
        return 12;   // the tabs sit on the pane's edge
    case PM_TabBarTabVSpace:
        return 4;
    default:
        return QProxyStyle::pixelMetric(pm, opt, w);
    }
}

QSize AquaStyle::sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &size, const QWidget *w) const
{
    switch (ct) {
    case CT_MenuItem: {
        // measured on 10.6's desktop menu: 21 px items, 12 px separators, the text 21 px in
        auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt);
        if (mi && mi->menuItemType == QStyleOptionMenuItem::Separator)
            return QSize(size.width(), 12);
        QFontMetrics fm(mi ? mi->font : QFont());
        int textW = mi ? fm.horizontalAdvance(mi->text.section('\t', 0, 0)) : size.width();
        int shortcutW = mi && mi->text.contains('\t') ? fm.horizontalAdvance(mi->text.section('\t', 1)) + 24 : 0;
        if (mi && !mi->icon.isNull() && prop(w, "aquaTrailingIcons")) {
            // AME_AirPortNetworkItemView's -idealMenuItemWidth: the title 21 in, then the icons (the
            // lock's 19, the bars' 6 + 17) and 12 at the right
            const QSize icon = mi->icon.availableSizes().value(0);
            return QSize(21 + textW + icon.width() + 12, 21);
        }
        return QSize(21 + textW + shortcutW + 22, 21);
    }
    case CT_PushButton:
        if (prop(w, "aquaHelp"))
            return QSize(25, 25);
        if (prop(w, "aquaTextured"))
            return QSize(qMax(size.width() + 20, 32), 24);
        return QSize(qMax(size.width() + 28, 70), 32);
    case CT_ComboBox:
        return QSize(size.width() + 30, 26);
    case CT_TabBarTab:
        return QSize(size.width(), 24);
    default:
        return QProxyStyle::sizeFromContents(ct, opt, size, w);
    }
}

void AquaStyle::drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const
{
    switch (pe) {
    case PE_IndicatorCheckBox: {
        QString value = (opt->state & State_On) ? "onchecked" : (opt->state & State_NoChange) ? "mixed" : "off";
        drawElement(p, art({"checkbox", "clear", value, windowState(opt), "regular"}), opt->rect);
        return;
    }
    case PE_PanelMenu:
    case PE_FrameMenu: {
        if (pe == PE_FrameMenu)
            return;
        QPixmap under = w ? w->property("aquaUnder").value<QPixmap>() : QPixmap();
        if (!under.isNull())
            p->drawPixmap(0, 0, under);
        const bool barMenu = w && w->property("aquaMenu").toString() == "straighttop";
        ArtFile::Element e = art({"menu", barMenu ? "straighttop" : "allround"});
        if (e.isNull())
            p->fillRect(opt->rect, QColor(248, 248, 248));
        else
            drawElement(p, e, opt->rect);
        return;
    }
    case PE_IndicatorBranch: {
        if (!(opt->state & State_Children))
            return;
        QString open = (opt->state & State_Open) ? "open" : "close";
        drawElement(p, art({"disclosuretriangle", open}), opt->rect);
        return;
    }
    case PE_PanelLineEdit:
        if (prop(w, "aquaSearch")) {
            drawElement(p, art({"textfieldround", (opt->state & State_Enabled) ? "active" : "disabled", "fortoolbars", "regular"}), opt->rect);
            return;
        }
        break;
    case PE_FrameTabWidget: {
        QRect r = opt->rect;
        drawElement(p, art({"tabpane", (opt->state & State_Enabled) ? "active" : "disabled", "opaque"}), r);
        return;
    }
    case PE_FrameFocusRect:
        return;   // 10.6's focus ring is for later
    default:
        break;
    }
    QProxyStyle::drawPrimitive(pe, opt, p, w);
}

void AquaStyle::drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p, const QWidget *w) const
{
    switch (ce) {
    case CE_PushButtonBevel: {
        auto *b = qstyleoption_cast<const QStyleOptionButton *>(opt);
        if (prop(w, "aquaHelp")) {
            QString s = (opt->state & State_Sunken) ? "pressed" : windowState(opt);
            drawElement(p, art({"roundbutton", "help", s, "regular"}), opt->rect);
            return;
        }
        if (prop(w, "aquaTextured")) {
            // toolbar buttons: textured segments; side by side ones share their edges
            QString s = (opt->state & State_Sunken) ? "pressed" : windowState(opt);
            ArtFile::Element e = art({"segment", "clear", "fortexture", s, "regular"});
            QString pos = w->property("aquaSegmentPosition").toString();
            p->save();
            p->setClipRect(opt->rect);
            QRect r = opt->rect;
            const int cap = e.isNull() ? 0 : e.pieces[0].width();
            if (pos == "left")
                r.setRight(r.right() + cap);
            else if (pos == "right")
                r.setLeft(r.left() - cap);
            drawElement(p, e, r);
            if (pos == "right")
                p->fillRect(QRect(opt->rect.left(), opt->rect.top() + 4, 1, opt->rect.height() - 8), QColor(0, 0, 0, 90));
            p->restore();
            return;
        }
        const bool isDefault = b && (b->features & QStyleOptionButton::DefaultButton) && (opt->state & State_Active);
        QString s = (opt->state & State_Sunken) ? "pressed" : windowState(opt);
        drawElement(p, art({"pushbutton", isDefault && s == "active" ? "blue" : "clear", s, "regular"}), opt->rect);
        return;
    }
    case CE_PushButtonLabel:
        if (prop(w, "aquaHelp"))
            return;   // the art has its "?"
        break;
    case CE_MenuItem: {
        auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt);
        if (!mi)
            break;
        const QRect r = opt->rect;
        if (mi->menuItemType == QStyleOptionMenuItem::Separator) {
            // measured: two rows, 242 then 224, in the middle of the 12 px
            const int y = r.top() + 5;
            p->fillRect(QRect(r.left() + 1, y, r.width() - 2, 1), QColor(0, 0, 0, 15));
            p->fillRect(QRect(r.left() + 1, y + 1, r.width() - 2, 1), QColor(0, 0, 0, 40));
            return;
        }
        const bool selected = (opt->state & State_Selected) && (opt->state & State_Enabled);
        if (selected) {
            ArtFile::Element sel = art({"menu", "selection"});
            if (sel.isNull())
                p->fillRect(r, QColor(0x2a, 0x5c, 0xe0));
            else
                p->drawImage(r, sel.single());
        }
        QColor text = !(opt->state & State_Enabled) ? QColor(0, 0, 0, 90) : selected ? Qt::white : Qt::black;
        p->setPen(text);
        p->setFont(mi->font);
        if (mi->checked)
            Aqua::drawText(*p, QRect(r.left() + 6, r.top(), 14, r.height()), Qt::AlignCenter, QString::fromUtf8("✓"));
        const QString label = mi->text.section('\t', 0, 0), shortcut = mi->text.section('\t', 1);
        Aqua::drawText(*p, r.adjusted(21, 0, -20, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic, label);
        if (!shortcut.isEmpty())
            Aqua::drawText(*p, r.adjusted(21, 0, -20, 0), Qt::AlignVCenter | Qt::AlignRight, shortcut);
        if (!mi->icon.isNull() && prop(w, "aquaTrailingIcons")) {
            // the menu extras' pictures at the right of the title (the AirPort menu's lock and bars),
            // their selected variant white
            const QSize size = mi->icon.availableSizes().value(0);
            const QPixmap px = mi->icon.pixmap(size, p->device() ? p->device()->devicePixelRatioF() : 1.0,
                                               selected ? QIcon::Selected : QIcon::Normal);
            p->drawPixmap(QPoint(r.right() + 1 - 12 - size.width(), r.top() + (r.height() - size.height()) / 2), px);
        }
        if (mi->menuItemType == QStyleOptionMenuItem::SubMenu) {
            // the submenu's triangle
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(text);
            const qreal cx = r.right() - 12, cy = r.center().y() + 0.5;
            p->drawPolygon(QPolygonF({QPointF(cx - 3, cy - 5), QPointF(cx - 3, cy + 5), QPointF(cx + 4, cy)}));
            p->restore();
        }
        return;
    }
    case CE_MenuEmptyArea:
        return;
    case CE_TabBarTabShape: {
        // the tabs as one segmented control: the first's left end, the last's right end, separators between
        auto *tab = qstyleoption_cast<const QStyleOptionTab *>(opt);
        if (!tab)
            break;
        const bool selected = opt->state & State_Selected;
        ArtFile::Element e = art({"tabs", selected ? "blue" : "clear", (opt->state & State_Sunken) ? "pressed" : windowState(opt),
                                  "horizontal", "regular"});
        if (e.isNull())
            e = art({"tabs", selected ? "blue" : "clear", "horizontal", "regular"});
        const int cap = e.isNull() ? 0 : e.pieces[0].width();
        QRect r = opt->rect;
        const bool first = tab->position == QStyleOptionTab::Beginning || tab->position == QStyleOptionTab::OnlyOneTab;
        const bool last = tab->position == QStyleOptionTab::End || tab->position == QStyleOptionTab::OnlyOneTab;
        if (!first)
            r.setLeft(r.left() - cap);
        if (!last)
            r.setRight(r.right() + cap);
        p->save();
        p->setClipRect(opt->rect);
        drawElement(p, e, r);
        if (!first)
            p->fillRect(QRect(opt->rect.left(), opt->rect.top() + 3, 1, opt->rect.height() - 7), QColor(0, 0, 0, 80));
        p->restore();
        return;
    }
    case CE_TabBarTabLabel: {
        auto *tab = qstyleoption_cast<const QStyleOptionTab *>(opt);
        if (!tab)
            break;
        p->setPen((opt->state & State_Enabled) ? Qt::black : QColor(0, 0, 0, 100));
        Aqua::drawText(*p, opt->rect.adjusted(0, -1, 0, -1), Qt::AlignCenter, tab->text);
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawControl(ce, opt, p, w);
}

QRect AquaStyle::subElementRect(SubElement se, const QStyleOption *opt, const QWidget *w) const
{
    if (se == SE_CheckBoxIndicator) {
        QRect r = QProxyStyle::subElementRect(se, opt, w);
        return QRect(opt->rect.left(), opt->rect.top() + (opt->rect.height() - 18) / 2, 18, 18);
        Q_UNUSED(r);
    }
    if (se == SE_CheckBoxContents) {
        QRect r = opt->rect;
        r.setLeft(r.left() + 20);
        return r;
    }
    return QProxyStyle::subElementRect(se, opt, w);
}

QRect AquaStyle::subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc, const QWidget *w) const
{
    if (cc == CC_ScrollBar) {
        auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(opt);
        if (sb && sb->orientation == Qt::Vertical) {
            const QRect r = opt->rect;
            const int arrows = UpArrow + DownArrow;
            const QRect groove(r.x(), r.y(), r.width(), qMax(0, r.height() - arrows));
            switch (sc) {
            case SC_ScrollBarSubLine:
                return QRect(r.x(), r.bottom() + 1 - arrows, r.width(), UpArrow);
            case SC_ScrollBarAddLine:
                return QRect(r.x(), r.bottom() + 1 - DownArrow, r.width(), DownArrow);
            case SC_ScrollBarGroove:
                return groove;
            case SC_ScrollBarSlider: {
                const int range = sb->maximum - sb->minimum;
                if (range <= 0)
                    return QRect();
                const int len = qMax(pixelMetric(PM_ScrollBarSliderMin, opt, w),
                                     int(qint64(groove.height()) * sb->pageStep / (range + sb->pageStep)));
                const int pos = sliderPositionFromValue(sb->minimum, sb->maximum, sb->sliderPosition, groove.height() - len, sb->upsideDown);
                return QRect(groove.x(), groove.y() + pos, groove.width(), len);
            }
            case SC_ScrollBarSubPage:
            case SC_ScrollBarAddPage: {
                QRect slider = subControlRect(cc, opt, SC_ScrollBarSlider, w);
                return sc == SC_ScrollBarSubPage ? QRect(groove.x(), groove.y(), groove.width(), slider.top() - groove.top())
                                                 : QRect(groove.x(), slider.bottom() + 1, groove.width(), groove.bottom() - slider.bottom());
            }
            default:
                break;
            }
        }
    }
    if (cc == CC_ComboBox) {
        // a popup button: the text in its body, the arrows' end on the right
        if (sc == SC_ComboBoxEditField)
            return opt->rect.adjusted(10, 0, -26, 0);
        if (sc == SC_ComboBoxArrow)
            return QRect(opt->rect.right() - 22, opt->rect.top(), 23, opt->rect.height());
        if (sc == SC_ComboBoxFrame)
            return opt->rect;
    }
    return QProxyStyle::subControlRect(cc, opt, sc, w);
}

QStyle::SubControl AquaStyle::hitTestComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, const QPoint &pt, const QWidget *w) const
{
    if (cc == CC_ScrollBar) {
        auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(opt);
        if (sb && sb->orientation == Qt::Vertical) {
            for (SubControl sc : {SC_ScrollBarSlider, SC_ScrollBarSubLine, SC_ScrollBarAddLine, SC_ScrollBarSubPage, SC_ScrollBarAddPage})
                if (subControlRect(cc, opt, sc, w).contains(pt))
                    return sc;
            return SC_None;
        }
    }
    return QProxyStyle::hitTestComplexControl(cc, opt, pt, w);
}

void AquaStyle::drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p, const QWidget *w) const
{
    if (cc == CC_ScrollBar) {
        auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(opt);
        if (sb && sb->orientation == Qt::Vertical) {
            const bool windowActive = (opt->state & State_Active) || (w && w->isActiveWindow());
            const QString state = windowActive ? "active" : "inactive";
            const QRect groove = subControlRect(cc, opt, SC_ScrollBarGroove, w);
            const QRect slider = subControlRect(cc, opt, SC_ScrollBarSlider, w);
            if (slider.isEmpty()) {
                // one piece, the track's cross section: stretched along its length
                const ArtFile::Element e = art({"scrollbars", "nothingtoscroll", "regular", "track", "vertical"});
                if (e.pieces.size() == 1)
                    p->drawImage(opt->rect, e.pieces[0]);
                else
                    drawElement(p, e, opt->rect);
                return;
            }
            // the track, the arrows' block at the bottom (its top cap over the track's end), the thumb
            drawElement(p, art({"scrollbars", state, "regular", "track", "vertical"}), groove.adjusted(0, 0, 0, ArrowCap));
            ArtFile::Element arrows = art({"scrollbars", state, "arrows", "bottom", "double", "regular"});
            if (!arrows.isNull() && arrows.pieces.size() == 3) {
                const int top = opt->rect.bottom() + 1 - UpArrow - DownArrow - ArrowCap;
                int y = top;
                for (const QImage &piece : arrows.pieces) {
                    p->drawImage(QRect(opt->rect.x(), y, ScrollWidth, piece.height()), piece);
                    y += piece.height();
                }
            }
            drawElement(p, art({"scrollbars", windowActive ? "blue" : "clear", "regular", "thumb", "vertical"}), slider);
            return;
        }
    }
    if (cc == CC_ComboBox) {
        auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>(opt);
        if (cb && !cb->editable) {
            const bool active = (opt->state & State_Active) && (opt->state & State_Enabled);
            QString s = !(opt->state & State_Enabled) ? "disabled" : (opt->state & State_Sunken) ? "pressed" : windowState(opt);
            ArtFile::Element e = art({"popup", active ? "blue" : "clear", s, "regular", "rightsidebutton"});
            if (e.isNull())
                e = art({"popup", "clear", s, "regular", "rightsidebutton"});
            drawElement(p, e, opt->rect);
            return;
        }
    }
    QProxyStyle::drawComplexControl(cc, opt, p, w);
}
