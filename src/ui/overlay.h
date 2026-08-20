#pragma once

// On-screen text drawn over the video: the statistics panel, the status card
// shown when there is nothing to display, the volume readout and short-lived
// toasts.

#include <string>

#include "audio/audio_engine.h"
#include "capture/dshow_util.h"
#include "capture/frame_sink.h"
#include "common.h"
#include "config.h"

namespace cap {

struct OverlayStats {
  std::string profileName;
  std::string deviceName;
  std::string inputName;
  VideoFormatInfo format;
  SinkStats sink;
  AudioStats audio;
  double presentFps = 0.0;
  double frameAgeMs = 0.0;  // how old the displayed frame was when it went out
  bool vsync = false;
  bool tearing = false;
  bool deinterlacing = false;
  int displayWidth = 0;
  int displayHeight = 0;
  const char* filterName = "";
  int videoDelayMs = 0;
};

void DrawStatsPanel(const OverlayStats& stats);

// Big centred card, used for "no device", "no signal" and error states.
// `spinner` adds an animated dot row to show that a retry is running.
void DrawStatusCard(const std::string& title, const std::string& detail, bool spinner);

// Fades out over its lifetime; `age` and `duration` are in seconds.
void DrawToast(const std::string& text, double age, double duration);

// Volume readout with a bar, parked in the chosen corner. Shown for a moment
// after every change so the level is visible without opening anything.
void DrawVolumeOsd(float volume, bool muted, OsdCorner corner, double age, double duration);

// Recording indicator: a pulsing dot and the elapsed time, in the corner
// opposite the volume readout so the two never overlap.
void DrawRecordIndicator(double seconds, OsdCorner volumeCorner);

}  // namespace cap
