#pragma once

// The settings dialog.
//
// It edits the live configuration in place, so every change takes effect on the
// spot -- there is no Apply button and nothing to confirm. A snapshot is taken
// when the dialog opens, which is what "Discard" restores.

#include <string>
#include <vector>

#include "audio/audio_devices.h"
#include "capture/video_capture.h"
#include "common.h"
#include "config.h"
#include "record/ffmpeg_download.h"
#include "record/ffmpeg_locator.h"
#include "record/remuxer.h"

namespace cap {

struct MonitorInfoEntry {
  int index = 0;
  std::string name;
  RECT rect = {};
  bool primary = false;
};

std::vector<MonitorInfoEntry> EnumerateMonitors();

class SettingsWindow {
 public:
  enum class Result { None, Close };

  // `live` must outlive the dialog; it is edited directly. `reason` is shown as
  // a banner, e.g. why the dialog opened by itself.
  void Open(Config* live, const std::string& reason = {});
  void Close();
  // Drops the banner; called once the condition that raised it is resolved.
  void ClearReason() { reason_.clear(); }
  // Reloads the folder text fields from the config, after something outside the
  // dialog changed them.
  void InvalidateFolderFields() { recordBuffersLoaded_ = false; }

  // What the automatic level detection settled on, shown next to the range
  // selector so "Automatic" is not a black box. Points at a static string owned
  // by the caller, or null while nothing has been measured.
  void SetDetectedRange(const char* const* text) { detectedRange_ = text; }
  bool isOpen() const { return open_; }

  // `liveCaps` are the capabilities of the device that is currently running, so
  // the dialog does not have to reopen a busy card. May be null. `ffmpeg` is the
  // app's shared state and is updated in place when the user tests encoders or
  // downloads a build.
  Result Draw(const DeviceProbeResult* liveCaps, FfmpegInfo* ffmpeg);

  // Set by the dialog when the user asks for a full encoder test; the app runs
  // it in the background and clears the flag.
  bool takeProbeRequest();

  // True while the binding editor is waiting for a key press. The app routes
  // key messages here instead of acting on them.
  bool waitingForKey() const { return captureAction_ >= 0; }
  void OfferKey(int vk, bool ctrl, bool shift, bool alt);

  // Forces the device and format lists to be read again on the next frame.
  void InvalidateDeviceLists();

 private:
  void RefreshDeviceLists();
  const DeviceProbeResult& CapsFor(const DeviceRef& device, const DeviceProbeResult* liveCaps);
  void DrawSourceTab(const DeviceProbeResult& caps);
  void DrawImageTab();
  void DrawAudioTab();
  void DrawDisplayTab();
  void DrawRecordTab(FfmpegInfo* ffmpeg);
  void DrawToolsTab(FfmpegInfo* ffmpeg);
  void DrawHotkeysTab();
  // Text field plus Browse / Default / Open, shared by both output folders.
  void FolderRow(const char* id, char* buffer, size_t bufferSize, std::string* value,
                 const std::wstring& defaultFolder);
 public:
  void setProbeBusy(bool busy) { probeBusy_ = busy; }
 private:
  void DrawProfilesTab();
  void EnsureValidFormat(const DeviceProbeResult& caps);

  Config& cfg() { return *live_; }

  bool open_ = false;
  bool centerNext_ = true;
  std::string reason_;
  Config* live_ = nullptr;
  Config snapshot_;  // state when the dialog opened, for Discard

  bool listsValid_ = false;
  std::vector<VideoDeviceInfo> videoDevices_;
  std::vector<AudioDeviceInfo> audioInputs_;
  std::vector<AudioDeviceInfo> audioOutputs_;
  std::vector<MonitorInfoEntry> monitors_;

  // Capabilities of the device the dialog last probed, keyed by its id.
  std::string probedId_;
  DeviceProbeResult probed_;

  // Resolved embedded audio device for the current video device, for display.
  std::string embeddedAudioName_;
  std::string embeddedAudioForDevice_;

  // Custom format entry.
  bool customFormat_ = false;
  int customWidth_ = 1920;
  int customHeight_ = 1080;
  double customFps_ = 60.0;

  char renameBuffer_[64] = {};
  int renameTarget_ = -1;

  // Recording tab.
  FfmpegDownloader downloader_;
  Remuxer remuxer_;
  bool probeRequested_ = false;
  bool probeBusy_ = false;
  char folderBuffer_[512] = {};
  char ffmpegPathBuffer_[512] = {};
  char shotFolderBuffer_[512] = {};
  bool recordBuffersLoaded_ = false;

  // Index into Hotkeys::items currently being rebound, -1 when idle.
  int captureAction_ = -1;
  const char* const* detectedRange_ = nullptr;
};

}  // namespace cap
