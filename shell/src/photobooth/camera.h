// SPDX-License-Identifier: LGPL-2.1-or-later
// Photo Booth's CameraController: 10.6's is a QTCaptureSession on the iSight; here a Video4Linux
// device (the first one, or PHOTOBOOTH_CAMERA: a device, or a picture standing for the camera),
// read as 640 x 480 frames, the size PBView's layers have (MainMenu.nib: {640, 480}).
#pragma once

#include "graphics.h"

#include <string>
#include <vector>

namespace PB {

constexpr int VideoWidth = 640, VideoHeight = 480;

class Camera {
public:
    // CameraController's _cameraStatus, as its CameraLayer words it (Localizable.strings)
    enum Status { On, Off, Busy, NoCamera };
    ~Camera() { close(); }
    void open();
    void close();
    Status status() const { return m_status; }
    // The newest frame, 640 x 480, as the camera sees it (not mirrored); false when none came
    bool read(AK::Image &frame);

private:
    bool start(const std::string &device);
    bool convert(const unsigned char *data, size_t length, AK::Image &frame);

    Status m_status = Off;
    int m_fd = -1;
    unsigned m_format = 0;
    int m_width = 0, m_height = 0;
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> m_buffers;
    AK::Image m_still;   // a picture as the camera
    std::string m_stillPath;
    long m_stillTime = 0;
    double m_lastStill = 0;
};

} // namespace PB
