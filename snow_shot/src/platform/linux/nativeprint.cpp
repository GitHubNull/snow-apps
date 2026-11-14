#include "nativeprint.h"

#include "../../presentation/services/nativeprintbackend.h"
#include "../../presentation/services/screenshotprintdiagnostics.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPainter>
#include <QPointer>
#include <QPrintDialog>
#include <QPrinter>
#include <QWidget>

#include <utility>

namespace snow_shot::platform::linux {

void paintPrintImage(QPainter& painter, const QImage& image, const QRectF& pageRect) {
    painter.fillRect(pageRect, Qt::white);
    if (image.isNull() || pageRect.isEmpty())
        return;
    const QRectF target = ScreenshotPrintService::fittedRect(image.size(), pageRect);
    if (target.isEmpty())
        return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, image);
}

} // namespace snow_shot::platform::linux

namespace {
using Service = ScreenshotPrintService;
using snow_shot::print_detail::logPrintEvent;
using snow_shot::print_detail::printStatusName;

QString printFailureText() {
    return QCoreApplication::translate("ScreenshotPrintService",
                                       "The native print operation failed");
}

void failPrint(const char* stage, const char* reason, Service::Completion& completion) {
    logPrintEvent("print.native_failed",
                  {{QStringLiteral("backend"), QStringLiteral("linux_cups")},
                   {QStringLiteral("stage"), QString::fromLatin1(stage)},
                   {QStringLiteral("reason"), QString::fromLatin1(reason)}},
                  QtWarningMsg);
    completion({Service::Status::Failed, printFailureText()});
}
} // namespace

ScreenshotPrintService::Backend screenshotNativePrintBackend(bool legacy) {
    // Linux has a single CUPS backend; there is no legacy print path to recover.
    if (legacy)
        return {};
    return [](QWidget* owner, QImage image, Service::Completion completion) {
        if (owner == nullptr || image.isNull()) {
            failPrint("prepare", owner == nullptr ? "missing_owner" : "empty_image", completion);
            return;
        }
        QPrinter printer(QPrinter::HighResolution);
        printer.setDocName(QStringLiteral("SnowShot"));
        printer.setColorMode(QPrinter::Color);
        printer.setPageOrientation(image.width() > image.height() ? QPageLayout::Landscape
                                                                  : QPageLayout::Portrait);

        logPrintEvent("print.native_ui_requested",
                      {{QStringLiteral("backend"), QStringLiteral("linux_cups")}});
        const QPointer<QWidget> ownerAlive(owner);
        QPrintDialog dialog(&printer, owner);
        if (dialog.exec() != QDialog::Accepted || !ownerAlive) {
            logPrintEvent(
                "print.native_task_completed",
                {{QStringLiteral("backend"), QStringLiteral("linux_cups")},
                 {QStringLiteral("status"), printStatusName(Service::Status::Cancelled)}});
            completion({Service::Status::Cancelled, {}});
            return;
        }

        QPainter painter;
        if (!painter.begin(&printer)) {
            failPrint("begin", "painter_begin", completion);
            return;
        }
        const QRectF pageRect = printer.pageLayout().paintRectPixels(printer.resolution());
        snow_shot::platform::linux::paintPrintImage(painter, image, pageRect);
        painter.end();

        const QPrinter::PrinterState state = printer.printerState();
        const bool failed = state == QPrinter::Error || state == QPrinter::Aborted;
        const auto status = failed ? Service::Status::Failed : Service::Status::Submitted;
        logPrintEvent("print.native_task_completed",
                      {{QStringLiteral("backend"), QStringLiteral("linux_cups")},
                       {QStringLiteral("status"), printStatusName(status)}},
                      failed ? QtWarningMsg : QtInfoMsg);
        completion({status, failed ? printFailureText() : QString()});
    };
}
