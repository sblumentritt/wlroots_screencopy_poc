#pragma once

#include <QImage>

struct wl_display;
struct wl_buffer;

namespace wayland {

class WlrootScreenCapture {
public:
    static WlrootScreenCapture& instance() {
        static WlrootScreenCapture _instance{};
        return _instance;
    }

    QImage captureFrame(const int screenIndex);

    WlrootScreenCapture(WlrootScreenCapture& other) = delete;
    void operator=(const WlrootScreenCapture&) = delete;

private:
    WlrootScreenCapture();
    ~WlrootScreenCapture();

    wl_display* _display{nullptr};
    void* _lastData{nullptr};
    wl_buffer* _lastBuffer{nullptr};
};

} // namespace wayland
