#include <QGuiApplication>
#include <QScreen>
#include <QThread>

#include "wayland/wlroots_screen_capture.hpp"

int main(int argc, char* argv[]) {
    QGuiApplication a(argc, argv);

    for (auto screenIndex = 1; screenIndex <= QGuiApplication::screens().size(); ++screenIndex) {
        for (auto i = 0; i < 3; ++i) {
            auto image = wayland::WlrootScreenCapture::instance().captureFrame(screenIndex);
            image.save(QString{"/tmp/wayland-screenshot_"} + QString::number(i) + ".screen"
                       + QString::number(screenIndex) + ".png");
            QThread::sleep(1);
        }
    }

    return 0;
}
