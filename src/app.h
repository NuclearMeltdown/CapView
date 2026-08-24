#pragma once

// Ties everything together: window, message loop, capture graph, audio engine,
// renderer and UI.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/mic_capture.h"
#include "capture/device_config.h"
#include "capture/video_capture.h"
#include "common.h"
#include "config.h"
#include "record/recorder.h"
#include "render/d3d_context.h"
#include "render/video_renderer.h"
#include "ui/overlay.h"
#include "ui/settings_host.h"
#include "update/updater.h"
#include "vcam/virtual_camera.h"
#include "ui/settings_window.h"
#include "ui/toolbar.h"

namespace cap {

// Holds captured frames back by a fixed time, used when the A/V offset asks for
// the picture to wait for the sound. Only allocated while that is the case,
// because it costs both memory and exactly the latency it introduces.
class FrameDelayLine {
 public:
  void Configure(double delayMs);
  void Clear();
  bool active() const { return delayMs_ > 0.0; }
  double delayMs() const { return delayMs_; }

  void Push(const FrameView& frame, int64_t qpc);
  // Returns the newest frame that is already old enough, dropping older ones.
  bool Pop(FrameView* out, int64_t qpc);

 private:
  struct Entry {
    std::vector<uint8_t> data;
    int64_t qpc = 0;
    uint64_t sequence = 0;
  };
  std::deque<Entry> queue_;
  std::vector<Entry> pool_;
  Entry current_;
  double delayMs_ = 0.0;
};

class App {
 public:
  App() = default;
  ~App();

  bool Initialize(HINSTANCE instance, int showCmd);
  int Run();
  void Shutdown();

  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

 private:
  enum class CaptureState { Idle, Running, Reconnecting, NeedsSetup };

  bool CreateMainWindow(HINSTANCE instance, int showCmd);
  bool InitImGui();
  void ShutdownImGui();

  void Tick();
  void RenderFrame();
  void DrawUi();
  void DrawContextMenu();

  bool StartCapture(std::string* error);
  void StopCapture();
  void StartAudio();
  void RestartAll(bool userRequested);

  void OpenSettings(const std::string& reason);

  // Live settings: the dialog edits config_ directly, and this notices what
  // changed and does whatever that change requires -- restart the graph, re-route
  // the crossbar, retheme, or nothing at all.
  void SyncConfigChanges();
  void CaptureAppliedState();
  void MaybeSaveConfig();

  void SwitchProfile(int index);
  void ToggleFullscreen();
  void SetFullscreen(bool on);
  void ApplyWindowFlags();
  void ApplyTheme();
  void AdjustVolume(float delta);
  void ToggleMute();
  void ShowVolumeOsd();

  // Test encodes take seconds and must never run on the UI thread -- doing so
  // froze the window long enough to drop displayed frames.
  void StartEncoderProbe(bool full);
  // Graphics hardware plus ffmpeg build. A cached encoder test only counts when
  // this still matches.
  std::string EncoderSignature() const;
  void LoadCachedEncoders();
  void SaveCachedEncoders();
  void CollectEncoderProbe();

  void ToggleRecording();
  // Grabs the next rendered frame and writes it to disk.
  void RequestScreenshot() { screenshotPending_ = true; }
  void WriteScreenshot();
  void DrawToolbarStrip();
  void OpenFolderInExplorer(std::string* configured, const std::wstring& fallback);
  // Starts or stops the microphone to match the settings and what is going on.
  // `aboutToRecord` starts it for a recording that has not begun yet -- the
  // sample rate has to be known before ffmpeg is given its command line.
  void SyncMicrophone(bool aboutToRecord = false);
  // What SyncMicrophone last acted on, so a failure is reported once instead of
  // once per frame. Cleared when the settings change.
  DeviceRef micApplied_;
  bool micAttempted_ = false;
  bool micFailed_ = false;
  // Whether the deinterlacer should run at all. Only interesting when
  // "interlaced sources only" is ticked, and then it is the measurement that
  // decides, not the media type.
  bool SourceLooksInterlaced(const Profile& profile) const;

