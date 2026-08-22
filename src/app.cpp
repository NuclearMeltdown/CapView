#include "app.h"

#include <shellapi.h>   // ShellExecuteW
#include <windowsx.h>  // GET_X_LPARAM

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "i18n.h"
#include "imgui.h"
#include "record/ffmpeg_locator.h"
#include "render/d3d_context.h"
#include "record/screenshot.h"
#include "resource.h"
#include "ui/theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace cap {
namespace {

const wchar_t kWindowClass[] = L"CapViewMainWindow";
const wchar_t kWindowTitle[] = L"CapView";

// How long without a frame before we call it "no signal".
const double kNoSignalSeconds = 1.5;
// Delay between reconnect attempts, and the slower pace once it is clear the
// device is not coming back on its own.
const double kRetrySeconds = 2.0;
const double kRetrySlowSeconds = 10.0;
const int kRetryBackoffAfter = 5;
// Hide the pointer after this much stillness in fullscreen.
const double kCursorIdleSeconds = 2.0;
// How long the volume readout stays on screen after a change.
const double kVolumeOsdSeconds = 1.6;
// One wheel notch.
const float kVolumeStep = 0.05f;

int64_t QpcNow() {
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double QpcFreq() {
  static const double f = [] {
    LARGE_INTEGER v;
    ::QueryPerformanceFrequency(&v);
    return (double)v.QuadPart;
  }();
  return f;
}

int64_t SecondsToQpc(double seconds) {
  return (int64_t)(seconds * QpcFreq());
}

double QpcToSeconds(int64_t ticks) {
  return (double)ticks / QpcFreq();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  App* app = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = (CREATESTRUCTW*)lparam;
    app = (App*)cs->lpCreateParams;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
  } else {
    app = (App*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }
  if (app) return app->HandleMessage(hwnd, msg, wparam, lparam);
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

// ------------------------------------------------------------ FrameDelayLine

void FrameDelayLine::Configure(double delayMs) {
  if (delayMs <= 0.0) {
    delayMs_ = 0.0;
    Clear();
    return;
  }
  if (std::abs(delayMs - delayMs_) > 0.5) Clear();
  delayMs_ = delayMs;
}

void FrameDelayLine::Clear() {
  queue_.clear();
  pool_.clear();
  current_ = Entry{};
}

void FrameDelayLine::Push(const FrameView& frame, int64_t qpc) {
  if (!active() || !frame.valid()) return;
  Entry e;
  if (!pool_.empty()) {
    e = std::move(pool_.back());
    pool_.pop_back();
  }
  e.data.assign(frame.data, frame.data + frame.size);
  e.qpc = qpc;
  e.sequence = frame.sequence;
  queue_.push_back(std::move(e));

  // A pathological setting must not eat all memory.
  while (queue_.size() > 64) queue_.pop_front();
}

bool FrameDelayLine::Pop(FrameView* out, int64_t qpc) {
  if (!active()) return false;
  const int64_t threshold = qpc - SecondsToQpc(delayMs_ / 1000.0);
  bool got = false;
  while (!queue_.empty() && queue_.front().qpc <= threshold) {
    if (!current_.data.empty()) pool_.push_back(std::move(current_));
    current_ = std::move(queue_.front());
    queue_.pop_front();
    got = true;
  }
  if (!got || current_.data.empty()) return false;
  if (out) {
    out->data = current_.data.data();
    out->size = current_.data.size();
    out->sequence = current_.sequence;
  }
  return true;
}

// ----------------------------------------------------------------------- App

App::~App() {
  Shutdown();
}

bool App::Initialize(HINSTANCE instance, int showCmd) {
  instance_ = instance;

  std::string configError;
  config_.Load(&configError);
  LogInit(config_.app.logToFile);
  CAP_LOG("CapView startet");
  if (!configError.empty()) CAP_WARN("%s", configError.c_str());

  SetLanguage(config_.app.language);
  darkMode_ = ResolveDark(config_.app.theme);
  lastSerialized_ = config_.Serialize();

  if (!CreateMainWindow(instance, showCmd)) return false;

  std::string error;
  if (!d3d_.Initialize(hwnd_, &error)) {
    ::MessageBoxW(nullptr, ToWide(error).c_str(), L"CapView", MB_ICONERROR | MB_OK);
    return false;
  }
  if (!InitImGui()) return false;
  if (!renderer_.Initialize(&d3d_, &error)) {
    ::MessageBoxW(nullptr, ToWide(error).c_str(), L"CapView", MB_ICONERROR | MB_OK);
    return false;
  }

  if (config_.app.startFullscreen) SetFullscreen(true);

  // Straight into the picture if we can; into the settings if we cannot.
  if (config_.active().capture.video.empty()) {
    OpenSettings(T("Noch kein Aufnahmegerät ausgewählt. Wähle unten die Capture-Karte aus.",
                   "No capture device selected yet. Pick your capture card below."));
  } else if (!StartCapture(&error)) {
    OpenSettings(error);
  }
  CaptureAppliedState();
  ffmpeg_ = LocateFfmpeg(config_.record.ffmpegPath);
  // No test on startup. Which encoders work does not change from one run to the
  // next, so the answer is loaded from the config; testing is something the user
  // asks for once, or when the machine changed underneath it.
  LoadCachedEncoders();

  // A previous update left its predecessor lying next to us; it can go now that
  // nothing is running from it.
  Updater::CleanUpPreviousBuild();
  if (config_.app.checkUpdatesOnStart) updater_.CheckAsync();

  running_ = true;
  return true;
}

void App::Shutdown() {
  if (hwnd_) SaveWindowPlacement();
  // Before anything else: closing the pipes is what makes ffmpeg finalise the
  // container, and that has to happen while the app is still alive.
  StopRecording();
  if (probeThread_.joinable()) probeThread_.join();
  StopSignalWatch();
  settingsHost_.Destroy();
  StopCapture();
  mic_.Stop();
  audio_.Stop();
  renderer_.Shutdown();
  ShutdownImGui();
  d3d_.Shutdown();
  if (hwnd_) {
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  ::SetThreadExecutionState(ES_CONTINUOUS);
}

// -------------------------------------------------------------------- window

bool App::CreateMainWindow(HINSTANCE instance, int showCmd) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // we paint every pixel ourselves
  wc.lpszClassName = kWindowClass;
  // Large icon for Alt+Tab, small one for the title bar and taskbar.
  wc.hIcon = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_CAPVIEW), IMAGE_ICON,
                                 ::GetSystemMetrics(SM_CXICON),
                                 ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
  wc.hIconSm = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_CAPVIEW), IMAGE_ICON,
                                   ::GetSystemMetrics(SM_CXSMICON),
                                   ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  if (!::RegisterClassExW(&wc)) {
    ::MessageBoxW(nullptr, L"Fensterklasse konnte nicht registriert werden.", L"CapView",
                  MB_ICONERROR);
    return false;
  }

  RECT rc = {0, 0, config_.app.windowW, config_.app.windowH};
  ::AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  const int x = config_.app.windowX >= 0 ? config_.app.windowX : CW_USEDEFAULT;
  const int y = config_.app.windowY >= 0 ? config_.app.windowY : CW_USEDEFAULT;

  hwnd_ = ::CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width,
                            height, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    ::MessageBoxW(nullptr, L"Fenster konnte nicht erstellt werden.", L"CapView", MB_ICONERROR);
    return false;
  }

  ApplyWindowDarkMode(hwnd_, darkMode_);
  ::ShowWindow(hwnd_, config_.app.maximized ? SW_SHOWMAXIMIZED : showCmd);
  ::UpdateWindow(hwnd_);
  ApplyWindowFlags();
  return true;
}

void App::SaveWindowPlacement() {
  if (!hwnd_ || fullscreen_) return;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!::GetWindowPlacement(hwnd_, &wp)) return;
  config_.app.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);

  RECT rc = wp.rcNormalPosition;
  RECT frame = {0, 0, 0, 0};
  ::AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  config_.app.windowX = rc.left;
  config_.app.windowY = rc.top;
  config_.app.windowW = std::max(160L, (rc.right - rc.left) - (frame.right - frame.left));
  config_.app.windowH = std::max(120L, (rc.bottom - rc.top) - (frame.bottom - frame.top));
}

