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
  // What is actually running. Not simply the setting: on a source whose fields
  // are co-sited every mode does the same thing, and saying "YADIF" there would
  // be describing a code path nobody took.
  std::string deinterlaceLabel;
  int displayWidth = 0;
  int displayHeight = 0;
  const char* filterName = "";
  int videoDelayMs = 0;
  StatsDetail detail = StatsDetail::Full;
  // What the automatic colour handling settled on; empty when it is still
  // measuring or the user picked the values by hand.
  std::string colorInfo;
};

void DrawStatsPanel(const OverlayStats& stats);

// The empty state: the icon at size, the name under it, and one line saying why
// there is nothing to show. `icon` is an ImTextureID -- passed as the underlying
// integer so this header does not have to pull in ImGui -- and may be zero, in
// which case only the text is drawn.
//
// Separate from DrawStatusCard because it means something different. The card
// interrupts a picture; this replaces one that was never there.
void DrawIdleScreen(unsigned long long icon, int iconPixels, const std::string& detail);

// Big centred card, used for "no device", "no signal" and error states.
// `spinner` adds an animated dot row to show that a retry is running.
void DrawStatusCard(const std::string& title, const std::string& detail, bool spinner);

// Fades out over its lifetime; `age` and `duration` are in seconds.
void DrawToast(const std::string& text, double age, double duration);

// Standard search, shown on the picture itself while it runs.
//
// It used to live only in the settings dialog, and that is the one place where
// nobody is looking: the search runs right after the source is switched, when
// the eye is on the picture and the dialog is closed. What is visible there
// without this is a picture that changes standard every few seconds for no
// stated reason. Top centre, clear of the statistics panel (top left), the
// recording dot (the corners) and the toasts (bottom centre).
void DrawSearchIndicator(const std::string& text);

// Volume readout with a bar, parked in the chosen corner. Shown for a moment
// after every change so the level is visible without opening anything.
void DrawVolumeOsd(float volume, bool muted, OsdCorner corner, double age, double duration);

// Recording indicator: a pulsing dot and the elapsed time, in the corner
// opposite the volume readout so the two never overlap.
void DrawRecordIndicator(double seconds, OsdCorner volumeCorner);

}  // namespace cap
