#pragma once

// Owns the DirectShow graph: capture filter -> FrameSink. Nothing else is in
// the graph, and the graph runs without a reference clock so samples are
// delivered the moment the driver produces them instead of being scheduled.

#include <dshow.h>

#include <string>
#include <vector>

#include "capture/dshow_util.h"
#include "capture/frame_sink.h"
#include "common.h"
#include "config.h"

namespace cap {

// What a device can do, gathered without committing to it. Used by the settings
// window to fill the format dropdowns and the input list.
struct DeviceProbeResult {
  bool ok = false;
  std::string error;
  VideoDeviceInfo device;
  CapsModel caps;
  std::vector<CrossbarInput> crossbarInputs;
  // Colour description read off the pin's current media type, so the diagnostics
  // can show whether the driver describes its output or leaves it to guesswork.
  VideoFormatInfo colorInfo;
};

class VideoCapture {
 public:
  VideoCapture() = default;
  ~VideoCapture();

  VideoCapture(const VideoCapture&) = delete;
  VideoCapture& operator=(const VideoCapture&) = delete;

  // Builds and runs the graph. On failure `error` holds a message meant for the
  // user and the object stays stopped.
  bool Start(const CaptureSettings& settings, std::string* error);
  void Stop();

  bool running() const { return control_ != nullptr; }

  FrameSink* sink() { return sink_.Get(); }
  const FrameSink* sink() const { return sink_.Get(); }

  VideoFormatInfo format() const;

  // Filled during Start, so the settings window can show the live device's
  // capabilities without probing it a second time (the card is busy then).
  const DeviceProbeResult& capabilities() const { return capabilities_; }

  // Actual device identity that got opened, for writing a refreshed id back to
  // the config after a match by name.
  const VideoDeviceInfo& resolvedDevice() const { return capabilities_.device; }

  // Opens a device briefly to read its capabilities. Do not call this for the
  // device that is currently running -- use capabilities() instead.
  static DeviceProbeResult Probe(const DeviceRef& device);

  // Drains the graph event queue. Returns true when the graph reported a fatal
  // condition (device lost, abort); `message` then describes it.
  bool PumpEvents(std::string* message);

  // Switches the crossbar input on a running graph, no restart needed.
  bool SetCrossbarInput(int index);

 private:
  void Teardown();

  ComPtr<IGraphBuilder> graph_;
  ComPtr<ICaptureGraphBuilder2> builder_;
  ComPtr<IMediaControl> control_;
  ComPtr<IMediaEventEx> events_;
  ComPtr<IBaseFilter> captureFilter_;
  ComPtr<IPin> capturePin_;
  ComPtr<FrameSink> sink_;

  DeviceProbeResult capabilities_;
};

}  // namespace cap