void App::ApplyWindowFlags() {
  if (!hwnd_) return;
  // The driver's dialog is a top-level window of its own with no owner, which is
  // what keeps it out of our message loop -- and also means nothing lifts it
  // above a window that insists on staying on top. So while it is up, we do not.
  const bool top = config_.app.alwaysOnTop && !devicePages_.busy();
  ::SetWindowPos(hwnd_, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::SetFullscreen(bool on) {
  if (!hwnd_ || on == fullscreen_) return;

  if (on) {
    windowedPlacement_.length = sizeof(windowedPlacement_);
    ::GetWindowPlacement(hwnd_, &windowedPlacement_);
    SaveWindowPlacement();

    // Which monitor: the configured one, otherwise the one the window is on.
    RECT target = {};
    bool haveTarget = false;
    if (config_.app.fullscreenMonitor >= 0) {
      std::vector<MonitorInfoEntry> monitors = EnumerateMonitors();
      if (config_.app.fullscreenMonitor < (int)monitors.size()) {
        target = monitors[(size_t)config_.app.fullscreenMonitor].rect;
        haveTarget = true;
      }
    }
    if (!haveTarget) {
      HMONITOR mon = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi = {};
      mi.cbSize = sizeof(mi);
      if (::GetMonitorInfoW(mon, &mi)) {
        target = mi.rcMonitor;
        haveTarget = true;
      }
    }
    if (!haveTarget) return;

    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    ::SetWindowPos(hwnd_, config_.app.alwaysOnTop ? HWND_TOPMOST : HWND_TOP, target.left,
                   target.top, target.right - target.left, target.bottom - target.top,
                   SWP_FRAMECHANGED | SWP_NOACTIVATE);
    fullscreen_ = true;
  } else {
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    ::SetWindowPlacement(hwnd_, &windowedPlacement_);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    fullscreen_ = false;
    if (cursorHidden_) {
      ::ShowCursor(TRUE);
      cursorHidden_ = false;
    }
    ApplyWindowFlags();
  }
  d3d_.Resize();
}

void App::ToggleFullscreen() {
  SetFullscreen(!fullscreen_);
}

// --------------------------------------------------------------------- ImGui

bool App::InitImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini next to the exe
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  const UINT dpi = ::GetDpiForWindow(hwnd_);
  const float scale = dpi > 0 ? (float)dpi / 96.0f : 1.0f;
  LoadUiFont(17.0f * scale);
  ApplyImGuiTheme(darkMode_, config_.app.accentColor);
  ImGui::GetStyle().ScaleAllSizes(scale);
  uiScale_ = scale;

  if (!ImGui_ImplWin32_Init(hwnd_)) return false;
  if (!ImGui_ImplDX11_Init(d3d_.device(), d3d_.context())) return false;
  imguiReady_ = true;
  return true;
}

void App::ShutdownImGui() {
  if (!imguiReady_) return;
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  imguiReady_ = false;
}

void App::ApplyTheme() {
  const bool dark = ResolveDark(config_.app.theme);
  darkMode_ = dark;
  // ApplyImGuiTheme resets the style wholesale, so the DPI scaling has to be
  // reapplied on top of it every time.
  ApplyImGuiTheme(dark, config_.app.accentColor);
  ImGui::GetStyle().ScaleAllSizes(uiScale_);
  ApplyWindowDarkMode(hwnd_, dark);
  if (settingsHost_.created()) settingsHost_.ApplyTheme(dark, config_.app.accentColor);
}

// ------------------------------------------------------------------- capture

bool App::StartCapture(std::string* error) {
  StopCapture();

  const Profile& p = config_.active();
  std::string err;
  if (!capture_.Start(p.capture, &err)) {
    captureState_ = CaptureState::Reconnecting;
    captureError_ = err;
    nextRetryQpc_ = QpcNow() + SecondsToQpc(kRetrySeconds);
    if (error) *error = err;
    return false;
  }

  // Write back the device identity we actually opened, so a card that moved
  // slots keeps working on the next start.
  const VideoDeviceInfo& resolved = capture_.resolvedDevice();
  if (!resolved.id.empty()) {
    config_.active().capture.video.id = resolved.id;
    config_.active().capture.video.name = resolved.name;
  }

  std::string rendererError;
  renderer_.SetSourceFormat(capture_.format(), &rendererError);
  if (!rendererError.empty()) CAP_WARN("%s", rendererError.c_str());

  captureState_ = CaptureState::Running;
  captureError_.clear();
  retryCount_ = 0;
  secondFieldPending_ = false;
  sawFirstFrame_ = false;
  captureStartQpc_ = QpcNow();

  // The dot crawl filter works on the colour subcarrier, so it has to be told
  // which one this signal carries. The card knows, when it has an analogue
  // decoder at all; without one the default covers the common case.
  const DeviceProbeResult& caps = capture_.capabilities();
  const long standard = caps.availableStandards != 0 ? caps.currentStandard : 0;
  renderer_.SetCarrierSamples(VideoStandardSubcarrierSamples(standard));

  StartAudio();
  UpdatePowerRequest();
  StartSignalWatch();
  return true;
}

void App::StopCapture() {
  StopSignalWatch();
  capture_.Stop();
  renderer_.DropFrame();
  delayLine_.Clear();
  captureState_ = CaptureState::Idle;
}

void App::StartAudio() {
  audio_.Stop();
  const Profile& p = config_.active();

  DeviceRef input;
  switch (p.capture.audioSource) {
    case AudioSource::None: return;
    case AudioSource::Manual: input = p.capture.audio; break;
    case AudioSource::Embedded:
    default: {
      AudioDeviceInfo found;
      if (FindEmbeddedAudioDevice(capture_.resolvedDevice(), &found)) {
        input = found.ToRef();
      } else {
        CAP_WARN("Kein eingebettetes Audiogerät gefunden, Ton bleibt aus");
        return;
      }
      break;
    }
  }

  std::string err;
  if (!audio_.Start(input, p.audio, &err)) {
    CAP_WARN("Ton konnte nicht gestartet werden: %s", err.c_str());
    Toast("Ton konnte nicht gestartet werden: " + err);
  }
  delayLine_.Configure(std::max(0, -p.audio.avOffsetMs));
}

void App::RestartAll(bool userRequested) {
  std::string error;
  if (StartCapture(&error)) {
    if (userRequested) Toast("Aufnahme neu gestartet");
  } else if (userRequested) {
    Toast(error);
  }
}

// ------------------------------------------------------------------ settings

void App::OpenSettings(const std::string& reason) {
  settings_.Open(&config_, reason);
}

void App::CaptureAppliedState() {
  const Profile& p = config_.active();
  applied_.activeProfile = config_.activeProfile;
  applied_.video = p.capture.video;
  applied_.format = p.capture.format;
  applied_.crossbarInput = p.capture.crossbarInput;
  applied_.videoStandard = p.capture.videoStandard;
  applied_.audioSource = p.capture.audioSource;
  applied_.audioIn = p.capture.audio;
  applied_.audioOut = p.audio.output;
  applied_.exclusive = p.audio.exclusive;
  applied_.bufferMs = p.audio.bufferMs;
  applied_.avOffsetMs = p.audio.avOffsetMs;
  applied_.volume = p.audio.volume;
  applied_.mute = p.audio.mute;
  applied_.theme = config_.app.theme;
  applied_.accent = config_.app.accentColor;
  applied_.alwaysOnTop = config_.app.alwaysOnTop;
  applied_.language = config_.app.language;
}

void App::SyncConfigChanges() {
  const Profile& p = config_.active();

  if (config_.app.language != applied_.language) {
    SetLanguage(config_.app.language);
  }
  if (config_.app.theme != applied_.theme || config_.app.accentColor != applied_.accent) {
    ApplyTheme();
  }
  if (config_.app.alwaysOnTop != applied_.alwaysOnTop ||
      devicePages_.busy() != devicePagesWereBusy_) {
    devicePagesWereBusy_ = devicePages_.busy();
    ApplyWindowFlags();
  }

  // Anything that changes what the card is asked to produce needs the graph
  // rebuilt. Everything else is applied without interrupting the picture.
  const bool profileChanged = config_.activeProfile != applied_.activeProfile;
  const bool deviceChanged = !(p.capture.video == applied_.video);
  const bool formatChanged = !p.capture.format.SameFormat(applied_.format) ||
                             std::abs(p.capture.format.fps - applied_.format.fps) > 0.01;
  // A different standard usually means a different number of lines, so the graph
  // has to come up again around it. Automatic is the exception: it has nothing
  // to apply until it has found something, and rebuilds by itself when it does.
  const bool standardChanged = p.capture.videoStandard != applied_.videoStandard &&
                               p.capture.videoStandard > 0;
  if (standardChanged) {
    CAP_LOG("Videonorm gewechselt: %s -> %s, Graph wird neu aufgebaut",
            VideoStandardSettingName(applied_.videoStandard).c_str(),
            VideoStandardSettingName(p.capture.videoStandard).c_str());
  }

  if (profileChanged || deviceChanged || formatChanged || standardChanged) {
    std::string error;
    if (!StartCapture(&error)) {
      // Keep the dialog open on the failing setting instead of closing over it.
      if (!settings_.isOpen()) OpenSettings(error);
      Toast(error);
    }
    CaptureAppliedState();
    UpdatePowerRequest();
    return;
  }

  if (p.capture.crossbarInput != applied_.crossbarInput) {
    capture_.SetCrossbarInput(p.capture.crossbarInput);
    // A different input is a different signal on the same format: levels, field
    // structure and where the picture sits all have to be measured again.
    renderer_.ResetAnalysis();
  }

  const bool audioRouteChanged = p.capture.audioSource != applied_.audioSource ||
                                 !(p.capture.audio == applied_.audioIn) ||
                                 !(p.audio.output == applied_.audioOut) ||
                                 p.audio.exclusive != applied_.exclusive;
  if (audioRouteChanged) {
    StartAudio();
  } else if (p.audio.bufferMs != applied_.bufferMs || p.audio.avOffsetMs != applied_.avOffsetMs ||
             p.audio.volume != applied_.volume || p.audio.mute != applied_.mute) {
    audio_.ApplySettings(p.audio);
    delayLine_.Configure(std::max(0, -p.audio.avOffsetMs));
  }

  CaptureAppliedState();
}

void App::MaybeSaveConfig() {
  const double now = ImGui::GetTime();

  // Serialising four times a second is cheap and means no field can be
  // forgotten here when one gets added to the config.
  if (now - lastSerializeCheck_ >= 0.25) {
    lastSerializeCheck_ = now;
    std::string current = config_.Serialize();
    if (current != lastSerialized_) {
      lastSerialized_ = std::move(current);
      configDirty_ = true;
      lastConfigChange_ = now;
    }
  }

  // Wait for the user to stop fiddling before touching the disk.
  if (configDirty_ && now - lastConfigChange_ >= 1.0) {
    configDirty_ = false;
    std::string error;
    if (!config_.Save(&error)) CAP_ERR("%s", error.c_str());
  }
}

void App::SaveConfig() {
  SaveWindowPlacement();
  std::string error;
  if (!config_.Save(&error)) {
    CAP_ERR("%s", error.c_str());
    Toast(error);
  }
}

void App::SwitchProfile(int index) {
  if (index < 0 || index >= (int)config_.profiles.size()) return;
  if (index == config_.activeProfile) return;
  config_.SetActiveProfile(index);
  std::string error;
  if (StartCapture(&error)) {
    Toast(Format(T("Profil %d: %s", "Profile %d: %s"), index + 1, config_.active().name.c_str()));
  } else {
    Toast(error);
  }
  CaptureAppliedState();
  SaveConfig();
}

// --------------------------------------------------------------------- misc

void App::Toast(const std::string& text) {
  toastText_ = text;
  toastStart_ = ImGui::GetTime();
}

void App::UpdatePowerRequest() {
  const DWORD now = ::GetTickCount();
  if (now - lastPowerPokeTick_ < 30000) return;
  lastPowerPokeTick_ = now;
  if (config_.app.preventSleep && captureState_ == CaptureState::Running) {
    ::SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
  } else {
    ::SetThreadExecutionState(ES_CONTINUOUS);
  }
}

void App::AdjustVolume(float delta) {
  AudioSettings& a = config_.active().audio;
  a.volume = Clamp(a.volume + delta, 0.0f, 1.0f);
  a.mute = false;
  audio_.ApplySettings(a);
  applied_.volume = a.volume;
  applied_.mute = a.mute;
  ShowVolumeOsd();
}

void App::ToggleMute() {
  AudioSettings& a = config_.active().audio;
  a.mute = !a.mute;
  audio_.ApplySettings(a);
  applied_.mute = a.mute;
  ShowVolumeOsd();
}

// ------------------------------------------------------------------ recording

void App::StartEncoderProbe(bool full) {
  if (probing_.load(std::memory_order_relaxed) || !ffmpeg_.found) return;
  if (probeThread_.joinable()) probeThread_.join();

  probeDone_.store(false, std::memory_order_relaxed);
  probing_.store(true, std::memory_order_relaxed);
  // The thread works on its own copy and publishes it in one go, so the UI
  // thread never sees a half filled structure.
  FfmpegInfo copy = ffmpeg_;
  probeThread_ = std::thread([this, copy, full]() mutable {
    ComScope com(COINIT_MULTITHREADED);
    if (full) {
      ProbeEncoders(&copy);
    } else {
      EnsureUsableEncoder(&copy, config_.record.encoder);
    }
    probeResult_ = std::move(copy);
    probing_.store(false, std::memory_order_relaxed);
    probeDone_.store(true, std::memory_order_release);
  });
}

void App::CollectEncoderProbe() {
  if (!probeDone_.load(std::memory_order_acquire)) return;
  probeDone_.store(false, std::memory_order_relaxed);
  if (probeThread_.joinable()) probeThread_.join();
  ffmpeg_ = std::move(probeResult_);
  SaveCachedEncoders();
}

std::string App::EncoderSignature() const {
  // The ffmpeg build belongs in here as well as the hardware: a different build
  // can be missing an encoder the last one had, and would then be judged by a
  // result it never produced.
  return GraphicsAdapterSignature() + " | " + ffmpeg_.version;
}

void App::LoadCachedEncoders() {
  if (!ffmpeg_.found) return;
  RecordSettings& rec = config_.record;
  if (rec.encodersAvailable.empty() && rec.encoderProbeSignature.empty()) return;

  if (rec.encoderProbeSignature != EncoderSignature()) {
    CAP_LOG("Encoder-Test verworfen: Hardware oder ffmpeg hat sich geändert");
    rec.encoderProbeSignature.clear();
    rec.encodersAvailable.clear();
    return;
  }

  ApplyCachedProbe(&ffmpeg_, rec.encodersAvailable);
  CAP_LOG("Encoder aus der Konfiguration übernommen: %zu verwendbar",
          rec.encodersAvailable.size());
}

void App::SaveCachedEncoders() {
  if (!ffmpeg_.found || !ffmpeg_.tested) return;
  RecordSettings& rec = config_.record;
  rec.encodersAvailable.clear();
  for (const EncoderInfo& e : ffmpeg_.encoders) {
    if (e.tested && e.available) rec.encodersAvailable.push_back((int)e.id);
  }
  rec.encoderProbeSignature = EncoderSignature();

  // A selection that did not survive the test would silently fail at F9, so it
  // goes back to Auto rather than staying as a trap.
  const EncoderInfo* chosen = ffmpeg_.Find(rec.encoder);
  if (!IsAutoEncoder(rec.encoder) && (!chosen || !chosen->available)) {
    CAP_WARN("Gewählter Encoder ist nicht verfügbar, zurück auf Automatisch");
    rec.encoder = RecordEncoder::Auto;
    Toast(T("Der gewählte Encoder funktioniert hier nicht, zurück auf Automatisch.",
            "The selected encoder does not work here, back to Automatic."));
  }
}

void App::ToggleRecording() {
  if (recorder_.recording()) {
    StopRecording();
  } else {
    StartRecording();
  }
}

void App::StartRecording() {
  if (recorder_.recording()) return;

  // The path was resolved at startup and could have gone stale since -- a
  // portable build on a stick that got unplugged, an antivirus quarantine, or
  // simply someone tidying up. Checking costs one file attribute lookup and
  // turns a confusing "ffmpeg exited with code 1" into an honest answer.
  if (ffmpeg_.found &&
      ::GetFileAttributesW(ToWide(ffmpeg_.path).c_str()) == INVALID_FILE_ATTRIBUTES) {
    CAP_WARN("ffmpeg ist verschwunden: %s", ffmpeg_.path.c_str());
    ffmpeg_ = FfmpegInfo{};
  }
  if (!ffmpeg_.found) {
    ffmpeg_ = LocateFfmpeg(config_.record.ffmpegPath);
    settings_.InvalidateFolderFields();
  }
  if (!ffmpeg_.found) {
    Toast(T("ffmpeg fehlt — in den Einstellungen unter Aufnahme holen.",
            "ffmpeg is missing — fetch it under Recording in the settings."));
    return;
  }
  if (captureState_ != CaptureState::Running || !renderer_.hasFrame()) {
    Toast(T("Kein Bild zum Aufnehmen.", "No picture to record."));
    return;
  }

  if (probing_.load(std::memory_order_relaxed)) {
    Toast(T("Encoder werden noch geprüft, gleich nochmal.",
            "Still testing encoders, try again in a moment."));
    return;
  }
  const EncoderInfo* usable = ffmpeg_.Resolve(config_.record.encoder);
  if (!usable || !usable->available) {
    // Nothing tested yet: kick a full probe off in the background rather than
    // freezing the window here, and let the user press again. Full, so the
    // settings end up with the complete list rather than the first answer that
    // happened to work.
    StartEncoderProbe(true);
    Toast(T("Encoder werden geprüft, gleich nochmal.",
            "Testing encoders, try again in a moment."));
    return;
  }

  // The tap has to exist before the recorder pulls from it, and the readback
  // before the first frame is fetched.
  const bool wantAudio = audio_.running() && audio_.tapSampleRate() > 0;
  if (wantAudio) audio_.SetTapEnabled(true);
  renderer_.SetReadbackEnabled(true);

  // Start the microphone before asking it for its rate: the device decides that,
  // and ffmpeg has to be told it on the command line.
  SyncMicrophone(true);
  const bool wantMic = mic_.running() && mic_.sampleRate() > 0;
  if (wantMic) mic_.ResetBuffer();

  // Resolve the folder before handing it over, so a deleted directory is
  // recreated -- or answered with the default -- instead of failing the start.
  RecordSettings settings = config_.record;
  const std::wstring folder =
      ResolveOutputFolder(&config_.record.outputFolder, DefaultRecordFolder());
  if (folder.empty()) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }
  settings.outputFolder = ToUtf8(folder);

  const VideoFormatInfo format = renderer_.sourceFormat();
  std::string error;
  Recorder::AudioSource mainTrack;
  if (wantAudio) {
    mainTrack.sampleRate = audio_.tapSampleRate();
    mainTrack.pull = [this](float* out, size_t frames) { return audio_.ReadTap(out, frames); };
  }
  Recorder::AudioSource micTrack;
  if (wantMic) {
    micTrack.sampleRate = mic_.sampleRate();
    micTrack.pull = [this](float* out, size_t frames) { return mic_.Read(out, frames); };
  }

  const bool ok = recorder_.Start(settings, ffmpeg_, renderer_.outputWidth(),
                                  renderer_.outputHeight(), format.fps, mainTrack, micTrack,
                                  config_.active().audio.micTrackMode, &error);

  if (!ok) {
    renderer_.SetReadbackEnabled(false);
    audio_.SetTapEnabled(false);
    Toast(error);
    return;
  }
  Toast(T("Aufnahme läuft", "Recording"));
}

