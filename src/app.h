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
#include "capture/video_capture.h"
#include "common.h"
#include "config.h"
#include "record/recorder.h"
#include "render/d3d_context.h"
#include "render/video_renderer.h"
#include "ui/overlay.h"
#include "ui/settings_window.h"

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
  // Starts or stops the microphone to match the settings and what is going on.
  // `aboutToRecord` starts it for a recording that has not begun yet -- the
  // sample rate has to be known before ffmpeg is given its command line.
  void SyncMicrophone(bool aboutToRecord = false);
  // What SyncMicrophone last acted on, so a failure is reported once instead of
  // once per frame. Cleared when the settings change.
  DeviceRef micApplied_;
  bool micAttempted_ = false;
  bool micFailed_ = false;
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

  // Dragging the crop edges on the picture instead of typing four numbers.
  struct CropPick {
    bool active = false;
    ImageSettings saved;              // restored on cancel
    int left = 0, right = 0, top = 0, bottom = 0;  // source pixels
    int drag = -1;                    // 0 left, 1 right, 2 top, 3 bottom
  } cropPick_;
  // Filled from the renderer each frame; the settings dialog reads it.
  const char* detectedRangeText_ = nullptr;
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
