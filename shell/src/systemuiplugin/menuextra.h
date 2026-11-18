// SPDX-License-Identifier: LGPL-2.1-or-later
// SystemUIPlugin.framework: what the menu extras are made of (NSMenuExtra, NSMenuExtraView), a library
// of its own shared by SystemUIServer and the .menu bundles it loads (shell/src/menuextras): each
// bundle, a library at Menu Extras/<Name>.menu/Contents/MacOS/<Name>, gives its extra through
// SYSTEMUI_MENU_EXTRA(Class), as a bundle's NSPrincipalClass gives the Mac's.
#pragma once

#include <QImage>
#include <QObject>
#include <QPainter>
#include <QRect>

class QAction;
class QMenu;
class QWidget;

#if defined(SYSTEMUIPLUGIN_LIBRARY)
#define SYSTEMUIPLUGIN_EXPORT Q_DECL_EXPORT
#else
#define SYSTEMUIPLUGIN_EXPORT Q_DECL_IMPORT
#endif

class SYSTEMUIPLUGIN_EXPORT MenuExtra : public QObject {
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

// The bundle's principal class: the function SystemUIServer looks up in it
#define SYSTEMUI_MENU_EXTRA(Class) \
    extern "C" Q_DECL_EXPORT MenuExtra *SystemUIMenuExtraCreate(QObject *parent) { return new Class(parent); }

// A menu-bar icon (black PDF art) for a state: white when highlighted, as the Mac draws templates.
SYSTEMUIPLUGIN_EXPORT QImage menuIcon(const QString &macPath, int height, qreal dpr, bool white);
// The icon centred in the extra's rectangle.
SYSTEMUIPLUGIN_EXPORT void drawCentered(QPainter &p, const QRect &r, const QImage &img);
// System Preferences on a pane (its short name: "network", "bluetooth", "sound"...).
SYSTEMUIPLUGIN_EXPORT void openPreferencePane(const QString &pane);
// Items taken out of a menu (deleted if the menu owns them), once their successors are in: a menu on
// screen emptied, even for a moment, has no width and the X server takes it down (and every popup with
// it), as QMenu::clear() does to it.
SYSTEMUIPLUGIN_EXPORT void removeActions(QMenu *menu, const QList<QAction *> &actions);
// The first of these programs found on the PATH, started with the arguments; false if none is.
SYSTEMUIPLUGIN_EXPORT bool startFirst(const QStringList &programs, const QStringList &args = {});
