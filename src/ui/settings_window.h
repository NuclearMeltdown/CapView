#pragma once

// The settings dialog.
//
// It edits the live configuration in place, so every change takes effect on the
// spot -- there is no Apply button and nothing to confirm. A snapshot is taken
// when the dialog opens, which is what "Discard" restores.

#include <string>
#include <vector>

struct ImGuiContext;

#include "audio/audio_devices.h"
#include "capture/video_capture.h"
#include "update/updater.h"
#include "common.h"
#include "config.h"
#include "record/ffmpeg_download.h"
#include "record/ffmpeg_locator.h"
#include "record/remuxer.h"
#include "ui/file_dialog.h"

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
  void SetDetectedInterlace(const char* const* text) { detectedInterlace_ = text; }
  void SetCoSitedFields(bool on) { coSitedFields_ = on; }
  // False while the app is itself trying to open the card. Probing means
  // building a second graph on the same device, and doing that while the first
  // one is being retried puts two of them on one card -- which the driver is
  // under no obligation to survive.
  void SetProbeAllowed(bool on) { probeAllowed_ = on; }

  // The updater lives in the app; the dialog only drives it and shows what it
  // reports.
  void SetUpdater(Updater* updater) { updater_ = updater; }
  // Set when the user asked to restart into a freshly installed build.
  bool takeRestartRequest();
  // 1 locked, 0 not, -1 unknown. Shown next to the video standard, because that
  // is the one number that says whether the setting is the right one.
  void SetSignalLocked(int locked) { signalLocked_ = locked; }
  // Whether the running source is being treated as analogue. Decides which of
  // the picture settings are worth showing at all.
  void SetAnalogueSource(bool on) { analogueSource_ = on; }

  // Frame rate the card is currently delivering; caps the recording rate.
  void SetSourceFps(double fps) { sourceFps_ = fps; }

  // Current input levels, 0..1, for the meters on the audio tab.
  void SetLevels(float input, float mic, bool micRunning) {
    inputPeak_ = input;
    micPeak_ = mic;
    micRunning_ = micRunning;
  }
  bool isOpen() const { return open_; }

  // `liveCaps` are the capabilities of the device that is currently running, so
  // the dialog does not have to reopen a busy card. May be null. `ffmpeg` is the
  // app's shared state and is updated in place when the user tests encoders or
  // downloads a build.
  Result Draw(const DeviceProbeResult* liveCaps, FfmpegInfo* ffmpeg);

  // Draws the same contents filling the whole viewport, with no frame of its
  // own -- for when the dialog *is* the window rather than a panel inside one.
  void SetFillsWindow(bool on) { fillsWindow_ = on; }

  // Set by the dialog when the user asks for a full encoder test; the app runs
  // it in the background and clears the flag.
  bool takeProbeRequest();
  // Set when the user wants to drag the crop on the picture; the app closes the
  // dialog and takes over.
  bool takeCropPickRequest();
  // Set when the user asks for the capture driver's own settings dialog.
  bool takeDeviceConfigRequest();
  // Set when the user wants the black border measured and cropped away.
  bool takeCropDetectRequest();

  // The virtual camera. 0 = nothing wanted, 1 = install the source, 2 = remove
  // it. Both raise a UAC prompt, so the app does it rather than the UI thread.
  int takeVirtualCameraRequest();
  // What the app knows and the settings cannot see for themselves.
  void SetVirtualCameraState(bool running, bool consumed, int width, int height, int fps,
                             bool sourceOutdated);
  void SetCarrierPeriod(float samples) { carrierPeriod_ = samples > 1.5f ? samples : 3.045f; }

  void SetHdrState(bool displayCapable, bool outputActive, float displayPeak, int sourceTransfer) {
    hdrDisplayCapable_ = displayCapable;
    hdrOutputActive_ = outputActive;
    hdrDisplayPeak_ = displayPeak;
    hdrSourceTransfer_ = sourceTransfer;
  }

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
  void DrawHdrBlock();
  void DrawRecordTab(FfmpegInfo* ffmpeg);
  void DrawFfmpegBlock(FfmpegInfo* ffmpeg);
  void DrawToolsTab(FfmpegInfo* ffmpeg);
  void DrawVirtualCameraBlock();
  void DrawEncoderBlock(const EncoderInfo* encoder);
  void DrawHotkeysTab();
  void DrawUpdatesTab();
  // Text field plus Browse / Default / Open, shared by both output folders.
  void FolderRow(const char* id, int pickTag, char* buffer, size_t bufferSize,
                 std::string* value, const std::wstring& defaultFolder);
  // Applies a finished file dialog to whichever field opened it.
  void PollFileDialog(FfmpegInfo* ffmpeg);
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

  // One dialog at a time, tagged so the result finds its way back. The tags are
  // PickTarget values.
  AsyncFileDialog picker_;
  enum PickTarget { kPickNone = 0, kPickRecordFolder, kPickShotFolder, kPickFfmpeg, kPickRemux };
  bool probeRequested_ = false;
  bool cropPickRequested_ = false;
  bool probeBusy_ = false;
  char folderBuffer_[512] = {};
  char ffmpegPathBuffer_[512] = {};
  char shotFolderBuffer_[512] = {};
  bool recordBuffersLoaded_ = false;

  // Index into Hotkeys::items currently being rebound, -1 when idle.
  int captureAction_ = -1;
  const char* const* detectedRange_ = nullptr;
  const char* const* detectedInterlace_ = nullptr;
  bool fillsWindow_ = false;
  bool probeAllowed_ = true;
  Updater* updater_ = nullptr;
  bool restartRequested_ = false;
  bool coSitedFields_ = false;
  int signalLocked_ = -1;
  bool analogueSource_ = true;
  bool captureRunning_ = false;
  bool deviceConfigRequested_ = false;
  bool cropDetectRequested_ = false;
  int virtualCameraRequest_ = 0;
  int vcamStatus_ = 0;
  double vcamStatusChecked_ = -10.0;
  bool vcamRunning_ = false;
  bool vcamConsumed_ = false;
  bool vcamOutdated_ = false;
  // What the dot crawl slider currently amounts to. Passed in because it
  // depends on the video standard and the captured width, neither of which the
  // settings know.
  float carrierPeriod_ = 3.045f;
  // Which tab is open, carried across the two ImGui contexts by hand.
  ImGuiContext* tabContext_ = nullptr;
  int activeTab_ = 0;
  int wantTab_ = -1;
  // What the app knows about the screen and the source; the settings cannot
  // ask DXGI themselves.
  bool hdrDisplayCapable_ = false;
  bool hdrOutputActive_ = false;
  float hdrDisplayPeak_ = 100.0f;
  int hdrSourceTransfer_ = 0;  // 0 SDR, 1 PQ, 2 HLG
  int vcamWidth_ = 0;
  int vcamHeight_ = 0;
  int vcamFps_ = 0;
  double sourceFps_ = 0.0;
  float inputPeak_ = 0.0f;
  float micPeak_ = 0.0f;
  bool micRunning_ = false;
};

}  // namespace cap
