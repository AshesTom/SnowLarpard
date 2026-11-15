// SPDX-License-Identifier: LGPL-2.1-or-later
// Aqua for the shell's Qt widgets: the controls drawn with CoreUI's own art (ArtFile.bin, by path), as
// 10.6's AppKit draws them. What a widget is beyond its class goes in a property:
//
//   "aquaTextured"   a toolbar button (segment/fortexture)       "aquaHelp"   the round help button
//   "aquaSearch"     a rounded search field (textfieldround)     "aquaMenu"   "straighttop" for the menu
//                                                                             bar's menus (allround else)
//   "aquaSegmentPosition"  "left", "middle", "right" for buttons side by side (back / forward)
//
// The art and its layout are in docs/reverse-status.md, section 8.
#pragma once

#include <QProxyStyle>

class AquaStyle : public QProxyStyle {
    Q_OBJECT
public:
    AquaStyle();
    void polish(QWidget *w) override;
    void unpolish(QWidget *w) override;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const override;
    void drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p, const QWidget *w) const override;
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p, const QWidget *w) const override;
    QRect subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc, const QWidget *w) const override;
    SubControl hitTestComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, const QPoint &pt, const QWidget *w) const override;
    QRect subElementRect(SubElement se, const QStyleOption *opt, const QWidget *w) const override;
    QSize sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &size, const QWidget *w) const override;
    int pixelMetric(PixelMetric pm, const QStyleOption *opt, const QWidget *w) const override;
    int styleHint(StyleHint sh, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const override;
    bool eventFilter(QObject *o, QEvent *e) override;
};
