#pragma once

#include <cstdint>

namespace cap {
namespace vcam {

// The picture the camera shows while nothing is feeding it: the CapView mark
// over a dark ground, with "CapView is not running" underneath.
//
// This exists because the filter is registered machine-wide and therefore sits
// in every device list from the moment it is installed, whether CapView is
// running or not -- the same as OBS's virtual camera. A camera that is always
// listed and shows black when idle is indistinguishable from a broken one.
//
// Writes a whole NV12 or P010 frame of the given shape into `dst`, which must
// hold width*height*3/2 bytes for NV12 and twice that for P010. Returns false
// when it could not draw at all, leaving `dst` untouched so the caller can fall
// back to black.
bool RenderIdlePicture(uint8_t* dst, uint32_t width, uint32_t height, uint32_t pixel);

}  // namespace vcam
}  // namespace cap
