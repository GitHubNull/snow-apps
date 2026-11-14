#ifndef SNOW_SHOT_PRESENTATION_SELECTOR_LINUXSELECTORPOLICY_H
#define SNOW_SHOT_PRESENTATION_SELECTOR_LINUXSELECTORPOLICY_H

#include "snow_shot/platform/linux/x11windowquery.h"

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>

#include <cstdint>
#include <optional>

namespace snow_shot::presentation::selector {

// Picks the topmost eligible window under a physical desktop point.
// `screenIndex` restricts entries to one X screen (-1 accepts every screen);
// `rootOffset` translates that screen's root coordinates into the virtual
// desktop used by the capture canvas.
[[nodiscard]] std::optional<snow_shot::platform::x11::WindowTarget>
pickWindowAtPoint(const QPoint& physicalPoint,
                  const QVector<snow_shot::platform::x11::WindowStackEntry>& topDown,
                  const QVector<std::uintptr_t>& excluded, quint32 excludedProcessId,
                  int screenIndex, const QPoint& rootOffset);

// Maps a physical rectangle into a display image. Returns an empty rectangle
// when the inputs do not intersect or the display geometry is unusable.
[[nodiscard]] QRect imageRectForPhysicalRect(const QRect& displayPhysicalRect,
                                             const QSize& imageSize, const QRect& physicalRect);

// Inverse of imageRectForPhysicalRect.
[[nodiscard]] QRect physicalRectForImageRect(const QRect& displayPhysicalRect,
                                             const QSize& imageSize, const QRect& imageRect);

// Selects the smallest detected rectangle containing the point. Returns
// nullopt when no detected rectangle contains it.
[[nodiscard]] std::optional<QRect> pickElementAtPoint(const QVector<QRect>& detectedPhysicalRects,
                                                      const QPoint& physicalPoint,
                                                      const QRect& windowRect);

} // namespace snow_shot::presentation::selector

#endif // SNOW_SHOT_PRESENTATION_SELECTOR_LINUXSELECTORPOLICY_H
