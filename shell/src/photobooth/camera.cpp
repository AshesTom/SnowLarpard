// SPDX-License-Identifier: LGPL-2.1-or-later
#include "camera.h"

#include "appkit.h"
#include "foundation.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <turbojpeg.h>
#include <unistd.h>

namespace PB {

using namespace AK;

namespace {

int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do
        r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

// A picture of any size as a frame: scaled to fill 640 x 480, its centre kept
Image fill(const Image &source)
{
    if (source.width() == VideoWidth && source.height() == VideoHeight)
        return source;
    const Size s = scaledToFill(source.size(), {double(VideoWidth), double(VideoHeight)});
    Image out(VideoWidth, VideoHeight);
    cairo_t *cr = cairo_create(out.surface());
    source.draw(cr, Rect{(VideoWidth - s.w) / 2, (VideoHeight - s.h) / 2, s.w, s.h});
    cairo_destroy(cr);
    return out;
}

unsigned char clamp8(int v) { return static_cast<unsigned char>(std::clamp(v, 0, 255)); }

} // namespace

void Camera::open()
{
    close();
    const char *env = std::getenv("PHOTOBOOTH_CAMERA");
    const std::string source = env ? env : "";
    struct stat st {};
    if (!source.empty() && stat(source.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        m_still = fill(Image::load(source));
        m_status = m_still.isNull() ? NoCamera : On;
        return;
    }
    if (!source.empty()) {
        m_status = start(source) ? On : m_status;
        return;
    }
    m_status = NoCamera;
    for (int i = 0; i < 8; ++i) {
        const std::string device = "/dev/video" + std::to_string(i);
        if (stat(device.c_str(), &st) != 0)
            continue;
        if (start(device)) {
            m_status = On;
            return;
        }
    }
}

bool Camera::start(const std::string &device)
{
    m_fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        m_status = errno == EBUSY ? Busy : NoCamera;
        return false;
    }
    auto fail = [this](Status s) {
        close();
        m_status = s;
        return false;
    };
    v4l2_capability cap {};
    if (xioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0)
        return fail(NoCamera);
    const unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING))
        return fail(NoCamera);   // a metadata node of the same camera, for one
    bool set = false;
    for (unsigned format : {V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG}) {
        v4l2_format fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = VideoWidth;
        fmt.fmt.pix.height = VideoHeight;
        fmt.fmt.pix.pixelformat = format;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat == format) {
            m_format = format;
            m_width = int(fmt.fmt.pix.width);
            m_height = int(fmt.fmt.pix.height);
            set = true;
            break;
        }
        if (errno == EBUSY)
            return fail(Busy);
    }
    if (!set)
        return fail(NoCamera);
    v4l2_requestbuffers req {};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
        return fail(errno == EBUSY ? Busy : NoCamera);
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0)
            return fail(NoCamera);
        void *p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);
        if (p == MAP_FAILED)
            return fail(NoCamera);
        m_buffers.push_back({p, buf.length});
        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
            return fail(NoCamera);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0)
        return fail(errno == EBUSY ? Busy : NoCamera);
    return true;
}

void Camera::close()
{
    if (m_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(m_fd, VIDIOC_STREAMOFF, &type);
        for (const Buffer &b : m_buffers)
            munmap(b.start, b.length);
        ::close(m_fd);
    }
    m_buffers.clear();
    m_fd = -1;
    m_still = Image();
    m_status = Off;
}

bool Camera::read(Image &frame)
{
    if (!m_still.isNull()) {
        // a picture: a frame fifteen times a second, as a camera would give them
        if (now() - m_lastStill < 1.0 / 15)
            return false;
        m_lastStill = now();
        frame = m_still;
        return true;
    }
    if (m_fd < 0)
        return false;
    // every frame waiting, the last one kept
    bool got = false;
    for (;;) {
        v4l2_buffer buf {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) {   // unplugged
                close();
                m_status = NoCamera;
            }
            break;
        }
        if (buf.index < m_buffers.size() && !(buf.flags & V4L2_BUF_FLAG_ERROR))
            got = convert(static_cast<const unsigned char *>(m_buffers[buf.index].start), buf.bytesused, frame) || got;
        xioctl(m_fd, VIDIOC_QBUF, &buf);
    }
    return got;
}

bool Camera::convert(const unsigned char *data, size_t length, Image &frame)
{
    Image raw(m_width, m_height);
    cairo_surface_flush(raw.surface());
    unsigned char *pixels = cairo_image_surface_get_data(raw.surface());
    const int stride = cairo_image_surface_get_stride(raw.surface());
    if (m_format == V4L2_PIX_FMT_MJPEG) {
        tjhandle tj = tjInitDecompress();
        const int r = tjDecompress2(tj, data, static_cast<unsigned long>(length), pixels, m_width, stride, m_height, TJPF_BGRX, TJFLAG_FASTDCT);
        tjDestroy(tj);
        if (r != 0)
            return false;
        for (int y = 0; y < m_height; ++y)   // opaque
            for (int x = 0; x < m_width; ++x)
                pixels[y * stride + x * 4 + 3] = 255;
    } else {
        if (length < size_t(m_width) * m_height * 2)
            return false;
        // YUYV (Y0 U Y1 V), BT.601 video range, as webcams give it
        for (int y = 0; y < m_height; ++y) {
            const unsigned char *in = data + size_t(y) * m_width * 2;
            unsigned char *out = pixels + y * stride;
            for (int x = 0; x < m_width; x += 2, in += 4) {
                const int u = in[1] - 128, v = in[3] - 128;
                const int rr = 409 * v + 128, gg = -100 * u - 208 * v + 128, bb = 516 * u + 128;
                for (int k = 0; k < 2 && x + k < m_width; ++k, out += 4) {
                    const int c = 298 * (in[k * 2] - 16);
                    out[0] = clamp8((c + bb) >> 8);
                    out[1] = clamp8((c + gg) >> 8);
                    out[2] = clamp8((c + rr) >> 8);
                    out[3] = 255;
                }
            }
        }
    }
    cairo_surface_mark_dirty(raw.surface());
    frame = fill(raw);
    return true;
}

} // namespace PB