void App::StopRecording() {
  if (!recorder_.recording()) return;
  const RecordStats stats = recorder_.stats();
  recorder_.Stop();
  renderer_.SetReadbackEnabled(false);
  audio_.SetTapEnabled(false);
  Toast(Format(T("Aufnahme gespeichert (%.0f s)", "Recording saved (%.0f s)"), stats.seconds));
}

std::wstring App::ResolveOutputFolder(std::string* configured, const std::wstring& fallback) {
  std::wstring folder = configured->empty() ? fallback : ToWide(*configured);
  while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();

  // Deleting the folder between two recordings is normal housekeeping, so the
  // first answer is simply to make it again.
  if (EnsureFolder(folder)) return folder;

  // It cannot be created either -- an unplugged drive, a path that is no longer
  // writable. Rather than refuse, fall back to the default and clear the custom
  // path so the settings show where the files are actually going now.
  CAP_WARN("Ordner nicht verfügbar: %s", ToUtf8(folder).c_str());
  if (!configured->empty() && EnsureFolder(fallback)) {
    Toast(T("Ordner nicht verfügbar, Standardordner wird benutzt.",
            "Folder not available, using the default folder."));
    configured->clear();
    settings_.InvalidateFolderFields();
    return fallback;
  }
  return {};
}

void App::SyncMicrophone(bool aboutToRecord) {
  const AudioSettings& a = config_.active().audio;

  // Held open only while it is actually being used: during a recording, and
  // while the settings are open so the level meter means something. Keeping a
  // microphone open the rest of the time would light up the Windows privacy
  // indicator for no reason and tell the user something untrue.
  const bool wanted =
      a.micEnabled && (aboutToRecord || recorder_.recording() || settings_.isOpen());

  // Changing the device is a fresh start, and clears a previous failure.
  if (!(micApplied_ == a.micDevice)) {
    micApplied_ = a.micDevice;
    micAttempted_ = false;
    micFailed_ = false;
    if (mic_.running()) mic_.Stop();
  }

  if (wanted && !mic_.running() && !micFailed_) {
    std::string error;
    micAttempted_ = true;
    if (!mic_.Start(a.micDevice, &error)) {
      // Not fatal: the recording still happens, just without the second track.
      // Latched, because retrying every frame would fill the log and the screen
      // with the same message sixty times a second.
      micFailed_ = true;
      if (!error.empty()) Toast(error);
    }
  } else if (!wanted && mic_.running()) {
    mic_.Stop();
    micAttempted_ = false;
  }
  mic_.SetGain(a.micGain);
}

