// SPDX-License-Identifier: LGPL-2.1-or-later
// The Dock's menus as CoreUI draws them (CUIRenderer::DrawDockMenu): a dark, rounded nine-piece frame
// from the shared art, with a callout pointing at the icon.
#pragma once

#include <QImage>
#include <QMenu>

class DockMenu : public QMenu {
    Q_OBJECT
public:
    // `wallpaper`: the desktop picture, drawn under the frame when nothing composites
    explicit DockMenu(const QImage &wallpaper, QWidget *parent = nullptr, bool callout = true);
    DockMenu *addDockMenu(const QString &title);
    // Shows the menu with the callout's tip at (`anchorX`, `iconTop`), global coordinates.
    void popupAbove(int anchorX, int iconTop);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QImage m_wallpaper;
    bool m_callout;
    int m_anchorX = -1;   // in the menu's coordinates
};
