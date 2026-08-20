#pragma once

// Writes a single frame to disk as PNG or JPEG.
//
// This goes through WIC, which ships with Windows, rather than through ffmpeg.
// A screenshot is the one output that should work on a fresh machine where
// nobody has pressed the download button yet.

#include <cstdint>
#include <string>

#include "common.h"
#include "config.h"

namespace cap {

// Saves tightly packed pixels in VideoRenderer::kReadbackPixelFormat order.
// Returns false and fills `error` on failure.
bool SaveScreenshot(const std::wstring& path, const uint8_t* pixels, int width, int height,
                    ScreenshotFormat format, int jpegQuality, std::string* error);

// Timestamped name in `folder`, with a counter when the same second is hit
// twice. Creates the folder. Returns empty when the folder cannot be made.
std::wstring MakeScreenshotPath(const std::wstring& folder, ScreenshotFormat format);

// Pictures\CapView.
std::wstring DefaultScreenshotFolder();

}  // namespace cap
