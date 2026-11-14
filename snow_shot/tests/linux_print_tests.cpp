#include "../src/platform/linux/nativeprint.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QImage paintedPage(const QSize& size, const QImage& image) {
    QImage page(size, QImage::Format_ARGB32_Premultiplied);
    page.fill(Qt::transparent);
    QPainter painter(&page);
    snow_shot::platform::linux::paintPrintImage(painter, image,
                                                QRectF(QPointF(0, 0), QSizeF(size)));
    painter.end();
    return page;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);

    {
        QImage image(100, 50, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        const QImage page = paintedPage(QSize(300, 800), image);
        require(page.pixelColor(0, 0) == QColor(Qt::white), "the page margin must stay white");
        require(page.pixelColor(150, 400) == QColor(Qt::red),
                "the image must be centered on the page");
        require(page.pixelColor(150, 300) == QColor(Qt::white),
                "content above the fitted image must stay white");
        require(page.pixelColor(150, 500) == QColor(Qt::white),
                "content below the fitted image must stay white");
    }

    {
        QImage image(400, 400, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::green);
        const QImage page = paintedPage(QSize(200, 100), image);
        require(page.pixelColor(0, 0) == QColor(Qt::white),
                "the left margin must stay white for oversized images");
        require(page.pixelColor(100, 50) == QColor(Qt::green),
                "oversized images must be fitted inside the page");
        require(page.pixelColor(145, 95) == QColor(Qt::green),
                "the fitted image must reach the short page edge");
        require(page.pixelColor(199, 99) == QColor(Qt::white),
                "the right margin must stay white for oversized images");
    }

    {
        const QImage page = paintedPage(QSize(200, 100), {});
        require(page.pixelColor(0, 0) == QColor(Qt::white) &&
                    page.pixelColor(199, 99) == QColor(Qt::white),
                "an empty image must paint a blank white page");
    }

    return 0;
}
