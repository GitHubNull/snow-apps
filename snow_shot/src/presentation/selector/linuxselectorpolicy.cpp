#include "linuxselectorpolicy.h"

#include <QtMath>

#include <cmath>

namespace snow_shot::presentation::selector {
namespace {

QRect scaledRect(const QRect& source, const QRect& sourceBounds, const QSize& targetSize) {
    if (source.isEmpty() || sourceBounds.isEmpty() || targetSize.isEmpty())
        return {};
    const qreal scaleX = static_cast<qreal>(targetSize.width()) / sourceBounds.width();
    const qreal scaleY = static_cast<qreal>(targetSize.height()) / sourceBounds.height();
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0 || scaleY <= 0)
        return {};
    const int left = qRound((source.x() - sourceBounds.x()) * scaleX);
    const int top = qRound((source.y() - sourceBounds.y()) * scaleY);
    const int right = qRound((source.x() + source.width() - sourceBounds.x()) * scaleX);
    const int bottom = qRound((source.y() + source.height() - sourceBounds.y()) * scaleY);
    const QRect mapped(left, top, right - left, bottom - top);
    return mapped.intersected(QRect(QPoint(0, 0), targetSize));
}

QRect unscaledRect(const QRect& source, const QRect& targetBounds, const QSize& sourceSize) {
    if (source.isEmpty() || targetBounds.isEmpty() || sourceSize.isEmpty())
        return {};
    const qreal scaleX = static_cast<qreal>(targetBounds.width()) / sourceSize.width();
    const qreal scaleY = static_cast<qreal>(targetBounds.height()) / sourceSize.height();
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0 || scaleY <= 0)
        return {};
    const int left = targetBounds.x() + qRound(source.x() * scaleX);
    const int top = targetBounds.y() + qRound(source.y() * scaleY);
    const int right = targetBounds.x() + qRound((source.x() + source.width()) * scaleX);
    const int bottom = targetBounds.y() + qRound((source.y() + source.height()) * scaleY);
    return QRect(left, top, right - left, bottom - top);
}

} // namespace

std::optional<snow_shot::platform::x11::WindowTarget>
pickWindowAtPoint(const QPoint& physicalPoint,
                  const QVector<snow_shot::platform::x11::WindowStackEntry>& topDown,
                  const QVector<std::uintptr_t>& excluded, quint32 excludedProcessId,
                  int screenIndex, const QPoint& rootOffset) {
    for (const auto& entry : topDown) {
        if (!entry.mapped || entry.skip || entry.frameRect.isEmpty())
            continue;
        if (screenIndex >= 0 && entry.screenIndex != screenIndex)
            continue;
        if (excluded.contains(static_cast<std::uintptr_t>(entry.windowId)))
            continue;
        if (excludedProcessId != 0 && entry.processId == excludedProcessId)
            continue;
        const QRect frameRect = entry.frameRect.translated(rootOffset);
        if (!frameRect.contains(physicalPoint))
            continue;
        snow_shot::platform::x11::WindowTarget target;
        target.windowId = entry.windowId;
        target.frameRect = frameRect;
        target.title = entry.title;
        target.processId = entry.processId;
        return target;
    }
    return std::nullopt;
}

QRect imageRectForPhysicalRect(const QRect& displayPhysicalRect, const QSize& imageSize,
                               const QRect& physicalRect) {
    if (displayPhysicalRect.isEmpty() || imageSize.isEmpty() || physicalRect.isEmpty())
        return {};
    const QRect intersect = physicalRect.intersected(displayPhysicalRect);
    if (intersect.isEmpty())
        return {};
    return scaledRect(intersect, displayPhysicalRect, imageSize);
}

QRect physicalRectForImageRect(const QRect& displayPhysicalRect, const QSize& imageSize,
                               const QRect& imageRect) {
    if (displayPhysicalRect.isEmpty() || imageSize.isEmpty() || imageRect.isEmpty())
        return {};
    const QRect intersect = imageRect.intersected(QRect(QPoint(0, 0), imageSize));
    if (intersect.isEmpty())
        return {};
    return unscaledRect(intersect, displayPhysicalRect, imageSize);
}

std::optional<QRect> pickElementAtPoint(const QVector<QRect>& detectedPhysicalRects,
                                        const QPoint& physicalPoint, const QRect& windowRect) {
    std::optional<QRect> best;
    qsizetype bestArea = 0;
    for (const QRect& rect : detectedPhysicalRects) {
        if (rect.isEmpty() || !rect.contains(physicalPoint))
            continue;
        if (!windowRect.isEmpty() && !windowRect.intersects(rect))
            continue;
        const qsizetype area = static_cast<qsizetype>(rect.width()) * rect.height();
        if (!best || area < bestArea) {
            best = rect;
            bestArea = area;
        }
    }
    return best;
}

} // namespace snow_shot::presentation::selector