void App::OpenFolderInExplorer(std::string* configured, const std::wstring& fallback) {
  const std::wstring folder = ResolveOutputFolder(configured, fallback);
  if (folder.empty()) {
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    return;
  }
  ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::DrawToolbarStrip() {
  ToolbarState state;
  state.recording = recorder_.recording();
  state.recordSeconds = state.recording ? recorder_.stats().seconds : 0.0;
  state.muted = config_.active().audio.mute;
  state.volume = config_.active().audio.volume;
  state.canRecord = captureState_ == CaptureState::Running && renderer_.hasFrame();

  const ToolbarResult result = DrawToolbar(state, config_.app.accentColor);
  if (result.volume >= 0.0f) {
    config_.active().audio.volume = result.volume;
    if (config_.active().audio.mute) config_.active().audio.mute = false;
    audio_.ApplySettings(config_.active().audio);
    ShowVolumeOsd();
  }

  switch (result.action) {
    case ToolbarAction::ToggleRecording: ToggleRecording(); break;
    case ToolbarAction::Screenshot: RequestScreenshot(); break;
    case ToolbarAction::Settings: OpenSettings({}); break;
    case ToolbarAction::OpenRecordFolder:
      OpenFolderInExplorer(&config_.record.outputFolder, DefaultRecordFolder());
      break;
    case ToolbarAction::OpenScreenshotFolder:
      OpenFolderInExplorer(&config_.record.screenshotFolder, DefaultScreenshotFolder());
      break;
    case ToolbarAction::ToggleMute: ToggleMute(); break;
    case ToolbarAction::Hide: config_.app.showToolbar = false; break;
    default: break;
  }
}

void App::WriteScreenshot() {
  std::vector<uint8_t> pixels;
  int width = 0, height = 0;
  if (!renderer_.GrabStill(&pixels, &width, &height)) {
    Toast(T("Kein Bild zum Speichern.", "No picture to save."));
    return;
  }

  RecordSettings& rec = config_.record;
  const std::wstring folder =
      ResolveOutputFolder(&rec.screenshotFolder, DefaultScreenshotFolder());
  const std::wstring path =
      folder.empty() ? std::wstring() : MakeScreenshotPath(folder, rec.screenshotFormat);
  if (path.empty()) {
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    CAP_ERR("Screenshot: Ordner nicht verfügbar: %s", ToUtf8(folder).c_str());
    return;
  }

  std::string error;
  if (!SaveScreenshot(path, pixels.data(), width, height, rec.screenshotFormat, rec.jpegQuality,
                      &error)) {
    Toast(T("Screenshot fehlgeschlagen: ", "Screenshot failed: ") + error);
    CAP_ERR("Screenshot fehlgeschlagen: %s", error.c_str());
    return;
  }

  // The file name, not the whole path: the path is long, and the point of the
  // message is "it worked and it is called this".
  const size_t slash = path.find_last_of(L'\\');
  Toast(T("Screenshot: ", "Screenshot: ") +
        ToUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1)));
  CAP_LOG("Screenshot gespeichert: %s (%dx%d)", ToUtf8(path).c_str(), width, height);
}

// ------------------------------------------------------------- crop picker
//
// Typing four numbers and checking the result is a loop nobody enjoys, so the
// edges can be dragged on the picture instead. While picking, the crop is set
// to zero so the whole frame is visible -- otherwise you would be cropping an
// already cropped image and the numbers would compound.

// How long the lock has to be missing before anything is changed, and how long
// a freshly set standard is given to prove itself. Both err on the generous
// side: a console being switched between 50 and 60 Hz drops out for a moment on
// its own, and reacting to that would mean changing the card's setting every
// time somebody opens a menu.
static const double kStandardLostSeconds = 1.5;
static const double kStandardSettleSeconds = 0.6;
// After a full pass with nothing locking, there is probably no signal at all --
// the console is off. Stop poking the card and look again in a while.
static const double kStandardBackoffSeconds = 6.0;

void App::StartSignalWatch() {
  StopSignalWatch();
  IBaseFilter* filter = capture_.filter();
  if (!filter) return;

  // The thread keeps its own reference, so tearing the graph down does not pull
  // the object out from under it; the worst that happens is that it asks a
  // filter that is no longer running, and gets told so.
  ComPtr<IBaseFilter> held = filter;
  signalWatchRun_.store(true, std::memory_order_relaxed);
  signalWatch_ = std::thread([this, held]() {
    // The same apartment the graph lives in, so the pointer is usable as it is.
    ComScope com(COINIT_MULTITHREADED);
    while (signalWatchRun_.load(std::memory_order_relaxed)) {
      signalLocked_.store(VideoStandardLocked(held.Get()), std::memory_order_relaxed);
      signalSeq_.fetch_add(1, std::memory_order_release);
      // Split into short naps so shutdown does not have to wait a quarter second.
      for (int i = 0; i < 25 && signalWatchRun_.load(std::memory_order_relaxed); ++i) {
        ::Sleep(10);
      }
    }
  });
}

void App::StopSignalWatch() {
  signalWatchRun_.store(false, std::memory_order_relaxed);
  if (signalWatch_.joinable()) signalWatch_.join();
  signalLocked_.store(-1, std::memory_order_relaxed);
}

int App::PollSignalLocked() {
  if (!capture_.running()) return -1;
  return signalLocked_.load(std::memory_order_relaxed);
}

void App::UpdateVideoStandard() {
  const Profile& profile = config_.active();
  if (profile.capture.videoStandard != -1) return;  // not our job
  if (captureState_ != CaptureState::Running) return;

  // Two fresh readings since the standard was last changed, so what is being
  // judged is the standard that is actually set.
  if (signalSeq_.load(std::memory_order_acquire) - standardSeqAtSet_ < 2) return;

  const int locked = signalLocked_.load(std::memory_order_relaxed);
  if (locked < 0) return;  // no analogue decoder, or nothing measured yet

  const int64_t now = QpcNow();

  if (locked == 1) {
    // Settled. Whatever is set is right, and the search starts from scratch if
    // it is ever needed again.
    standardLostQpc_ = 0;
    standardSweeps_ = 0;
    if (standardCandidate_ >= 0) {
      // A candidate just proved itself. The line count may have changed with
      // it, so the graph has to be rebuilt around the new format.
      standardCandidate_ = -1;
      const long settled = capture_.currentStandard();
      CAP_LOG("Videonorm automatisch gefunden: %s",
              VideoStandardName(VideoStandardIndexOf(settled)));
      std::string error;
      if (StartCapture(&error)) {
        Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                     VideoStandardName(VideoStandardIndexOf(settled))));
      } else {
        Toast(error);
      }
    }
    return;
  }

  // No lock.
  if (standardLostQpc_ == 0) {
    standardLostQpc_ = now;
    return;
  }
  if (QpcToSeconds(now - standardLostQpc_) < kStandardLostSeconds) return;
  if (now < standardNextTryQpc_) return;

  const std::vector<long> candidates =
      AutoStandardCandidates(capture_.capabilities().availableStandards);
  if (candidates.empty()) return;

  if (standardSweeps_ >= 1 && standardCandidate_ < 0) {
    // Backing off after a fruitless pass.
    standardSweeps_ = 0;
    standardNextTryQpc_ = now + SecondsToQpc(kStandardBackoffSeconds);
    return;
  }

  standardCandidate_ = (standardCandidate_ + 1) % (int)candidates.size();
  if (standardCandidate_ == 0) ++standardSweeps_;

  const long next = candidates[(size_t)standardCandidate_];
  capture_.SetStandard(next);
  standardSeqAtSet_ = signalSeq_.load(std::memory_order_acquire);
  // Setting it is not the same as it working. Give the decoder a moment, then
  // this function will look at the lock again and either keep it or move on.
  standardNextTryQpc_ = now + SecondsToQpc(kStandardSettleSeconds);
}

bool App::DrawSettingsWindowed() {
  const bool wanted = config_.app.settingsSeparateWindow;

  // Nothing to do, and nothing built: the common case, and it costs one branch.
  if (!wanted && !settingsHost_.created()) return false;

  if (wanted && !settingsHost_.created()) {
    std::string error;
    // The font atlas is shared rather than rebuilt -- same glyphs, and one copy
    // on the GPU is enough for both windows.
    if (!settingsHost_.Create(instance_, hwnd_, d3d_.device(), d3d_.context(),
                              ImGui::GetIO().Fonts, uiScale_, &error)) {
      CAP_WARN("%s", error.c_str());
      config_.app.settingsSeparateWindow = false;
      Toast(error);
      return false;
    }
    settingsHost_.ApplyTheme(darkMode_, config_.app.accentColor);
    // While its window is being dragged, Windows keeps the loop to itself. The
    // timer inside that loop is what still lets the picture run.
    settingsHost_.SetFrameCallback([this]() {
      if (inModalFrame_) return;
      inModalFrame_ = true;
      Tick();
      RenderFrame();
      inModalFrame_ = false;
    });
  }

  if (!wanted) {
    // Switched off again: put the panel back inside the picture.
    settingsHost_.Destroy();
    return false;
  }

  // Closing the window is closing the settings, the same as the button is.
  if (settingsHost_.takeCloseRequest()) settings_.Close();

  if (settings_.isOpen() != settingsHost_.visible()) {
    if (settings_.isOpen()) {
      settingsHost_.Show(ToWide(T("CapView – Einstellungen", "CapView – Settings")));
    } else {
      settingsHost_.Hide();
    }
  }

  if (!settingsHost_.BeginFrame(darkMode_, config_.app.accentColor)) return settings_.isOpen();

  settings_.SetFillsWindow(true);
  const SettingsWindow::Result result =
      settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr, &ffmpeg_);
  settingsHost_.EndFrame();

  // The host left the device pointing at its own back buffer; the main window is
  // still in the middle of its frame and needs it back.
  ID3D11RenderTargetView* backbuffer[] = {d3d_.rtv()};
  d3d_.context()->OMSetRenderTargets(1, backbuffer, nullptr);
  D3D11_VIEWPORT vp = {};
  vp.Width = (float)d3d_.width();
  vp.Height = (float)d3d_.height();
  vp.MaxDepth = 1.0f;
  d3d_.context()->RSSetViewports(1, &vp);

  // Closing is closing, whether it was the footer button or the window's own.
  if (result == SettingsWindow::Result::Close) settings_.Close();
  return true;
}

void App::OpenDeviceConfig() {
  if (!capture_.running()) {
    Toast(T("Die Karte läuft nicht.", "The card is not running."));
    return;
  }
  const std::wstring title = ToWide(config_.active().capture.video.name);
  std::string error;
  if (!devicePages_.Open(capture_.filter(), title, &error)) {
    Toast(error.empty() ? T("Der Konfigurationsdialog ließ sich nicht öffnen.",
                            "The configuration dialog could not be opened.")
                        : error);
  }
}

