// SPDX-License-Identifier: LGPL-2.1-or-later
#include "effects.h"

#include "frame.h"
#include "server.h"
#include "window.h"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGTextureMaterial>
#include <algorithm>
#include <cmath>

// ---- the mesh

MeshItem::MeshItem(QQuickItem *parent) : QQuickItem(parent) { setFlag(ItemHasContents); }

void MeshItem::setImage(const QImage &image)
{
    m_image = image;
    m_imageDirty = true;
    update();
}

void MeshItem::setRows(const QList<Row> &rows)
{
    m_rows = rows;
    update();
}

QSGNode *MeshItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGGeometryNode *>(old);
    if (m_image.isNull() || m_rows.size() < 2) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGGeometryNode;
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto *material = new QSGTextureMaterial;
        material->setFlag(QSGMaterial::Blending);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        m_imageDirty = true;
    }
    auto *material = static_cast<QSGTextureMaterial *>(node->material());
    if (m_imageDirty) {
        delete material->texture();
        QSGTexture *t = window()->createTextureFromImage(m_image, QQuickWindow::TextureHasAlphaChannel);
        t->setFiltering(QSGTexture::Linear);
        material->setTexture(t);
        m_imageDirty = false;
        node->markDirty(QSGNode::DirtyMaterial);
    }
    const double h = qMax(1, m_image.height());
    QSGGeometry *g = node->geometry();
    g->allocate(int(m_rows.size()) * 2);
    QSGGeometry::TexturedPoint2D *v = g->vertexDataAsTexturedPoint2D();
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &r = m_rows[i];
        const float tv = float(std::clamp(r.local / h, 0.0, 1.0));
        v[2 * i].set(float(r.left), float(r.y), 0, tv);
        v[2 * i + 1].set(float(r.right), float(r.y), 1, tv);
    }
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}

// ---- the animations

Effects::Effects(Server *server) : QObject(server), m_server(server)
{
    m_timer.setInterval(8);
    connect(&m_timer, &QTimer::timeout, this, &Effects::step);
}

bool Effects::animating(Window *w) const
{
    return std::any_of(m_animations.begin(), m_animations.end(), [w](const Animation &a) { return a.window == w; });
}

void Effects::animate(Window *w, const QImage &snapshot, const QRect &frame, const QRect &tile, bool minimizing, bool scale, double seconds,
                      bool waitForTarget, std::function<void()> finished)
{
    for (int i = m_animations.size() - 1; i >= 0; --i)
        if (m_animations[i].window == w) {
            delete m_animations[i].item;
            if (m_animations[i].finished)
                m_animations[i].finished();
            m_animations.removeAt(i);
        }
    if (snapshot.isNull()) {
        if (finished)
            finished();
        return;
    }
    Animation a;
    a.window = w;
    a.item = new MeshItem(m_server->level(Server::EffectsLevel));
    a.item->setImage(snapshot);
    a.frame = QRect(frame.topLeft(), snapshot.size());
    a.tile = tile;
    a.minimizing = minimizing;
    a.scale = scale;
    a.seconds = seconds;
    a.started = !(minimizing && waitForTarget);
    a.finished = std::move(finished);
    a.clock.start();
    m_animations.append(a);
    if (!m_timer.isActive())
        m_timer.start();
    step();
}

void Effects::retarget(Window *w, const QRect &tile)
{
    for (Animation &a : m_animations)
        if (a.window == w && !tile.isEmpty()) {
            a.tile = tile;
            if (!a.started) {
                a.started = true;
                a.clock.restart();
            }
        }
}

void Effects::step()
{
    for (int i = m_animations.size() - 1; i >= 0; --i) {
        Animation &a = m_animations[i];
        if (!a.started && a.clock.elapsed() >= 150) {   // the Dock didn't answer: where it said before
            a.started = true;
            a.clock.restart();
        }
        const double t = a.started ? std::clamp(a.clock.elapsed() / (a.seconds * 1000), 0.0, 1.0) : 0;
        const double p = a.minimizing ? t : 1 - t;
        const QRectF win(a.frame);
        const QRectF tile(a.tile);
        QList<MeshItem::Row> rows;
        if (a.scale) {
            // setSystemFrame:from: the frame goes to the tile's
            const QRectF r(win.x() + (tile.x() - win.x()) * p, win.y() + (tile.y() - win.y()) * p, win.width() + (tile.width() - win.width()) * p,
                           win.height() + (tile.height() - win.height()) * p);
            rows = {{r.top(), r.left(), r.right(), 0}, {r.bottom(), r.left(), r.right(), win.height()}};
        } else {
            for (const Frame::GenieRow &r : Frame::genieRows(win, tile, p))
                rows.append({r.y, win.x() + r.left, win.right() + r.right, r.local});
        }
        a.item->setRows(rows);
        if (a.started && t >= 1) {
            delete a.item;
            auto finished = std::move(a.finished);
            m_animations.removeAt(i);
            if (finished)
                finished();
        }
    }
    if (m_animations.isEmpty())
        m_timer.stop();
}
