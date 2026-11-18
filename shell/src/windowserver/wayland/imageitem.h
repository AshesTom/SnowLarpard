// SPDX-License-Identifier: LGPL-2.1-or-later
// A picture in the scene: a title bar, a shadow, the desktop picture, a window's snapshot.
#pragma once

#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>

class ImageItem : public QQuickItem {
public:
    explicit ImageItem(QQuickItem *parent = nullptr) : QQuickItem(parent) { setFlag(ItemHasContents); }
    void setImage(const QImage &image)
    {
        m_image = image;
        m_dirty = true;
        update();
    }
    const QImage &image() const { return m_image; }
    void setSmooth(bool smooth) { m_smooth = smooth; }

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        auto *node = static_cast<QSGImageNode *>(old);
        if (m_image.isNull() || width() <= 0 || height() <= 0) {
            delete node;
            return nullptr;
        }
        if (!node) {
            node = window()->createImageNode();
            node->setOwnsTexture(true);
            m_dirty = true;
        }
        if (m_dirty) {
            node->setTexture(window()->createTextureFromImage(m_image, QQuickWindow::TextureHasAlphaChannel));
            m_dirty = false;
        }
        node->setFiltering(m_smooth ? QSGTexture::Linear : QSGTexture::Nearest);
        node->setRect(boundingRect());
        node->setSourceRect(QRectF(QPointF(0, 0), m_image.size()));
        return node;
    }

private:
    QImage m_image;
    bool m_dirty = true, m_smooth = false;
};