void App::DetectCrop() {
  int left = 0, top = 0, right = 0, bottom = 0;
  if (!renderer_.contentBounds(&left, &top, &right, &bottom)) {
    Toast(T("Noch nichts gemessen. Einen Moment warten.",
            "Nothing measured yet. Give it a moment."));
    return;
  }
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid()) return;

  ImageSettings& img = config_.active().image;
  const int cl = left;
  const int ct = top;
  const int cr = format.width - 1 - right;
  const int cb = format.height - 1 - bottom;
  // A border of a pixel or two is measurement noise on an analogue input, not a
  // border, and cropping it would only cost resolution.
  const int floorPx = 3;
  img.cropLeft = cl >= floorPx ? cl : 0;
  img.cropTop = ct >= floorPx ? ct : 0;
  img.cropRight = cr >= floorPx ? cr : 0;
  img.cropBottom = cb >= floorPx ? cb : 0;

  if (img.cropLeft || img.cropRight || img.cropTop || img.cropBottom) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  T("Rand erkannt: links %d, rechts %d, oben %d, unten %d",
                    "Border found: left %d, right %d, top %d, bottom %d"),
                  img.cropLeft, img.cropRight, img.cropTop, img.cropBottom);
    Toast(text);
  } else {
    Toast(T("Kein schwarzer Rand gefunden.", "No black border found."));
  }
}

bool App::SourceLooksInterlaced(const Profile& profile) const {
  if (!profile.image.deinterlaceAuto) return true;
  // The media type is believed when it claims interlaced -- a card that bothers
  // to say so is right. It is not believed when it stays quiet, which is the
  // usual case on an analogue input and is why the picture is measured as well.
  if (renderer_.sourceFormat().interlaced) return true;
  return renderer_.detectedInterlace() == VideoRenderer::InterlaceVerdict::Interlaced;
}

void App::BeginCropPick() {
  if (cropPick_.active) return;
  const VideoFormatInfo format = renderer_.sourceFormat();
  if (!format.valid() || !renderer_.hasFrame()) {
    Toast(T("Kein Bild zum Zuschneiden.", "No picture to crop."));
    return;
  }

  ImageSettings& img = config_.active().image;
  cropPick_.saved = img;
  cropPick_.left = img.cropLeft;
  cropPick_.right = img.cropRight;
  cropPick_.top = img.cropTop;
  cropPick_.bottom = img.cropBottom;
  cropPick_.drag = -1;
  cropPick_.active = true;

  // Show the full frame underneath, so screen position maps straight to source
  // pixels and the handles start where the current crop is.
  img.cropLeft = img.cropRight = img.cropTop = img.cropBottom = 0;
  settings_.Close();
}

void App::EndCropPick(bool apply) {
  if (!cropPick_.active) return;
  ImageSettings& img = config_.active().image;
  img = cropPick_.saved;
  if (apply) {
    img.cropLeft = cropPick_.left;
    img.cropRight = cropPick_.right;
    img.cropTop = cropPick_.top;
    img.cropBottom = cropPick_.bottom;
    CAP_LOG("Zuschnitt gesetzt: links %d, rechts %d, oben %d, unten %d", img.cropLeft,
            img.cropRight, img.cropTop, img.cropBottom);
  }
  cropPick_.active = false;
  OpenSettings({});
}

void App::DrawCropPicker() {
  const VideoFormatInfo format = renderer_.sourceFormat();
  const RECT& r = renderer_.videoRect();
  const float rw = (float)(r.right - r.left);
  const float rh = (float)(r.bottom - r.top);
  if (!format.valid() || rw < 8.0f || rh < 8.0f) {
    EndCropPick(false);
    return;
  }

  const float srcW = (float)format.width;
  const float srcH = (float)format.height;
  const float scaleX = rw / srcW;
  const float scaleY = rh / srcH;

  // Source pixels <-> client pixels.
  auto toScreenX = [&](int src) { return (float)r.left + (float)src * scaleX; };
  auto toScreenY = [&](int src) { return (float)r.top + (float)src * scaleY; };
  auto toSrcX = [&](float screen) { return (int)std::lround((screen - (float)r.left) / scaleX); };
  auto toSrcY = [&](float screen) { return (int)std::lround((screen - (float)r.top) / scaleY); };

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 mouse = ImGui::GetMousePos();

  float xL = toScreenX(cropPick_.left);
  float xR = toScreenX(format.width - cropPick_.right);
  float yT = toScreenY(cropPick_.top);
  float yB = toScreenY(format.height - cropPick_.bottom);

  // ---- grab handling ----
  // Everything outside the picture is ignored, so dragging the window or using
  // the buttons above still works.
  const float grab = 10.0f;
  const bool overVideo = mouse.x >= r.left - grab && mouse.x <= r.right + grab &&
                         mouse.y >= r.top - grab && mouse.y <= r.bottom + grab;

  int hot = -1;
  if (cropPick_.drag >= 0) {
    hot = cropPick_.drag;
  } else if (overVideo && !ImGui::GetIO().WantCaptureMouse) {
    float best = grab;
    if (std::abs(mouse.x - xL) < best) { best = std::abs(mouse.x - xL); hot = 0; }
    if (std::abs(mouse.x - xR) < best) { best = std::abs(mouse.x - xR); hot = 1; }
    if (std::abs(mouse.y - yT) < best) { best = std::abs(mouse.y - yT); hot = 2; }
    if (std::abs(mouse.y - yB) < best) { best = std::abs(mouse.y - yB); hot = 3; }
  }

  if (hot >= 0) {
    ImGui::SetMouseCursor(hot < 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  }
  if (hot >= 0 && cropPick_.drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    cropPick_.drag = hot;
  }
  if (cropPick_.drag >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    cropPick_.drag = -1;
  }

  if (cropPick_.drag >= 0) {
    // At least sixteen source pixels have to survive in each direction --
    // a zero sized picture is not a crop, it is a crash waiting to happen.
    const int minKeep = 16;
    switch (cropPick_.drag) {
      case 0:
        cropPick_.left = Clamp(toSrcX(mouse.x), 0, format.width - cropPick_.right - minKeep);
        break;
      case 1:
        cropPick_.right =
            Clamp(format.width - toSrcX(mouse.x), 0, format.width - cropPick_.left - minKeep);
        break;
      case 2:
        cropPick_.top = Clamp(toSrcY(mouse.y), 0, format.height - cropPick_.bottom - minKeep);
        break;
      case 3:
        cropPick_.bottom =
            Clamp(format.height - toSrcY(mouse.y), 0, format.height - cropPick_.top - minKeep);
        break;
      default: break;
    }
    xL = toScreenX(cropPick_.left);
    xR = toScreenX(format.width - cropPick_.right);
    yT = toScreenY(cropPick_.top);
    yB = toScreenY(format.height - cropPick_.bottom);
  }

  // ---- painting ----
  const ImU32 dim = IM_COL32(0, 0, 0, 150);
  dl->AddRectFilled(ImVec2((float)r.left, (float)r.top), ImVec2(xL, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xR, (float)r.top), ImVec2((float)r.right, (float)r.bottom), dim);
  dl->AddRectFilled(ImVec2(xL, (float)r.top), ImVec2(xR, yT), dim);
  dl->AddRectFilled(ImVec2(xL, yB), ImVec2(xR, (float)r.bottom), dim);

  const ImU32 line = IM_COL32(255, 255, 255, 230);
  const ImU32 lineHot = IM_COL32(255, 200, 80, 255);
  dl->AddRect(ImVec2(xL, yT), ImVec2(xR, yB), line, 0.0f, 0, 1.5f);

  // A thicker bar on each edge, so there is something obvious to aim at.
  const float bar = 4.0f;
  dl->AddRectFilled(ImVec2(xL - bar * 0.5f, yT), ImVec2(xL + bar * 0.5f, yB),
                    hot == 0 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xR - bar * 0.5f, yT), ImVec2(xR + bar * 0.5f, yB),
                    hot == 1 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yT - bar * 0.5f), ImVec2(xR, yT + bar * 0.5f),
                    hot == 2 ? lineHot : line);
  dl->AddRectFilled(ImVec2(xL, yB - bar * 0.5f), ImVec2(xR, yB + bar * 0.5f),
                    hot == 3 ? lineHot : line);

  // ---- toolbar ----
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 18.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (ImGui::Begin("##croptools", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::TextUnformatted(T("Ränder ziehen", "Drag the edges"));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("links %d  rechts %d  oben %d  unten %d", "left %d  right %d  top %d  bottom %d"),
                cropPick_.left, cropPick_.right, cropPick_.top, cropPick_.bottom);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text(T("Ergebnis %dx%d", "Result %dx%d"),
                format.width - cropPick_.left - cropPick_.right,
                format.height - cropPick_.top - cropPick_.bottom);

    ImGui::SameLine();
    if (ImGui::Button(T("Übernehmen", "Apply"))) {
      EndCropPick(true);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abbrechen", "Cancel"))) {
      EndCropPick(false);
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Nichts##crop", "None##crop"))) {
      cropPick_.left = cropPick_.right = cropPick_.top = cropPick_.bottom = 0;
    }
  }
  ImGui::End();
}

void App::FeedRecorder() {
  if (!recorder_.recording()) return;

  // Splitting is a restart, not a seamless cut: ffmpeg has to close the
  // container it is writing. A fraction of a second is lost at the boundary,
  // which is why this is off unless someone really is on FAT32.
  if (config_.record.splitFiles) {
    const double now = ImGui::GetTime();
    if (now - lastSplitCheck_ > 1.0) {
      lastSplitCheck_ = now;
      const uint64_t limit = (uint64_t)config_.record.splitSizeMb * 1024ull * 1024ull;
      if (recorder_.outputFileSize() >= limit) {
        CAP_LOG("Aufnahme: Größenlimit erreicht, neue Datei");
        StopRecording();
        StartRecording();
        return;
      }
    }
  }

  // ffmpeg died on its own: stop cleanly rather than filling a dead pipe.
  if (recorder_.failed()) {
    const RecordStats stats = recorder_.stats();
    StopRecording();
    Toast(stats.error.empty() ? T("Aufnahme abgebrochen.", "Recording aborted.") : stats.error);
    return;
  }

  VideoRenderer::ReadbackFrame frame;
  if (renderer_.FetchReadback(&frame)) {
    recorder_.PushVideo(frame.data, frame.stride);
    renderer_.ReleaseReadback();
  }
}

void App::ShowVolumeOsd() {
  // The readout replaces a toast here: a number plus a bar says more than a
  // line of text, and it is what you want to see while a game is running.
  if (config_.app.showVolumeOsd) {
    volumeOsdStart_ = ImGui::GetTime();
  } else {
    const AudioSettings& a = config_.active().audio;
    Toast(a.mute ? T("Stumm", "Muted")
                 : Format(T("Lautstärke %.0f %%", "Volume %.0f %%"), a.volume * 100.0f));
  }
}

