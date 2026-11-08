#pragma once

#include "snow_shot/presentation/directcaptureworkflow.h"

namespace snow_shot::presentation {

// Qt/X11 implementation of direct capture for the snow_capture-less Linux
// build. Runs on the direct-capture worker thread; screen grabs are marshaled
// to the GUI thread internally.
[[nodiscard]] DirectCaptureFrame captureDirectTargetQt(const DirectCaptureRequest& request);

} // namespace snow_shot::presentation