  // Finds the analogue video standard by watching whether the decoder locks.
  // Runs only when the source is set to automatic.
  void UpdateVideoStandard();
  // Whether the decoder has a signal, asked of the driver a few times a second
  // rather than once a frame. Each call crosses into the driver, and doing that
  // twice per frame cost over ten milliseconds of every one -- more than the
  // entire video pipeline, and enough to stop the deinterlacer being shown at
  // its proper rate.
  int PollSignalLocked();
  // Asking the driver for the signal state takes about ten milliseconds -- it is
  // a round trip into kernel mode -- which is most of a field period and, on the
  // render thread, a visible hitch in the deinterlacer. So a small thread of its
  // own does the asking and leaves the answer where the render thread can pick
  // it up for free.
  void StartSignalWatch();
  void StopSignalWatch();

  // Draws the settings into their own window when that is switched on.
  // Returns true when it took care of them, so the in-picture panel is skipped.
  // True while the settings live in a window of their own, whether or not that
  // window happens to be visible.
  bool settingsAreWindowed() const { return config_.app.settingsSeparateWindow; }
  // The settings window's own frame. Called *after* the main window has been
  // presented, never inside its frame -- see the comment at the call site.
  void DrawSettingsWindowed();
  // The one-off notice when the check made at startup finds something. Shown in
  // the picture, because a tab nobody opened is not a notice.
  void DrawUpdatePrompt();

  void OpenDeviceConfig();
  void DetectCrop();

  void BeginCropPick();
  void EndCropPick(bool apply);
  void DrawCropPicker();
 public:
  bool cropPickActive() const { return cropPick_.active; }
 private:
  // Makes sure an output folder exists. Recreates it when it was deleted, and
  // falls back to the default when even that fails.
  std::wstring ResolveOutputFolder(std::string* configured, const std::wstring& fallback);
  void StartRecording();
  void StopRecording();
  // Hands the readback frame to the recorder, if one is running.
  void FeedRecorder();
  // One place decides whether the GPU has to hand pictures back, and one place
  // hands them out -- the recorder and the camera both want the same frame, and
  // fetching it twice would give each of them every second one.
  void FeedFrameConsumers();
  void UpdateVirtualCamera();
  // Decides what curve the picture is in and what the screen should be given.
  // Runs every frame because both ends can change underneath it: a console
  // switches to HDR, or the window is dragged onto another screen.
  void UpdateHdr();
  // Opens the release page for a tag in the browser. Empty tag opens the list.
  void OpenReleasePage(const std::string& tag);

  void Toast(const std::string& text);
  void UpdatePowerRequest();
  void SaveWindowPlacement();
  void SaveConfig();

  bool HandleKeyDown(WPARAM key);

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  bool running_ = false;
  bool minimized_ = false;
  bool fullscreen_ = false;
  WINDOWPLACEMENT windowedPlacement_ = {};
  bool imguiReady_ = false;
  bool darkMode_ = true;
  float uiScale_ = 1.0f;

  Config config_;
  D3DContext d3d_;
  VideoRenderer renderer_;
  VideoCapture capture_;
  AudioEngine audio_;
  MicCapture mic_;
  SettingsWindow settings_;
  FrameDelayLine delayLine_;
  Recorder recorder_;
  // Located once at startup; encoder tests run on probeThread_ and are handed
  // over through probeResult_ once probeDone_ flips.
  FfmpegInfo ffmpeg_;
  FfmpegInfo probeResult_;
  std::thread probeThread_;
  std::atomic<bool> probing_{false};
  std::atomic<bool> probeDone_{false};

  CaptureState captureState_ = CaptureState::Idle;
  std::string captureError_;
  int64_t nextRetryQpc_ = 0;
  int retryCount_ = 0;

  // Bob deinterlacing presents each captured frame twice, once per field.
  bool secondFieldPending_ = false;
  int64_t secondFieldQpc_ = 0;
  int fieldIndex_ = 0;

  // Present rate measurement.
  int64_t fpsWindowQpc_ = 0;
  int presentCount_ = 0;
  double presentFps_ = 0.0;
  double lastFrameAgeMs_ = 0.0;
  int statsLogCounter_ = 0;

  // Diagnostics for "the card starts but nothing shows up".
  bool sawFirstFrame_ = false;
  int64_t captureStartQpc_ = 0;