// ------------------------------------------------------------------ main loop

int App::Run() {
  MSG msg = {};
  while (running_) {
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running_ = false;
        break;
      }
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    if (!running_) break;

    Tick();

    if (minimized_) {
      // Nothing to draw; block on messages so we use no CPU at all.
      ::WaitMessage();
      continue;
    }

    RenderFrame();

    // Wait for the next captured frame, a pending second field, or input.
    DWORD timeout = settings_.isOpen() ? 8 : 100;
    if (secondFieldPending_) {
      // Rounded up, not truncated. Truncating asks to be woken a fraction of a
      // millisecond before the field is due, at which point the loop finds it is
      // not due yet, redraws the same field for nothing and then spins on a zero
      // timeout until it is. Waiting the extra millisecond costs a millisecond
      // and saves all of that.
      const double waitMs = QpcToSeconds(secondFieldQpc_ - QpcNow()) * 1000.0;
      timeout = (DWORD)Clamp((int)std::ceil(waitMs), 0, (int)timeout);
    }
    HANDLE waits[1] = {nullptr};
    DWORD waitCount = 0;
    if (capture_.sink() && capture_.sink()->frameEvent()) {
      waits[0] = capture_.sink()->frameEvent();
      waitCount = 1;
    }
    ::MsgWaitForMultipleObjectsEx(waitCount, waitCount ? waits : nullptr, timeout, QS_ALLINPUT,
                                  MWMO_INPUTAVAILABLE);
  }
  return 0;
}

void App::Tick() {
  UpdatePowerRequest();
  CollectEncoderProbe();
  UpdateVideoStandard();

  if (captureState_ == CaptureState::Running) {
    std::string message;
    if (capture_.PumpEvents(&message)) {
      CAP_WARN("Capture unterbrochen: %s", message.c_str());
      captureError_ = message;
      captureState_ = CaptureState::Reconnecting;
      capture_.Stop();
      audio_.Stop();
      nextRetryQpc_ = QpcNow() + SecondsToQpc(kRetrySeconds);
      retryCount_ = 0;
    }
  }

  // While the settings dialog is open the user is presumably fixing exactly
  // this, so retrying behind their back only produces noise and blocks the
  // device they are about to pick.
  if (captureState_ == CaptureState::Reconnecting && !settings_.isOpen() &&
      QpcNow() >= nextRetryQpc_) {
    ++retryCount_;
    std::string error;
    if (StartCapture(&error)) {
      CAP_LOG("Wieder verbunden nach %d Versuchen", retryCount_);
      Toast("Wieder verbunden");
    } else {
      captureError_ = error;
      // Back off once it is clear this is not a brief hiccup.
      const double wait = retryCount_ >= kRetryBackoffAfter ? kRetrySlowSeconds : kRetrySeconds;
      nextRetryQpc_ = QpcNow() + SecondsToQpc(wait);
    }
  }

  if (audio_.failed()) {
    CAP_WARN("Audio ausgefallen, starte neu");
    StartAudio();
  }

  // Hide the pointer once it has been still for a while in fullscreen.
  if (fullscreen_ && config_.app.hideCursorFullscreen && !settings_.isOpen()) {
    if (!cursorHidden_ && QpcToSeconds(QpcNow() - lastMouseMoveQpc_) > kCursorIdleSeconds) {
      ::ShowCursor(FALSE);
      cursorHidden_ = true;
    }
  } else if (cursorHidden_) {
    ::ShowCursor(TRUE);
    cursorHidden_ = false;
  }
}

void App::RenderFrame() {
  const int64_t now = QpcNow();
  const Profile& profile = config_.active();

  // ---- pull the newest frame ----
  FrameSink* sink = capture_.sink();
  bool haveNewFrame = false;
  if (sink) {
    FrameView view;
    if (sink->AcquireFrame(&view) && view.valid()) {
      if (!sawFirstFrame_) {
        sawFirstFrame_ = true;
        CAP_LOG("Erstes Bild nach %.0f ms empfangen (%zu Byte)",
                QpcToSeconds(now - captureStartQpc_) * 1000.0, view.size);
      }
      renderer_.SetSourceFormat(sink->format(), nullptr);
      if (delayLine_.active()) {
        delayLine_.Push(view, now);
      } else {
        renderer_.UploadFrame(view);
        haveNewFrame = true;
      }
    }
    if (delayLine_.active()) {
      FrameView delayed;
      if (delayLine_.Pop(&delayed, now)) {
        renderer_.UploadFrame(delayed);
        haveNewFrame = true;
      }
    }
  }

  // ---- bob deinterlacing: second field of the previous frame ----
  const VideoFormatInfo format = renderer_.sourceFormat();
  const bool deinterlacing =
      profile.image.deinterlace != Deinterlace::Off && SourceLooksInterlaced(profile);

  // Which field is the earlier one. The media type is asked first and is usually
  // silent on an analogue card, in which case top-field-first is the convention
  // for standard definition -- but a wrong guess here does not soften the
  // picture, it makes it jump: the two fields are shown in the wrong order, so
  // every frame steps back half a frame and then forward again. Hence the
  // override in the settings, which is the only reliable fix when the card says
  // nothing.
  int firstField = format.fieldOneFirst ? 0 : 1;
  if (profile.image.fieldOrder == FieldOrder::TopFirst) firstField = 0;
  else if (profile.image.fieldOrder == FieldOrder::BottomFirst) firstField = 1;

  if (haveNewFrame) {
    fieldIndex_ = firstField;
    if (deinterlacing) ++fieldsShown_[0];
    if (deinterlacing) {
      // The announced rate and the delivered rate are not always the same
      // number. This card announces 50 fps and hands over 25 woven frames a
      // second -- half the rate, twice the content per frame. Splitting the
      // announced interval would put the second field on screen 10 ms in and
      // then leave it there for 30, which judders harder than not deinterlacing
      // at all, so the measured arrival rate wins whenever there is one.
      double frameSeconds = format.fps > 1.0 ? 1.0 / format.fps : 1.0 / 60.0;
      const double measured = sink ? sink->stats().sourceFps : 0.0;
      if (measured > 1.0) frameSeconds = 1.0 / measured;

      // Counted from when the frame arrived, not from when we got round to
      // drawing it. Those are not the same instant: the log has shown the
      // displayed frame to be twenty milliseconds old, and half a frame after
      // *that* falls past the arrival of the next one -- at which point the
      // second field is never shown at all. Half the frames then get both
      // fields and half get one, which is exactly what a juddering picture is.
      const int64_t arrival = sink && sink->lastArrivalQpc() != 0 ? sink->lastArrivalQpc() : now;
      const int64_t period = SecondsToQpc(frameSeconds);

      // The card delivers when the driver gets round to it -- the graph runs
      // without a reference clock on purpose -- and measured here the arrivals
      // wander by ten milliseconds either way. Half a frame after each raw
      // arrival therefore lands anywhere, and bob ends up showing one field for
      // five milliseconds and the next for twenty-eight. The average is exactly
      // right and the picture stutters anyway.
      //
      // So the next arrival is predicted from a phase that is nudged towards the
      // arrivals rather than following each one, and the second field is put
      // halfway between the frame we have and the frame we expect. A late frame
      // then shortens both of its fields equally instead of crushing one of them.
      if (framePhaseQpc_ == 0 || std::llabs(arrival - framePhaseQpc_) > period) {
        framePhaseQpc_ = arrival;  // first frame, or the stream jumped
      } else {
        framePhaseQpc_ += (arrival - framePhaseQpc_) / 8;
      }
      const int64_t expectedNext = framePhaseQpc_ + period;
      framePhaseQpc_ = expectedNext;

      int64_t gap = (expectedNext - arrival) / 2;
      const int64_t minGap = period / 4;
      const int64_t maxGap = period * 3 / 4;
      if (gap < minGap) gap = minGap;
      if (gap > maxGap) gap = maxGap;
      secondFieldQpc_ = arrival + gap;
      secondFieldPending_ = true;
    } else {
      secondFieldPending_ = false;
    }
  } else if (secondFieldPending_ && now >= secondFieldQpc_) {
    fieldIndex_ = 1 - firstField;
    secondFieldPending_ = false;
    ++fieldsShown_[1];
  }

  // ---- draw ----
  float clear[4];
  GetBackgroundColor(darkMode_, config_.app.accentColor, clear);
  if (!d3d_.BeginFrame(clear)) return;

  // Decided before drawing, so the picture is laid out around the bar in the
  // same frame the bar appears in.
  renderer_.SetTopInset(toolbarVisible_ ? (int)std::lround(ToolbarHeight()) : 0);
  renderer_.Draw(profile.image, fieldIndex_);

  // Right after the first pass, so the still is the picture that was just put on
  // screen -- and before the UI is drawn, so the overlay never lands in it.
  if (screenshotPending_) {
    screenshotPending_ = false;
    WriteScreenshot();
  }

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  DrawUi();
  ImGui::Render();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

  if (sink) lastFrameAgeMs_ = sink->stats().lastArrivalAgeMs;
  SyncMicrophone();
  FeedRecorder();
  d3d_.EndFrame(config_.app.vsync);

  // ---- present rate ----
  ++presentCount_;
  if (fpsWindowQpc_ == 0) fpsWindowQpc_ = now;
  const double elapsed = QpcToSeconds(now - fpsWindowQpc_);
  if (elapsed >= 1.0) {
    presentFps_ = presentCount_ / elapsed;
    presentCount_ = 0;
    fpsWindowQpc_ = now;

    // With logging on, write a line every few seconds. This is what makes a
    // "it stutters" report actionable without having to reproduce it here.
    if (config_.app.logToFile && captureState_ == CaptureState::Running) {
      if (++statsLogCounter_ >= 5) {
        statsLogCounter_ = 0;
        const SinkStats sinkStats = sink ? sink->stats() : SinkStats{};
        const AudioStats audioStats = audio_.stats();
        CAP_LOG("Status: Quelle %.2f fps, Ausgabe %.1f fps, %llu angezeigt, %llu verworfen, "
                "Bildalter %.1f ms | Halbbilder %llu/%llu | Ton %.1f/%.0f ms, %llu leer, "
                "%llu übergelaufen",
                sinkStats.sourceFps, presentFps_, (unsigned long long)sinkStats.displayed,
                (unsigned long long)sinkStats.dropped, sinkStats.lastArrivalAgeMs,
                (unsigned long long)fieldsShown_[0], (unsigned long long)fieldsShown_[1],
                audioStats.bufferMs, audioStats.targetMs,
                (unsigned long long)audioStats.underruns,
                (unsigned long long)audioStats.overruns);
      }
    }
  }
}

