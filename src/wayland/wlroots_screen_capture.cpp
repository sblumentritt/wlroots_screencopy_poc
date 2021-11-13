#include "wlroots_screen_capture.hpp"

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <wayland-client-protocol.h>

#include <QUuid>
#include <QVector>

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>

namespace {

// -----------------------------------------------------------------------
// data structures
// -----------------------------------------------------------------------
struct Output {
    wl_output* output = nullptr;
    zxdg_output_v1* zxdg_output = nullptr;
    std::string name;
    std::string description;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct Buffer {
    wl_buffer* wl_buffer = nullptr;
    void* data = nullptr;
    wl_shm_format format = static_cast<wl_shm_format>(0); // default doesn't matter
    int width = 0;
    int height = 0;
    int stride = 0;
    bool yInvert = false;
};

// -----------------------------------------------------------------------
// 'global' variables
// -----------------------------------------------------------------------
bool useDamage = false;
bool bufferCopyDone = false;

Buffer buffer;
QVector<Output> availableOutputs;

wl_shm* sharedMemory = nullptr;
zxdg_output_manager_v1* xdgOutputManager = nullptr;
zwlr_screencopy_manager_v1* screencopyManager = nullptr;

// -----------------------------------------------------------------------
// xdg-output related functions
// -----------------------------------------------------------------------
void handleXdgOutputLogicalPosition(void*, zxdg_output_v1* zxdg_output, int32_t x, int32_t y) {
    for (auto& output : availableOutputs) {
        if (output.zxdg_output == zxdg_output) {
            output.x = x;
            output.y = y;
        }
    }
}

void handleXdgOutputLogicalSize(void*, zxdg_output_v1* zxdg_output, int32_t w, int32_t h) {
    for (auto& output : availableOutputs) {
        if (output.zxdg_output == zxdg_output) {
            output.width = w;
            output.height = h;
        }
    }
}

void handleXdgOutputDone(void*, zxdg_output_v1*) {}

void handleXdgOutputName(void*, zxdg_output_v1* zxdg_output_v1, const char* name) {
    for (auto& output : availableOutputs) {
        if (output.zxdg_output == zxdg_output_v1)
            output.name = name;
    }
}

void handleXdgOutputDescription(void*, zxdg_output_v1* zxdg_output_v1, const char* description) {
    for (auto& output : availableOutputs) {
        if (output.zxdg_output == zxdg_output_v1)
            output.description = description;
    }
}

// -----------------------------------------------------------------------
// wlr-screencopy/frame related functions
// -----------------------------------------------------------------------
wl_buffer* createSharedMemoryBuffer(wl_shm_format fmt,
                                    int width,
                                    int height,
                                    int stride,
                                    void** dataOut) {
    const auto size = stride * height;
    const auto sharedMemoryName = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);

    const auto fd = shm_open(sharedMemoryName.data(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);

    if (fd < 0) {
        std::cerr << "shm_open failed\n";
        return nullptr;
    }
    shm_unlink(sharedMemoryName);

    int ret{0};
    while ((ret = ftruncate(fd, size)) == EINTR) {
        // No-op
    }

    if (ret < 0) {
        close(fd);
        std::cerr << "ftruncate failed\n";
        return nullptr;
    }

    auto* data = mmap(nullptr,
                      static_cast<std::size_t>(size),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED,
                      fd,
                      0);

    if (data == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return nullptr;
    }

    auto* pool = wl_shm_create_pool(sharedMemory, fd, size);
    close(fd);

    auto* poolBuffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
    wl_shm_pool_destroy(pool);

    *dataOut = data;
    return poolBuffer;
}

void frameHandleBuffer(void* data,
                       zwlr_screencopy_frame_v1* frame,
                       uint32_t format,
                       uint32_t width,
                       uint32_t height,
                       uint32_t stride) {
    Q_UNUSED(data)

    buffer.format = static_cast<wl_shm_format>(format);
    buffer.width = static_cast<int>(width);
    buffer.height = static_cast<int>(height);
    buffer.stride = static_cast<int>(stride);

    // Make sure the buffer is not allocated
    assert(!buffer.wl_buffer);

    buffer.wl_buffer = createSharedMemoryBuffer(static_cast<wl_shm_format>(format),
                                                static_cast<int>(width),
                                                static_cast<int>(height),
                                                static_cast<int>(stride),
                                                &buffer.data);

    if (buffer.wl_buffer == nullptr) {
        fprintf(stderr, "failed to create buffer\n");
        exit(EXIT_FAILURE);
    }
    if (useDamage) {
        zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.wl_buffer);
    } else {
        zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
    }
}

void frameHandleFlags(void*, struct zwlr_screencopy_frame_v1*, uint32_t flags) {
    buffer.yInvert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

void frameHandleReady(void*, struct zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {
    bufferCopyDone = true;
}

void frameHandleFailed(void*, struct zwlr_screencopy_frame_v1*) {
    std::cerr << "failed to copy frame\n";
    exit(EXIT_FAILURE);
}

void frameHandleDamage(void*,
                       struct zwlr_screencopy_frame_v1*,
                       uint32_t,
                       uint32_t,
                       uint32_t,
                       uint32_t) {}

// -----------------------------------------------------------------------
// registry listener related functions
// -----------------------------------------------------------------------
void handleGlobal(void*,
                  struct wl_registry* registry,
                  uint32_t name,
                  const char* interface,
                  uint32_t) {
    if (strcmp(interface, wl_output_interface.name) == 0) {
        auto output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, 1));
        Output wlOutput;
        wlOutput.output = output;
        availableOutputs.push_back(wlOutput);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        sharedMemory = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        screencopyManager = static_cast<zwlr_screencopy_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 2));
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        xdgOutputManager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry,
                             name,
                             &zxdg_output_manager_v1_interface,
                             2)); // version 2 for name & description, if available
    }
}

