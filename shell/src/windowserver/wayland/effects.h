// SPDX-License-Identifier: LGPL-2.1-or-later
// Minimizing, as 10.6's Dock has the window server do it (WAWindow genie:direction:genie: and
// setSystemFrame:from:, docs/reverse-status.md): the window's picture warped into its Dock tile and
// back out of it, here as a mesh in the scene (frame.cpp's genie rows, the X11 server's).
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include "window.h"
#include <QRect>
#include <QTimer>
#include <functional>

class Server;
class Window;

// A picture drawn through a mesh of rows (the genie's), or into a rectangle (the scale's)
class MeshItem : public QQuickItem {
public:
    explicit MeshItem(QQuickItem *parent);
    void setImage(const QImage &image);
    // rows: y on the screen, the left and right edges, the picture's row there
    struct Row {
        double y, left, right, local;
    };
    void setRows(const QList<Row> &rows);

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    QImage m_image;
    QList<Row> m_rows;
    bool m_imageDirty = true;
};

class Effects : public QObject {
    Q_OBJECT
public:
    explicit Effects(Server *server);
    // the window's picture (its frame, `snapshot`) between `frame` and `tile` on the screen. Minimizing
    // with waitForTarget, the clock starts when the Dock gives the tile's place (retarget), or after
    // 150 ms; `finished` when it is over
    void animate(Window *w, const QImage &snapshot, const QRect &frame, const QRect &tile, bool minimizing, bool scale, double seconds,
                 bool waitForTarget, std::function<void()> finished = {});
    void retarget(Window *w, const QRect &tile);
    bool animating(Window *w) const;

private:
    struct Animation {
        QPointer<Window> window;
        uint32_t id = 0;
        MeshItem *item = nullptr;
        QRect frame, tile;
        bool minimizing = true, scale = false, started = false;
        double seconds = 0.5;
        QElapsedTimer clock;
        std::function<void()> finished;
    };
    void step();
    Server *m_server;
    QList<Animation> m_animations;
    QTimer m_timer;
};