void App::DrawUi() {
  const Profile& profile = config_.active();
  FrameSink* sink = capture_.sink();

  // ---- toolbar ----
  // Windowed it is simply there; in fullscreen it follows the pointer, which is
  // already hidden after a couple of seconds of play.
  toolbarVisible_ = config_.app.showToolbar && !cropPick_.active &&
                    (!fullscreen_ || !cursorHidden_);
  if (toolbarVisible_) {
    DrawToolbarStrip();
    // Everything else positions itself against the viewport work area, which is
    // exactly what it is for: the part of the window not taken by a bar. Moving
    // it here means the statistics, the toasts, the status card and the settings
    // dialog all keep clear of the toolbar without knowing it exists. ImGui
    // resets this at the start of every frame.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float reserved = ToolbarHeight();
    vp->WorkPos.y += reserved;
    vp->WorkSize.y -= reserved;
  }

  // ---- status card ----
  const bool haveSignal = sink && sink->HasRecentFrame(kNoSignalSeconds) && renderer_.hasFrame();
  if (!haveSignal) {
    if (captureState_ == CaptureState::Reconnecting) {
      DrawStatusCard(T("Verbindung unterbrochen", "Connection lost"),
                     captureError_.empty() ? std::string() : captureError_, true);
    } else if (captureState_ == CaptureState::Running) {
      DrawStatusCard(T("Kein Signal", "No signal"),
                     T("Die Karte läuft, liefert aber gerade kein Bild. Quelle "
                       "eingeschaltet? Richtiger Eingang gewählt?",
                       "The card is running but not delivering a picture. Is the source "
                       "on? Is the right input selected?"),
                     false);
    } else if (!settings_.isOpen()) {
      DrawStatusCard(T("Kein Gerät aktiv", "No device active"),
                     T("Rechtsklick oder F2 öffnet die Einstellungen.",
                       "Right-click or press F2 to open the settings."),
                     false);
    }
  }

  // ---- stats ----
  if (config_.app.showStats) {
    OverlayStats stats;
    stats.profileName = profile.name;
    stats.deviceName = capture_.resolvedDevice().name;
    const auto& inputs = capture_.capabilities().crossbarInputs;
    if (profile.capture.crossbarInput >= 0 &&
        profile.capture.crossbarInput < (int)inputs.size()) {
      stats.inputName = inputs[(size_t)profile.capture.crossbarInput].name;
    }
    stats.format = renderer_.sourceFormat();
    if (sink) stats.sink = sink->stats();
    stats.audio = audio_.stats();
    stats.presentFps = presentFps_;
    stats.frameAgeMs = lastFrameAgeMs_;
    stats.vsync = config_.app.vsync;
    stats.tearing = d3d_.tearingSupported();
    stats.deinterlacing =
        profile.image.deinterlace != Deinterlace::Off && SourceLooksInterlaced(profile);
    stats.deinterlaceLabel = renderer_.sourceCoSitedFields()
                                 ? T("deckungsgleich (240p/288p)", "aligned (240p/288p)")
                                 : DeinterlaceName((int)profile.image.deinterlace);
    const RECT& r = renderer_.videoRect();
    stats.displayWidth = (int)(r.right - r.left);
    stats.displayHeight = (int)(r.bottom - r.top);
    stats.filterName = ScaleFilterName((int)profile.image.filter);
    stats.videoDelayMs = (int)delayLine_.delayMs();
    stats.detail = config_.app.statsDetail;
    // Spell out what "automatic" resolved to, since that is the setting people
    // second-guess when a picture looks wrong.
    {
      std::string range = profile.image.range == ColorRange::Auto
                              ? (detectedRangeText_ ? detectedRangeText_
                                                    : T("wird gemessen", "measuring"))
                              : ColorRangeName((int)profile.image.range);
      std::string matrix = ColorMatrixName((int)profile.image.matrix);
      stats.colorInfo = range + "  /  " + matrix;
    }
    DrawStatsPanel(stats);
  }

  // ---- crop picker ----
  if (cropPick_.active) {
    DrawCropPicker();
    if (!cropPick_.active) return;  // Apply or Cancel closed it this frame
  }

  // ---- recording indicator ----
  if (recorder_.recording()) {
    DrawRecordIndicator(recorder_.stats().seconds, config_.app.osdCorner);
  }

  // ---- volume readout ----
  {
    const double age = ImGui::GetTime() - volumeOsdStart_;
    if (age < kVolumeOsdSeconds) {
      DrawVolumeOsd(profile.audio.volume, profile.audio.mute, config_.app.osdCorner, age,
                    kVolumeOsdSeconds);
    }
  }

  // ---- toast ----
  if (!toastText_.empty()) {
    const double age = ImGui::GetTime() - toastStart_;
    if (age > 2.5) {
      toastText_.clear();
    } else {
      DrawToast(toastText_, age, 2.5);
    }
  }

  DrawContextMenu();

  // ---- settings ----
  // The banner explains why the dialog opened by itself. Once the card is
  // actually running the reason is gone, whichever route got it there --
  // picking a device, F5, or the automatic retry. Leaving it up until the
  // dialog is closed and reopened reads like the selection did not take.
  if (captureState_ == CaptureState::Running) settings_.ClearReason();

  switch (renderer_.detectedRange()) {
    case VideoRenderer::RangeVerdict::Limited:
      detectedRangeText_ = T("begrenzt (16-235)", "limited (16-235)");
      break;
    case VideoRenderer::RangeVerdict::Full:
      detectedRangeText_ = T("voll (0-255)", "full (0-255)");
      break;
    default:
      detectedRangeText_ = nullptr;
      break;
  }
  settings_.SetDetectedRange(&detectedRangeText_);

  switch (renderer_.detectedInterlace()) {
    case VideoRenderer::InterlaceVerdict::Interlaced:
      detectedInterlaceText_ = renderer_.sourceCoSitedFields()
                                   ? T("interlaced, 240p/288p-Quelle",
                                       "interlaced, 240p/288p source")
                                   : T("interlaced", "interlaced");
      break;
    case VideoRenderer::InterlaceVerdict::Progressive:
      detectedInterlaceText_ = T("progressiv", "progressive");
      break;
    default:
      detectedInterlaceText_ = nullptr;
      break;
  }
  settings_.SetDetectedInterlace(&detectedInterlaceText_);
  settings_.SetCoSitedFields(renderer_.sourceCoSitedFields());
  settings_.SetSignalLocked(PollSignalLocked());
  settings_.SetProbeAllowed(captureState_ != CaptureState::Reconnecting);
  settings_.SetUpdater(&updater_);
  // Auto means: analogue when the card has a decoder for it. A card that only
  // does one of the two therefore needs nobody to say which.
  {
    const SignalKind kind = config_.active().capture.signalKind;
    const bool hasDecoder =
        capture_.running() && capture_.capabilities().availableStandards != 0;
    settings_.SetAnalogueSource(kind == SignalKind::Analog ||
                                (kind == SignalKind::Auto && hasDecoder));
  }
  settings_.SetSourceFps(renderer_.sourceFormat().fps);
  settings_.SetLevels(audio_.inputPeak(), mic_.peak(), mic_.running());
  if (settings_.takeCropPickRequest()) BeginCropPick();
  if (settings_.takeDeviceConfigRequest()) OpenDeviceConfig();
  if (settings_.takeCropDetectRequest()) DetectCrop();
  settings_.setProbeBusy(probing_.load(std::memory_order_relaxed));
  if (!DrawSettingsWindowed()) {
    settings_.SetFillsWindow(false);
    if (settings_.Draw(capture_.running() ? &capture_.capabilities() : nullptr, &ffmpeg_) ==
        SettingsWindow::Result::Close) {
      settings_.Close();
    }
  }
  if (settings_.takeProbeRequest()) StartEncoderProbe(true);
  if (settings_.takeRestartRequest()) {
    if (updater_.RestartIntoNewBuild()) {
      // The new build is coming up; this one gets out of its way so the window
      // position and the configuration are written before it reads them.
      running_ = false;
    } else {
      Toast(T("Neustart fehlgeschlagen.", "Restart failed."));
    }
  }

  // Everything above may have edited the configuration in place, so act on it
  // here in one spot rather than sprinkling apply calls through the UI code.
  SyncConfigChanges();
  MaybeSaveConfig();
}