  // Snapshot of the settings that actually drive something, so a live edit can
  // be told apart from a harmless one.
  struct AppliedState {
    int activeProfile = -1;
    long videoStandard = 0;
    DeviceRef video;
    FormatSel format;
    int crossbarInput = -1;
    AudioSource audioSource = AudioSource::Embedded;
    DeviceRef audioIn;
    DeviceRef audioOut;
    bool exclusive = false;
    int bufferMs = 0;
    int avOffsetMs = 0;
    float volume = 1.0f;
    bool mute = false;
    Theme theme = Theme::Dark;
    unsigned accent = 0;
    bool alwaysOnTop = false;
    Language language = Language::German;
  };
  AppliedState applied_;

  // Saving is throttled: dragging a slider must not write the file every frame.
  std::string lastSerialized_;
  bool configDirty_ = false;
  double lastConfigChange_ = 0.0;
  double lastSerializeCheck_ = 0.0;

  bool screenshotPending_ = false;
  // Whether the bar was drawn this frame; the picture layout follows it.
  bool toolbarVisible_ = false;

  // Dragging the crop edges on the picture instead of typing four numbers.
  struct CropPick {
    bool active = false;
    ImageSettings saved;              // restored on cancel
    int left = 0, right = 0, top = 0, bottom = 0;  // source pixels
    int drag = -1;                    // 0 left, 1 right, 2 top, 3 bottom
  } cropPick_;
  // Filled from the renderer each frame; the settings dialog reads it.
  const char* detectedRangeText_ = nullptr;
  const char* detectedInterlaceText_ = nullptr;
  DevicePropertyPages devicePages_;
  SettingsHost settingsHost_;
  Updater updater_;
  VirtualCamera virtualCamera_;
  bool virtualCameraMismatchSaid_ = false;
  bool cameraHdrOffered_ = false;
  // What woke the main loop last time round, and when it last drew. Together
  // they keep the preview paced by the picture rather than by the message
  // queue -- see the comment at the call to RenderFrame.
  unsigned long lastWait_ = 0x00000102ul;  // WAIT_TIMEOUT
  bool lastWaitHadEvent_ = false;
  int64_t lastRenderQpc_ = 0;
  int hdrDisplayPoll_ = 0;
  bool updatePromptQueued_ = false;   // waiting to be opened
  bool updatePromptRaised_ = false;   // already shown once this session
  bool devicePagesWereBusy_ = false;
  // Guards the frame drawn from inside a window drag against re-entering itself.
  bool inModalFrame_ = false;
  // How often each field actually reached the screen. Equal counts mean the
  // deinterlacer is being shown at the rate it is designed for; a shortfall on
  // the second one is the picture juddering.
  uint64_t fieldsShown_[2] = {0, 0};
  // Smoothed estimate of when the next frame is due. The card does not deliver
  // on a metronome, and pacing the fields off each raw arrival hands that
  // wobble straight to the viewer.
  int64_t framePhaseQpc_ = 0;

  // Automatic video standard. Only the timestamps live here; what the card can
  // do is asked of the card each time, because it is the card that knows.
  int standardCandidate_ = -1;      // index into the candidate list, -1 = not searching
  int64_t standardLostQpc_ = 0;     // when the lock was first missing
  int64_t standardNextTryQpc_ = 0;  // not before this
  int standardSweeps_ = 0;          // completed passes through the list without a lock
  std::thread signalWatch_;
  std::atomic<bool> signalWatchRun_{false};
  std::atomic<int> signalLocked_{-1};
  // Counts up every time the watcher stores a reading. The automatic search
  // notes it down when it changes the standard and then ignores anything older:
  // otherwise it judges the new standard by a measurement taken before it was
  // set, and settles on whichever one happened to be tried when a stale "locked"
  // came through.
  std::atomic<uint32_t> signalSeq_{0};
  uint32_t standardSeqAtSet_ = 0;
  std::string toastText_;
  double toastStart_ = 0.0;
  double volumeOsdStart_ = -1000.0;
  double lastSplitCheck_ = 0.0;
  // lParam of the key message being handled, for telling a real key press apart
  // from a synthesised one in the log.
  uint64_t lastKeyLParam_ = 0;

  DWORD lastPowerPokeTick_ = 0;
  bool cursorHidden_ = false;
  int64_t lastMouseMoveQpc_ = 0;
  POINT lastMousePos_ = {};
};

}  // namespace cap