void handleGlobalRemove(void*, struct wl_registry*, uint32_t) {}

// -----------------------------------------------------------------------
// listener variables
// -----------------------------------------------------------------------
const zxdg_output_v1_listener xdgOutputListener = {handleXdgOutputLogicalPosition,
                                                   handleXdgOutputLogicalSize,
                                                   handleXdgOutputDone,
                                                   handleXdgOutputName,
                                                   handleXdgOutputDescription};

const struct zwlr_screencopy_frame_v1_listener frameListener = {frameHandleBuffer,
                                                                frameHandleFlags,
                                                                frameHandleReady,
                                                                frameHandleFailed,
                                                                frameHandleDamage,
                                                                nullptr,
                                                                nullptr};

const struct wl_registry_listener registryListener = {handleGlobal, handleGlobalRemove};

} // namespace

namespace wayland {

QImage WlrootScreenCapture::captureFrame(const int screenIndex) {
    auto* requestedOutput = &availableOutputs[screenIndex - 1];
    auto* frame = zwlr_screencopy_manager_v1_capture_output(screencopyManager,
                                                            0,
                                                            requestedOutput->output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frameListener, nullptr);

    while (!bufferCopyDone && wl_display_dispatch(_display) != -1) {
        // intentionally left blank
    }

    auto image = QImage{static_cast<uchar*>(buffer.data),
                        buffer.width,
                        buffer.height,
                        QImage::Format_RGB32};

    _lastBuffer = buffer.wl_buffer;
    _lastData = buffer.data;

    bufferCopyDone = false;
    buffer.data = nullptr;
    buffer.wl_buffer = nullptr;

    return image;
}

WlrootScreenCapture::WlrootScreenCapture() {
    _display = wl_display_connect(nullptr);
    if (_display == nullptr) {
        perror("failed to create display");
        return;
    }

    auto* registry = wl_display_get_registry(_display);
    wl_registry_add_listener(registry, &registryListener, nullptr);

    syncWayland();

    if (!hasProtocolSupport()) {
        return;
    }

    loadOutputDetails();
}

WlrootScreenCapture::~WlrootScreenCapture() {
    wl_buffer_destroy(_lastBuffer);
    munmap(_lastData,
           static_cast<std::size_t>(buffer.stride) * static_cast<std::size_t>(buffer.height));
}

bool WlrootScreenCapture::hasProtocolSupport() {
    if (sharedMemory == nullptr) {
        std::cerr << "compositor is missing wl_shm\n";
        return false;
    }

    if (screencopyManager == nullptr) {
        std::cerr << "compositor doesn't support wlr-screencopy-unstable-v1\n";
        return false;
    }

    if (xdgOutputManager == nullptr) {
        std::cerr << "compositor doesn't support xdg-output-unstable-v1\n";
        return false;
    }

    if (availableOutputs.empty()) {
        std::cerr << "no outputs available\n";
        return false;
    }

    return true;
}

void WlrootScreenCapture::syncWayland() {
    wl_display_dispatch(_display);
    wl_display_roundtrip(_display);
}

void WlrootScreenCapture::loadOutputDetails() {
    for (auto& wo : availableOutputs) {
        wo.zxdg_output = zxdg_output_manager_v1_get_xdg_output(xdgOutputManager, wo.output);
        zxdg_output_v1_add_listener(wo.zxdg_output, &xdgOutputListener, nullptr);
    }

    syncWayland();
}

} // namespace wayland