void App::DrawContextMenu() {
  if (!ImGui::BeginPopupContextVoid("capview_context", ImGuiPopupFlags_MouseButtonRight)) return;

  // Shortcut labels come from the live bindings, so rebinding a key is visible
  // here immediately instead of leaving the menu quietly lying about it. The
  // strings have to outlive the frame, hence the static buffer per action.
  auto sc = [this](HotkeyAction action) -> const char* {
    static std::string text[(int)HotkeyAction::Count];
    const int i = (int)action;
    text[i] = config_.hotkeys[action].bound() ? HotkeyText(config_.hotkeys[action]) : std::string();
    return text[i].empty() ? nullptr : text[i].c_str();
  };

  if (ImGui::MenuItem(T("Einstellungen...", "Settings..."), sc(HotkeyAction::Settings))) OpenSettings({});
  ImGui::Separator();

  bool fs = fullscreen_;
  if (ImGui::MenuItem(T("Vollbild", "Fullscreen"), sc(HotkeyAction::Fullscreen), &fs)) SetFullscreen(fs);

  bool top = config_.app.alwaysOnTop;
  if (ImGui::MenuItem(T("Immer im Vordergrund", "Always on top"), nullptr, &top)) {
    config_.app.alwaysOnTop = top;
    ApplyWindowFlags();
  }

  bool stats = config_.app.showStats;
  if (ImGui::MenuItem(T("Statistik", "Statistics"), sc(HotkeyAction::Stats), &stats)) config_.app.showStats = stats;

  bool toolbar = config_.app.showToolbar;
  if (ImGui::MenuItem(T("Werkzeugleiste", "Toolbar"), nullptr, &toolbar)) {
    config_.app.showToolbar = toolbar;
  }

  // Same reasoning as the colour menu below, only more so: whether the standard
  // is right is something you see instantly, and on a console that switches
  // between 50 and 60 Hz it is the setting you reach for most.
  const long availableStandards =
      capture_.running() ? capture_.capabilities().availableStandards : 0;
  if (availableStandards != 0 && ImGui::BeginMenu(T("Videonorm", "Video standard"))) {
    CaptureSettings& cap = config_.active().capture;
    const long before = cap.videoStandard;

    if (ImGui::MenuItem(T("Automatisch", "Automatic"), nullptr, cap.videoStandard == -1)) {
      cap.videoStandard = -1;
    }
    if (ImGui::MenuItem(T("Nicht ändern", "Leave alone"), nullptr, cap.videoStandard == 0)) {
      cap.videoStandard = 0;
    }
    ImGui::Separator();
    for (int i = 0; i < VideoStandardCount(); ++i) {
      const long value = VideoStandardValue(i);
      if ((availableStandards & value) == 0) continue;
      if (ImGui::MenuItem(VideoStandardName(i), nullptr, cap.videoStandard == value)) {
        cap.videoStandard = value;
      }
    }

    if (cap.videoStandard != before) {
      // A different standard usually means a different number of lines, so the
      // graph has to come up again around the new format. Automatic is the one
      // case that does not restart here: it has nothing to apply yet and will
      // rebuild by itself once it has found something that locks.
      if (cap.videoStandard > 0) {
        std::string error;
        if (StartCapture(&error)) {
          Toast(Format(T("Videonorm: %s", "Video standard: %s"),
                       VideoStandardSettingName(cap.videoStandard).c_str()));
        } else {
          Toast(error);
        }
      }
      SaveConfig();
    }
    ImGui::EndMenu();
  }

  // Right here rather than buried in the dialog: wrong levels or a wrong matrix
  // are things you spot by looking at the picture, and both take effect on the
  // very next frame, so switching them while watching is the fastest way to
  // land on the right one.
  if (ImGui::BeginMenu(T("Farbe", "Colour"))) {
    ImageSettings& img = config_.active().image;

    ImGui::SeparatorText(T("Wertebereich", "Range"));
    for (int i = 0; i < 3; ++i) {
      // "##range" keeps the id unique: the first entry of both lists is called
      // "Automatic", and two menu items with the same label in one menu share an
      // id, which Dear ImGui reports as a programmer error.
      const std::string label = std::string(ColorRangeName(i)) + "##range";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.range == i)) {
        img.range = (ColorRange)i;
        Toast(std::string(T("Wertebereich: ", "Range: ")) + ColorRangeName(i));
      }
    }

    ImGui::SeparatorText(T("Farbmatrix", "Colour matrix"));
    for (int i = 0; i < 3; ++i) {
      const std::string label = std::string(ColorMatrixName(i)) + "##matrix";
      if (ImGui::MenuItem(label.c_str(), nullptr, (int)img.matrix == i)) {
        img.matrix = (ColorMatrix)i;
        Toast(std::string(T("Farbmatrix: ", "Colour matrix: ")) + ColorMatrixName(i));
      }
    }
    ImGui::EndMenu();
  }

  // Volume lives in the menu as well as on the wheel: the menu is where you
  // look when you cannot remember the shortcut.
  AudioSettings& audio = config_.active().audio;
  bool muted = audio.mute;
  if (ImGui::MenuItem(T("Stumm", "Muted"), sc(HotkeyAction::Mute), &muted)) ToggleMute();

  ImGui::SetNextItemWidth(180.0f);
  float percent = audio.volume * 100.0f;
  if (ImGui::SliderFloat(T("Lautstärke", "Volume"), &percent, 0.0f, 100.0f, "%.0f %%")) {
    audio.volume = Clamp(percent / 100.0f, 0.0f, 1.0f);
    audio.mute = false;
    audio_.ApplySettings(audio);
    applied_.volume = audio.volume;
    applied_.mute = audio.mute;
  }

  if (config_.profiles.size() > 1 && ImGui::BeginMenu(T("Profil", "Profile"))) {
    for (int i = 0; i < (int)config_.profiles.size(); ++i) {
      const bool selected = (i == config_.activeProfile);
      std::string shortcut = i < 9 ? Format(T("Strg+%d", "Ctrl+%d"), i + 1) : std::string();
      if (ImGui::MenuItem(config_.profiles[(size_t)i].name.c_str(),
                          shortcut.empty() ? nullptr : shortcut.c_str(), selected)) {
        SwitchProfile(i);
      }
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  {
    const bool rec = recorder_.recording();
    if (ImGui::MenuItem(rec ? T("Aufnahme stoppen", "Stop recording")
                            : T("Aufnahme starten", "Start recording"),
                        sc(HotkeyAction::Record))) {
      ToggleRecording();
    }
  }
  if (ImGui::MenuItem(T("Screenshot", "Screenshot"), sc(HotkeyAction::Screenshot))) RequestScreenshot();
  if (ImGui::MenuItem(T("Aufnahme neu starten", "Restart capture"), sc(HotkeyAction::RestartCapture))) RestartAll(true);
  if (ImGui::MenuItem(T("Beenden", "Quit"), "Alt+F4")) ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);

  ImGui::EndPopup();
}

// ------------------------------------------------------------------ messages

bool App::HandleKeyDown(WPARAM key) {
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;

  // Fixed on purpose, see hotkeys.h: a rebindable Escape is a way to lock
  // yourself into fullscreen, and the profile digits are a block, not a key.
  if (ctrl && key >= '1' && key <= '9') {
    SwitchProfile((int)(key - '1'));
    return true;
  }
  if (cropPick_.active) {
    if (key == VK_ESCAPE) {
      EndCropPick(false);
      return true;
    }
    if (key == VK_RETURN) {
      EndCropPick(true);
      return true;
    }
    return true;  // swallow everything else: one job at a time
  }

  if (key == VK_ESCAPE) {
    if (settings_.isOpen()) {
      settings_.Close();
    } else if (fullscreen_) {
      SetFullscreen(false);
    }
    return true;
  }

  switch (config_.hotkeys.Find((int)key, ctrl, shift, alt)) {
    case HotkeyAction::Fullscreen:
      ToggleFullscreen();
      return true;
    case HotkeyAction::Settings:
      if (settings_.isOpen()) {
        settings_.Close();
      } else {
        OpenSettings({});
      }
      return true;
    case HotkeyAction::Stats:
      config_.app.showStats = !config_.app.showStats;
      return true;
    case HotkeyAction::RestartCapture:
      RestartAll(true);
      return true;
    case HotkeyAction::Record:
      ToggleRecording();
      return true;
    case HotkeyAction::Screenshot:
      RequestScreenshot();
      return true;
    case HotkeyAction::Mute:
      ToggleMute();
      return true;
    case HotkeyAction::VolumeUp:
      AdjustVolume(kVolumeStep);
      return true;
    case HotkeyAction::VolumeDown:
      AdjustVolume(-kVolumeStep);
      return true;
    default:
      break;
  }

  // The numeric keypad follows the main volume keys without needing its own
  // binding -- nobody expects to have to bind both.
  if (key == VK_ADD && config_.hotkeys[HotkeyAction::VolumeUp].bound()) {
    AdjustVolume(kVolumeStep);
    return true;
  }
  if (key == VK_SUBTRACT && config_.hotkeys[HotkeyAction::VolumeDown].bound()) {
    AdjustVolume(-kVolumeStep);
    return true;
  }
  return false;
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (imguiReady_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;

  switch (msg) {
    case WM_ENTERSIZEMOVE:
      // Same as for the settings window: Windows takes the loop away while a
      // window is being dragged, and only a timer still gets through.
      ::SetTimer(hwnd, 1, 8, nullptr);
      return 0;
    case WM_EXITSIZEMOVE:
      ::KillTimer(hwnd, 1);
      return 0;
    case WM_TIMER:
      if (wparam == 1 && !inModalFrame_) {
        inModalFrame_ = true;
        Tick();
        RenderFrame();
        inModalFrame_ = false;
      }
      return 0;
    case WM_SIZE:
      minimized_ = (wparam == SIZE_MINIMIZED);
      if (!minimized_) d3d_.Resize();
      return 0;

    case WM_GETMINMAXINFO: {
      auto* info = (MINMAXINFO*)lparam;
      info->ptMinTrackSize.x = 320;
      info->ptMinTrackSize.y = 240;
      return 0;
    }

    case WM_MOUSEMOVE: {
      POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (p.x != lastMousePos_.x || p.y != lastMousePos_.y) {
        lastMousePos_ = p;
        lastMouseMoveQpc_ = QpcNow();
        if (cursorHidden_) {
          ::ShowCursor(TRUE);
          cursorHidden_ = false;
        }
      }
      return 0;
    }

    case WM_SETCURSOR:
      if (cursorHidden_ && LOWORD(lparam) == HTCLIENT) {
        ::SetCursor(nullptr);
        return 1;
      }
      break;

    case WM_MOUSEWHEEL:
      // Only over the picture: inside the settings window the wheel scrolls.
      if (config_.app.wheelVolume && imguiReady_ && !ImGui::GetIO().WantCaptureMouse) {
        const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        if (notches != 0) {
          AdjustVolume(kVolumeStep * (float)notches);
          return 0;
        }
      }
      break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // The binding editor wants the raw key, before anyone acts on it -- that
      // is the whole point of it being open.
      if (settings_.waitingForKey()) {
        settings_.OfferKey((int)wparam, (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
                           (::GetKeyState(VK_SHIFT) & 0x8000) != 0,
                           (::GetKeyState(VK_MENU) & 0x8000) != 0);
        return 0;
      }
      // While the settings window has the keyboard, let it type.
      if (!(imguiReady_ && ImGui::GetIO().WantCaptureKeyboard)) {
        lastKeyLParam_ = (uint64_t)lparam;
        if (HandleKeyDown(wparam)) return 0;
      } else if (wparam == VK_ESCAPE) {
        settings_.Close();
        return 0;
      }
      break;

    case WM_SYSCOMMAND:
      // Block the screensaver from starting over our window.
      if ((wparam & 0xFFF0) == SC_SCREENSAVE || (wparam & 0xFFF0) == SC_MONITORPOWER) {
        if (config_.app.preventSleep) return 0;
      }
      break;

    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
      settings_.InvalidateDeviceLists();
      break;

    case WM_SETTINGCHANGE:
      if (config_.app.theme == Theme::System) ApplyTheme();
      break;

    case WM_DPICHANGED: {
      auto* rc = (RECT*)lparam;
      ::SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                     rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }

    case WM_CLOSE:
      SaveConfig();
      running_ = false;
      ::PostQuitMessage(0);
      return 0;

    case WM_DESTROY:
      running_ = false;
      ::PostQuitMessage(0);
      return 0;

    default: break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace cap
