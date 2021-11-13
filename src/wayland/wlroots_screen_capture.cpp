#include "wlroots_screen_capture.hpp"

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <wayland-client-protocol.h>

#include <QVector>

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>

namespace {

struct WlOutput {
    wl_output* output = nullptr;
    zxdg_output_v1* zxdg_output = nullptr;
    std::string name;
    std::string description;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct WlBuffer {
    wl_buffer* wl_buffer = nullptr;
    void* data = nullptr;
    wl_shm_format format = WL_SHM_FORMAT_Y0L0; // default doesn't matter
    int width = 0;
    int height = 0;
    int stride = 0;
    bool y_invert = false;
};

WlBuffer buffer;
QVector<WlOutput> available_outputs;

bool useDamage = false;
bool bufferCopyDone = false;

WlOutput* chosenOutput = nullptr;
wl_shm* shm = nullptr;
zxdg_output_manager_v1* xdg_output_manager = nullptr;
zwlr_screencopy_manager_v1* screencopy_manager = nullptr;

} // namespace

static void handle_xdg_output_logical_position(void*,
                                               zxdg_output_v1* zxdg_output,
                                               int32_t x,
                                               int32_t y) {
    for (auto& wo : available_outputs) {
        if (wo.zxdg_output == zxdg_output) {
            wo.x = x;
            wo.y = y;
        }
    }
}

static void handle_xdg_output_logical_size(void*,
                                           zxdg_output_v1* zxdg_output,
                                           int32_t w,
                                           int32_t h) {
    for (auto& wo : available_outputs) {
        if (wo.zxdg_output == zxdg_output) {
            wo.width = w;
            wo.height = h;
        }
    }
}

static void handle_xdg_output_done(void*, zxdg_output_v1*) {}

static void handle_xdg_output_name(void*, zxdg_output_v1* zxdg_output_v1, const char* name) {
    for (auto& wo : available_outputs) {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.name = name;
    }
}

static void handle_xdg_output_description(void*,
                                          zxdg_output_v1* zxdg_output_v1,
                                          const char* description) {
    for (auto& wo : available_outputs) {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.description = description;
    }
}

const zxdg_output_v1_listener xdg_output_implementation = {handle_xdg_output_logical_position,
                                                           handle_xdg_output_logical_size,
                                                           handle_xdg_output_done,
                                                           handle_xdg_output_name,
                                                           handle_xdg_output_description};

static wl_buffer* create_shm_buffer(wl_shm_format fmt,
                                    int width,
                                    int height,
                                    int stride,
                                    void** data_out) {
    const auto size = stride * height;
    const auto shm_name = QByteArray{"/wlroots-screencopy"};

    const auto fd = shm_open(shm_name.data(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);

    if (fd < 0) {
        fprintf(stderr, "shm_open failed\n");
        return nullptr;
    }
    shm_unlink(shm_name);

    int ret{0};
    while ((ret = ftruncate(fd, size)) == EINTR) {
        // No-op
    }

    if (ret < 0) {
        close(fd);
        fprintf(stderr, "ftruncate failed\n");
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

    auto* pool = wl_shm_create_pool(shm, fd, size);
    close(fd);

    auto* pool_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
    wl_shm_pool_destroy(pool);

    *data_out = data;
    return pool_buffer;
}

static void frame_handle_buffer(void* data,
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

    buffer.wl_buffer = create_shm_buffer(static_cast<wl_shm_format>(format),
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

static void frame_handle_flags(void*, struct zwlr_screencopy_frame_v1*, uint32_t flags) {
    buffer.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_handle_ready(void*,
                               struct zwlr_screencopy_frame_v1*,
                               uint32_t,
                               uint32_t,
                               uint32_t) {
    bufferCopyDone = true;
}

static void frame_handle_failed(void*, struct zwlr_screencopy_frame_v1*) {
    fprintf(stderr, "failed to copy frame\n");
    exit(EXIT_FAILURE);
}

static void frame_handle_damage(void*,
                                struct zwlr_screencopy_frame_v1*,
                                uint32_t,
                                uint32_t,
                                uint32_t,
                                uint32_t) {}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    frame_handle_buffer,
    frame_handle_flags,
    frame_handle_ready,
    frame_handle_failed,
    frame_handle_damage,
};

static void handle_global(void*,
                          struct wl_registry* registry,
                          uint32_t name,
                          const char* interface,
                          uint32_t) {
    if (strcmp(interface, wl_output_interface.name) == 0) {
        auto output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, 1));
        WlOutput wlOutput;
        wlOutput.output = output;
        available_outputs.push_back(wlOutput);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        screencopy_manager = static_cast<zwlr_screencopy_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 2));
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        xdg_output_manager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry,
                             name,
                             &zxdg_output_manager_v1_interface,
                             2)); // version 2 for name & description, if available
    }
}

static void handle_global_remove(void*, struct wl_registry*, uint32_t) {
    // Who cares?
}

static const struct wl_registry_listener registry_listener = {
    handle_global,
    handle_global_remove,
};

static void load_output_info() {
    for (auto& wo : available_outputs) {
        wo.zxdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, wo.output);
        zxdg_output_v1_add_listener(wo.zxdg_output, &xdg_output_implementation, nullptr);
    }
}

static WlOutput* choose_interactive() {
    fprintf(stdout, "Please select an output from the list to capture (enter output no.):\n");

    int i = 1;
    for (auto& wo : available_outputs) {
        printf("%d. Name: %s Description: %s\n", i++, wo.name.c_str(), wo.description.c_str());
        std::cout << "name: " << wo.name << ", description: " << wo.description;
    }

    printf("Enter output no.:");
    fflush(stdout);

    int choice;
    if (scanf("%d", &choice) != 1 || choice > static_cast<int>(available_outputs.size())
        || choice <= 0)
        return nullptr;

    return &available_outputs[choice - 1];
}

namespace wayland {

QImage WlrootScreenCapture::captureFrame(const int screenIndex) {
    chosenOutput = &available_outputs[screenIndex - 1];

    auto* frame = zwlr_screencopy_manager_v1_capture_output(screencopy_manager,
                                                            0,
                                                            chosenOutput->output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, nullptr);

    while (!bufferCopyDone && wl_display_dispatch(_display) != -1) {
        // This space is intentionally left blank
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
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(_display);

    load_output_info();

    // if (available_outputs.size() == 1) {
    //     chosenOutput = &available_outputs[0];
    // } else {
    //     chosenOutput = choose_interactive();
    // }
    //
    // if (chosenOutput == nullptr) {
    //     fprintf(stderr, "Failed to select output\n");
    //     return;
    // }

    if (shm == nullptr) {
        fprintf(stderr, "compositor is missing wl_shm\n");
        return;
    }

    if (screencopy_manager == nullptr) {
        fprintf(stderr, "compositor doesn't support wlr-screencopy-unstable-v1\n");
        return;
    }

    if (xdg_output_manager == nullptr) {
        fprintf(stderr, "compositor doesn't support xdg-output-unstable-v1\n");
        return;
    }

    if (available_outputs.empty()) {
        fprintf(stderr, "no outputs available\n");
        return;
    }
}

WlrootScreenCapture::~WlrootScreenCapture() {
    wl_buffer_destroy(_lastBuffer);
    munmap(_lastData,
           static_cast<std::size_t>(buffer.stride) * static_cast<std::size_t>(buffer.height));
}
} // namespace wayland
