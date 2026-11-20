// SPDX-License-Identifier: LGPL-2.1-or-later
// Linux's glue, not a Mac framework: AquaStyle (shell/src/common/aquastyle.cpp: Qt's widgets drawn with
// CoreUI's own art, as 10.6's AppKit draws them) as a Qt 6 style plugin, so that the Linux Qt
// applications of the session look as the Mac's applications did. The session asks for it
// (QT_PLUGIN_PATH, QT_STYLE_OVERRIDE=Aqua); a program may also pass -style Aqua.
//
// Besides the controls, what an Aqua application has: Lucida Grande 13 (the DVD's), the window's 237
// grey, white text backgrounds, the light blue text selection (NSColor selectedTextBackgroundColor) and
// the blue of selected rows (alternateSelectedControlColor), whatever colours the desktop's own theme
// gave the application.
#include "aquastyle.h"
#include "assets.h"

#include <QApplication>
#include <QPalette>
#include <QStylePlugin>

namespace {

QPalette aquaPalette()
{
    QPalette p;
    const QColor window(237, 237, 237), text(0, 0, 0), disabled(0, 0, 0, 100);
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool off = g == QPalette::Disabled;
        p.setColor(g, QPalette::Window, window);
        p.setColor(g, QPalette::WindowText, off ? disabled : text);
        p.setColor(g, QPalette::Base, Qt::white);
        p.setColor(g, QPalette::AlternateBase, QColor(237, 243, 254));   // the list's alternating rows
        p.setColor(g, QPalette::Text, off ? disabled : text);
        p.setColor(g, QPalette::PlaceholderText, QColor(0, 0, 0, 110));
        p.setColor(g, QPalette::Button, window);
        p.setColor(g, QPalette::ButtonText, off ? disabled : text);
        p.setColor(g, QPalette::BrightText, Qt::white);
        p.setColor(g, QPalette::ToolTipBase, QColor(255, 255, 199));   // the help tag's yellow
        p.setColor(g, QPalette::ToolTipText, text);
        p.setColor(g, QPalette::Light, Qt::white);
        p.setColor(g, QPalette::Midlight, QColor(245, 245, 245));
        p.setColor(g, QPalette::Mid, QColor(165, 165, 165));
        p.setColor(g, QPalette::Dark, QColor(128, 128, 128));
        p.setColor(g, QPalette::Shadow, QColor(0, 0, 0, 90));
        p.setColor(g, QPalette::Link, QColor(0, 0, 238));
        p.setColor(g, QPalette::LinkVisited, QColor(85, 26, 139));
    }
    // the selection: Qt has one colour for rows and text alike; the rows' (alternateSelectedControl-
    // Color, white text) in the key window, the grey of an inactive one (secondarySelectedControlColor)
    p.setColor(QPalette::Active, QPalette::Highlight, QColor(56, 117, 215));
    p.setColor(QPalette::Active, QPalette::HighlightedText, Qt::white);
    for (QPalette::ColorGroup g : {QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(g, QPalette::Highlight, QColor(212, 212, 212));
        p.setColor(g, QPalette::HighlightedText, Qt::black);
    }
    p.setColor(QPalette::Active, QPalette::Accent, QColor(56, 117, 215));
    return p;
}

class AquaApplicationStyle : public AquaStyle {
public:
    QPalette standardPalette() const override { return aquaPalette(); }
    void polish(QApplication *app) override
    {
        QProxyStyle::polish(app);
        Assets::loadFonts();
        app->setFont(Assets::uiFont(13));
        app->setPalette(aquaPalette());
    }
    void polish(QPalette &palette) override { palette = aquaPalette(); }
    int styleHint(StyleHint sh, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const override
    {
        // no mnemonics on the Mac: "&Open" is "Open"
        if (sh == SH_UnderlineShortcut)
            return 0;
        return AquaStyle::styleHint(sh, opt, w, ret);
    }
    using AquaStyle::polish;
    using QProxyStyle::polish;
};

} // namespace

class AquaStylePlugin : public QStylePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QStyleFactoryInterface_iid FILE "aqua.json")
public:
    QStyle *create(const QString &key) override
    {
        return key.compare(QLatin1String("Aqua"), Qt::CaseInsensitive) == 0 ? new AquaApplicationStyle : nullptr;
    }
};

#include "aquastyleplugin.moc"
